#include "CameraSession.h"
#include "platform/Platform.h"

#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "CrDeviceProperty.h"
#include "CrCommandData.h"
#include "CrImageDataBlock.h"

namespace SDK = SCRSDK;
using namespace std::chrono_literals;

namespace {

// SDK Sony отдаёт строки в wchar_t — это единственное место, где он тут нужен.
std::string wideToUtf8(const wchar_t* w) { return plat::utf8FromWide(w); }

constexpr std::uint32_t kIsoValueMask = 0x00FFFFFF;
constexpr std::uint32_t kIsoAutoValue = 0x00FFFFFF;   // value-part meaning AUTO
constexpr std::uint16_t kBatteryUntaken = 0xFFFF;
constexpr int           kMaxReadFail   = 3;           // failed polls in a row -> treat as offline

// ---- display labels for selectable properties ----
std::string apertureLabel(std::uint16_t f) {           // FNumber = F * 100
    if (f == 0 || f == 0xFFFE || f == 0xFFFF || f == 0xFFFD) return "";
    char buf[16];
    if (f % 100) std::snprintf(buf, sizeof(buf), "F%.1f", f / 100.0);
    else         std::snprintf(buf, sizeof(buf), "F%d", f / 100);
    return buf;
}
std::string shutterLabel(std::uint32_t s) {            // hi = numerator, lo = denominator
    if (s == 0) return "BULB";
    if (s == 0xFFFFFFFFu) return "";
    std::uint16_t num = static_cast<std::uint16_t>(s >> 16);
    std::uint16_t den = static_cast<std::uint16_t>(s & 0xFFFF);
    if (den == 0) return "";
    char buf[24];
    if (num == 1)                std::snprintf(buf, sizeof(buf), "1/%u", den);
    else if (num % den == 0)     std::snprintf(buf, sizeof(buf), "%u\"", num / den);
    else                         std::snprintf(buf, sizeof(buf), "%.1f\"", static_cast<double>(num) / den);
    return buf;
}
std::string wbLabel(std::uint16_t w) {                 // CrWhiteBalanceSetting
    switch (w) {
    case 0x0000: return "Авто";
    case 0x0001: return "Подводный авто";
    case 0x0011: return "Дневной свет";
    case 0x0012: return "Тень";
    case 0x0013: return "Облачно";
    case 0x0014: return "Лампа накаливания";
    case 0x0020: return "Флуоресц.";
    case 0x0021: return "Флуоресц. тёплый";
    case 0x0022: return "Флуоресц. холодный";
    case 0x0023: return "Флуоресц. дневн.-бел.";
    case 0x0024: return "Флуоресц. дневной";
    case 0x0030: return "Вспышка";
    case 0x0100: return "Цвет. темп-ра";
    case 0x0101: return "Пользоват. 1";
    case 0x0102: return "Пользоват. 2";
    case 0x0103: return "Пользоват. 3";
    case 0x0104: return "Пользоват.";
    default: { char b[16]; std::snprintf(b, sizeof(b), "WB 0x%X", w); return b; }
    }
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Диагностика в консоль (плат. слой печатает настоящим Unicode, независимо от
// кодовой страницы, которую сбрасывает SDK).
void clog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n) : sizeof(buf) - 1;
    plat::writeConsole(std::string(buf, len));
}

} // namespace

namespace coll {

CameraSession::CameraSession(int index, const SDK::ICrCameraObjectInfo* e)
    : m_index(index)
{
    // Deep-copy the enumerated info so the connection survives releasing the enum list.
    m_info = SDK::CreateCameraObjectInfo(
        e->GetName(),
        e->GetModel(),
        e->GetUsbPid(),
        e->GetIdType(),
        e->GetIdSize(),
        e->GetId(),
        e->GetConnectionTypeName(),
        e->GetAdaptorName(),
        e->GetPairingNecessity(),
        e->GetSSHsupport());
}

CameraSession::~CameraSession() {
    if (m_info) {
        m_info->Release();
        m_info = nullptr;
    }
}

std::string CameraSession::idLabel() const {
    return "CAM " + std::to_string(m_index);
}

std::string CameraSession::modelUtf8() const {
    return m_info ? wideToUtf8(m_info->GetModel()) : std::string();
}

std::string CameraSession::ipUtf8() const {
    return m_info ? wideToUtf8(m_info->GetIPAddressChar()) : std::string();
}

std::string CameraSession::macUtf8() const {
    return m_info ? wideToUtf8(m_info->GetMACAddressChar()) : std::string();
}

bool CameraSession::startConnect() {
    if (!m_info) return false;

    // Clean teardown of any previous handle BEFORE re-attempting, so we never
    // leave a half-open session on the camera (Sony allows only one PC Remote
    // client; a dangling session is what wedges a camera until it's rebooted).
    if (m_handle) {
        SDK::Disconnect(m_handle);
        std::this_thread::sleep_for(500ms);
        SDK::ReleaseDevice(m_handle);
        m_handle = 0;
        std::this_thread::sleep_for(300ms);   // let the camera clear the session
    }

    m_attemptMs.store(nowMs());
    // Non-SSH PC Remote connection. Reconnect ON so drops recover automatically.
    // userId "admin" mirrors RemoteCli's call; password/fingerprint stay empty.
    // pairingDisplayName defaults to the PC host name (shown in the camera dialog).
    auto st = SDK::Connect(m_info, this, &m_handle,
                           SDK::CrSdkControlMode_Remote,
                           SDK::CrReconnecting_ON,
                           "admin");
    if (CR_SUCCEEDED(st)) {
        m_attempting.store(true);
        clog("[CAM %d %s] подключаюсь... (если на камере появится запрос связывания — подтверди)\n",
             m_index, modelUtf8().c_str());
        return true;
    }
    m_attempting.store(false);
    clog("[CAM %d %s] Connect не запустился: 0x%08x\n",
         m_index, modelUtf8().c_str(), static_cast<unsigned>(st));
    return false;
}

bool CameraSession::maybeRetryConnect() {
    if (m_connected.load() || m_everConnected.load()) return false;  // connected (now/ever)
    const long long since = nowMs() - m_attemptMs.load();
    // A Connect is in flight (handshake / waiting for the pairing dialog): leave
    // it alone for a long while — re-issuing here is what causes camera wedges.
    if (m_attempting.load() && since < 45000) return false;
    // After a failure, back off briefly before trying again.
    if (!m_attempting.load() && since < 8000) return false;
    return startConnect();
}

void CameraSession::OnConnected(SDK::DeviceConnectionVersioin /*version*/) {
    m_connected.store(true);
    m_everConnected.store(true);
    m_attempting.store(false);
    clog("[CAM %d %s] ПОДКЛЮЧЕНА\n", m_index, modelUtf8().c_str());
}

void CameraSession::OnDisconnected(CrInt32u error) {
    m_connected.store(false);
    m_attempting.store(false);
    clog("[CAM %d %s] отключилась (0x%08x)\n", m_index, modelUtf8().c_str(),
         static_cast<unsigned>(error));
}

void CameraSession::OnError(CrInt32u error) {
    m_lastError.store(error);
    m_attempting.store(false);
    clog("[CAM %d %s] ошибка SDK: 0x%08x\n", m_index, modelUtf8().c_str(),
         static_cast<unsigned>(error));
}

bool CameraSession::waitConnected(int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!m_connected.load()) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(100ms);
    }
    return m_connected.load();
}

void CameraSession::beginDisconnect() {
    std::lock_guard<std::mutex> lk(m_io);
    // Disconnect any open/half-open session (even a pending one) so the camera
    // releases its single PC-Remote slot. Async — OnDisconnected fires later.
    if (m_handle) SDK::Disconnect(m_handle);
}

void CameraSession::finishRelease() {
    std::lock_guard<std::mutex> lk(m_io);
    if (m_handle) {
        SDK::ReleaseDevice(m_handle);
        m_handle = 0;
    }
    m_connected.store(false);
}

int CameraSession::readRecordingStateLocked() {
    if (!m_handle || !m_connected.load()) return -1;
    CrInt32u code = SDK::CrDevicePropertyCode::CrDeviceProperty_RecordingState;
    SDK::CrDeviceProperty* props = nullptr;
    CrInt32 n = 0;
    auto st = SDK::GetSelectDeviceProperties(m_handle, 1, &code, &props, &n);
    int result = -1;
    if (CR_SUCCEEDED(st) && props && n >= 1) {
        for (CrInt32 i = 0; i < n; ++i) {
            if (props[i].GetCode() == code) {
                result = (props[i].GetCurrentValue() ==
                          SDK::CrMovie_Recording_State_Recording) ? 1 : 0;
                break;
            }
        }
    }
    if (props) SDK::ReleaseDeviceProperties(m_handle, props);
    return result;
}

CamStatus CameraSession::readStatusLocked() {
    if (!m_handle) { m_readFailStreak = kMaxReadFail; return CamStatus{}; }

    CrInt32u codes[] = {
        SDK::CrDevicePropertyCode::CrDeviceProperty_BatteryRemain,
        SDK::CrDevicePropertyCode::CrDeviceProperty_BatteryLevel,
        SDK::CrDevicePropertyCode::CrDeviceProperty_PowerSource,
        SDK::CrDevicePropertyCode::CrDeviceProperty_RecordingState,
        SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT1_RemainingTime,
        SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT1_WritingState,
        SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity,
        SDK::CrDevicePropertyCode::CrDeviceProperty_IsoCurrentSensitivity,
        SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber,
        SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed,
        SDK::CrDevicePropertyCode::CrDeviceProperty_WhiteBalance,
        SDK::CrDevicePropertyCode::CrDeviceProperty_Colortemp,
        SDK::CrDevicePropertyCode::CrDeviceProperty_AudioInputMasterLevel,
    };
    const CrInt32u nCodes = sizeof(codes) / sizeof(codes[0]);

    SDK::CrDeviceProperty* props = nullptr;
    CrInt32 n = 0;
    auto st = SDK::GetSelectDeviceProperties(m_handle, nCodes, codes, &props, &n);
    const bool readOk = CR_SUCCEEDED(st) && props && n >= 1;

    if (readOk) {
        CamStatus s;
        s.online = true;                 // the camera is actively responding
        for (CrInt32 i = 0; i < n; ++i) {
            const CrInt32u code = props[i].GetCode();
            const CrInt64u val  = props[i].GetCurrentValue();
            switch (code) {
            case SDK::CrDevicePropertyCode::CrDeviceProperty_BatteryRemain:
                if (static_cast<std::uint16_t>(val) != kBatteryUntaken)
                    s.battery = static_cast<int>(static_cast<std::uint16_t>(val));
                break;
            case SDK::CrDevicePropertyCode::CrDeviceProperty_BatteryLevel: {
                // BatteryLevel >= 0x00010000 (..0x00010005) means USB/external power.
                std::uint32_t bl = static_cast<std::uint32_t>(val);
                if (bl >= SDK::CrBatteryLevel_USBPowerSupply &&
                    bl <= SDK::CrBatteryLevel_4_4_PowerSupply) s.acPower = true;
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_PowerSource:
                // DC or PoE = external power (not running off the battery).
                if (val == SDK::CrPowerSource_DC || val == SDK::CrPowerSource_PoE)
                    s.acPower = true;
                break;
            case SDK::CrDevicePropertyCode::CrDeviceProperty_RecordingState:
                s.rec = (val == SDK::CrMovie_Recording_State_Recording);
                break;
            case SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT1_RemainingTime: {
                std::uint32_t t = static_cast<std::uint32_t>(val);   // recordable movie time, SECONDS
                if (t != 0xFFFFFFFFu) s.cardMinutes = static_cast<int>(t / 60);
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_MediaSLOT1_WritingState:
                s.writing = (val == SDK::CrMediaSlotWritingState_ContentsWriting);
                break;
            case SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity: {
                std::uint32_t iv = static_cast<std::uint32_t>(val) & kIsoValueMask;
                s.iso = (iv == kIsoAutoValue) ? std::string("AUTO")
                                              : std::to_string(iv);
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_IsoCurrentSensitivity: {
                std::uint32_t iv = static_cast<std::uint32_t>(val) & kIsoValueMask;
                if (iv != kIsoAutoValue && iv != 0) s.isoEff = static_cast<int>(iv);
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber: {
                s.aperture.cur = static_cast<long long>(static_cast<std::uint16_t>(val));
                const auto* arr = reinterpret_cast<const std::uint16_t*>(props[i].GetValues());
                if (arr) { CrInt32u cnt = props[i].GetValueSize() / sizeof(std::uint16_t);
                    for (CrInt32u k = 0; k < cnt; ++k) { std::string l = apertureLabel(arr[k]);
                        if (!l.empty()) s.aperture.opts.push_back({ arr[k], l }); } }
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed: {
                s.shutter.cur = static_cast<long long>(static_cast<std::uint32_t>(val));
                const auto* arr = reinterpret_cast<const std::uint32_t*>(props[i].GetValues());
                if (arr) { CrInt32u cnt = props[i].GetValueSize() / sizeof(std::uint32_t);
                    for (CrInt32u k = 0; k < cnt; ++k) { std::string l = shutterLabel(arr[k]);
                        if (!l.empty()) s.shutter.opts.push_back({ arr[k], l }); } }
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_WhiteBalance: {
                s.wb.cur = static_cast<long long>(static_cast<std::uint16_t>(val));
                const auto* arr = reinterpret_cast<const std::uint16_t*>(props[i].GetValues());
                if (arr) { CrInt32u cnt = props[i].GetValueSize() / sizeof(std::uint16_t);
                    for (CrInt32u k = 0; k < cnt; ++k) { std::string l = wbLabel(arr[k]);
                        if (!l.empty()) s.wb.opts.push_back({ arr[k], l }); } }
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_Colortemp: {
                // Текущая цв. температура, K (валидна в режиме ББ «цвет.темп-ра»).
                // 0x0000 = ниже min, 0xFFFF = выше max — оба отсекаются диапазоном.
                std::uint32_t k = static_cast<std::uint32_t>(val);
                if (k >= 1000 && k <= 50000) s.wbKelvin = static_cast<int>(k);
                break;
            }
            case SDK::CrDevicePropertyCode::CrDeviceProperty_AudioInputMasterLevel: {
                // Уровень записи микрофона. Диапазон приходит как [min, max, шаг];
                // варианты строим сами — камера отдаёт границы, а не список.
                s.micGain.cur = static_cast<long long>(static_cast<std::uint16_t>(val));
                const auto* arr = reinterpret_cast<const std::uint16_t*>(props[i].GetValues());
                const CrInt32u cnt = arr ? props[i].GetValueSize() / sizeof(std::uint16_t) : 0;
                if (cnt >= 3) {
                    const int lo = arr[0], hi = arr[1];
                    const int step = arr[2] > 0 ? arr[2] : 1;
                    if (hi > lo && hi - lo <= 256)
                        for (int v = lo; v <= hi; v += step)
                            s.micGain.opts.push_back({ v, std::to_string(v) });
                }
                break;
            }
            default: break;
            }
        }
        SDK::ReleaseDeviceProperties(m_handle, props);
        m_connected.store(true);         // responsive => active connection (recovers silent reconnects)
        m_readFailStreak = 0;
        m_lastGood = s;
        m_recCached.store(s.rec);
        return s;
    }

    if (props) SDK::ReleaseDeviceProperties(m_handle, props);

    // Read failed. Debounce a couple of misses, then declare the camera offline
    // even if the SDK never sent a disconnect event (e.g. silent Wi-Fi drop).
    if (m_readFailStreak < kMaxReadFail) ++m_readFailStreak;
    if (m_readFailStreak >= kMaxReadFail) {
        m_connected.store(false);
        return CamStatus{};              // online = false
    }
    return m_lastGood;                    // brief transient: keep last known values
}

CamStatus CameraSession::readStatus() {
    std::lock_guard<std::mutex> lk(m_io);
    return readStatusLocked();
}

bool CameraSession::setIso(const std::string& value) {
    std::lock_guard<std::mutex> lk(m_io);
    if (!m_handle || !m_connected.load()) return false;

    std::uint32_t enc;
    bool wantAuto = (value == "AUTO" || value == "auto" || value == "Auto");
    if (wantAuto) {
        enc = kIsoAutoValue;                       // mode Normal + AUTO value
    } else {
        char* end = nullptr;
        long n = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || n <= 0) return false;
        enc = static_cast<std::uint32_t>(n);       // mode Normal (0) + value
    }

    // Prefer an exact encoded value advertised by the camera (keeps mode/ext bits).
    {
        CrInt32u code = SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity;
        SDK::CrDeviceProperty* props = nullptr;
        CrInt32 n = 0;
        if (CR_SUCCEEDED(SDK::GetSelectDeviceProperties(m_handle, 1, &code, &props, &n)) &&
            props && n >= 1) {
            for (CrInt32 i = 0; i < n; ++i) {
                if (props[i].GetCode() != code) continue;
                const std::uint32_t* arr =
                    reinterpret_cast<const std::uint32_t*>(props[i].GetValues());
                CrInt32u bytes = props[i].GetValueSize();
                if (arr && bytes >= sizeof(std::uint32_t)) {
                    const std::uint32_t want = enc & kIsoValueMask;
                    for (CrInt32u k = 0; k < bytes / sizeof(std::uint32_t); ++k) {
                        if ((arr[k] & kIsoValueMask) == want) { enc = arr[k]; break; }
                    }
                }
                break;
            }
        }
        if (props) SDK::ReleaseDeviceProperties(m_handle, props);
    }

    SDK::CrDeviceProperty prop;
    prop.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_IsoSensitivity);
    prop.SetCurrentValue(static_cast<CrInt64u>(enc));
    prop.SetValueType(SDK::CrDataType::CrDataType_UInt32Array);
    return CR_SUCCEEDED(SDK::SetDeviceProperty(m_handle, &prop));
}

// Set a property to an exact encoded value (the panel sends a value taken from
// the camera's own option list in /status.json, so no matching needed).
bool CameraSession::setEncoded(CrInt32u code, const std::string& value, SDK::CrDataType type) {
    std::lock_guard<std::mutex> lk(m_io);
    if (!m_handle || !m_connected.load()) return false;
    char* end = nullptr;
    long long v = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str()) return false;
    SDK::CrDeviceProperty prop;
    prop.SetCode(code);
    prop.SetCurrentValue(static_cast<CrInt64u>(v));
    prop.SetValueType(type);
    return CR_SUCCEEDED(SDK::SetDeviceProperty(m_handle, &prop));
}

bool CameraSession::setAperture(const std::string& enc) {
    return setEncoded(SDK::CrDevicePropertyCode::CrDeviceProperty_FNumber, enc,
                      SDK::CrDataType::CrDataType_UInt16Array);
}
bool CameraSession::setShutter(const std::string& enc) {
    return setEncoded(SDK::CrDevicePropertyCode::CrDeviceProperty_ShutterSpeed, enc,
                      SDK::CrDataType::CrDataType_UInt32Array);
}
bool CameraSession::setWb(const std::string& enc) {
    return setEncoded(SDK::CrDevicePropertyCode::CrDeviceProperty_WhiteBalance, enc,
                      SDK::CrDataType::CrDataType_UInt16Array);
}
// Уровень записи микрофона. У ZV-E1 это диапазон 0..31 (проверено на живой камере),
// у других моделей границы свои — поэтому значение просто передаём как есть.
bool CameraSession::setMicGain(const std::string& value) {
    return setEncoded(SDK::CrDevicePropertyCode::CrDeviceProperty_AudioInputMasterLevel, value,
                      SDK::CrDataType::CrDataType_UInt16);
}

// Set white balance by colour temperature (Kelvin). The K value only takes
// effect when WB mode is "Color Temperature", so switch the mode first.
bool CameraSession::setWbKelvin(const std::string& value) {
    std::lock_guard<std::mutex> lk(m_io);
    if (!m_handle || !m_connected.load()) return false;

    char* end = nullptr;
    long k = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || k < 1000 || k > 50000) return false;

    // 1) put WB into Color Temperature mode (no-op if already there).
    SDK::CrDeviceProperty mode;
    mode.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_WhiteBalance);
    mode.SetCurrentValue(SDK::CrWhiteBalance_ColorTemp);
    mode.SetValueType(SDK::CrDataType::CrDataType_UInt16);
    SDK::SetDeviceProperty(m_handle, &mode);
    std::this_thread::sleep_for(120ms);

    // 2) set the colour temperature itself (Colortemp = 0x0115, UInt16 Kelvin).
    SDK::CrDeviceProperty ct;
    ct.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_Colortemp);
    ct.SetCurrentValue(static_cast<CrInt64u>(k));
    ct.SetValueType(SDK::CrDataType::CrDataType_UInt16);
    return CR_SUCCEEDED(SDK::SetDeviceProperty(m_handle, &ct));
}

// Locale-independent fixed-point (avoids the ',' decimal separator of RU locale,
// which would produce invalid JSON). Values are normalized 0..1 (non-negative).
static std::string fx4(double v) {
    if (v < 0) v = 0;
    long sc = static_cast<long>(v * 10000.0 + 0.5);
    char b[24];
    std::snprintf(b, sizeof(b), "%ld.%04ld", sc / 10000, sc % 10000);
    return std::string(b);
}

// Append one normalized frame rect ({center x/y, w/h} in 0..1) to a JSON array.
// state = CrFocusFrameState (focus lock); type = frame type (2 = AF target = "captured").
static void appendFrame(std::string& s, bool& first, const char* kind,
                        CrInt32u xn, CrInt32u xd, CrInt32u yn, CrInt32u yd,
                        CrInt32u w, CrInt32u h, int state, int type) {
    if (xd == 0 || yd == 0) return;
    double cx = static_cast<double>(xn) / xd, cy = static_cast<double>(yn) / yd;
    double ww = static_cast<double>(w)  / xd, hh = static_cast<double>(h)  / yd;
    if (!first) s += ",";
    first = false;
    s += "{\"cx\":" + fx4(cx) + ",\"cy\":" + fx4(cy) + ",\"w\":" + fx4(ww) +
         ",\"h\":" + fx4(hh) + ",\"st\":" + std::to_string(state) +
         ",\"ty\":" + std::to_string(type) + ",\"k\":\"" + kind + "\"}";
}

bool CameraSession::grabLiveView(std::string& jpegOut, std::string& framesJson) {
    std::lock_guard<std::mutex> lk(m_io);
    if (!m_handle || !m_connected.load()) return false;

    SDK::CrImageInfo info;
    if (!CR_SUCCEEDED(SDK::GetLiveViewImageInfo(m_handle, &info))) return false;
    CrInt32u bufSize = info.GetBufferSize();
    if (bufSize == 0) return false;                     // live view not up yet

    if (m_lvBuf.size() < bufSize) m_lvBuf.resize(bufSize);
    SDK::CrImageDataBlock img;
    img.SetSize(bufSize);
    img.SetData(m_lvBuf.data());
    if (!CR_SUCCEEDED(SDK::GetLiveViewImage(m_handle, &img))) return false;
    CrInt32u imgSize = img.GetImageSize();
    if (imgSize == 0 || !img.GetImageData()) return false;
    jpegOut.assign(reinterpret_cast<const char*>(img.GetImageData()), imgSize);

    // Focus/face/tracking frames require the camera's OSD image mode on; enable once.
    if (!m_osdEnabled) {
        SDK::CrDeviceProperty osd;
        osd.SetCode(SDK::CrDevicePropertyCode::CrDeviceProperty_OSDImageMode);
        osd.SetCurrentValue(SDK::CrOSDImageMode_On);
        osd.SetValueType(SDK::CrDataType::CrDataType_UInt8);
        SDK::SetDeviceProperty(m_handle, &osd);
        m_osdEnabled = true;
    }

    // Focus / face / tracking frames -> normalized JSON rectangles.
    framesJson = "[";
    bool first = true;
    SDK::CrLiveViewProperty* props = nullptr;
    CrInt32 n = 0;
    if (CR_SUCCEEDED(SDK::GetLiveViewProperties(m_handle, &props, &n)) && props) {
        for (CrInt32 i = 0; i < n; ++i) {
            auto           ft  = props[i].GetFrameInfoType();
            CrInt32u       vs  = props[i].GetValueSize();
            const CrInt8u* val = props[i].GetValue();
            if (!val || vs == 0) continue;
            if (ft == SDK::CrFrameInfoType::CrFrameInfoType_FocusFrameInfo) {
                auto* f = reinterpret_cast<const SDK::CrFocusFrameInfo*>(val);
                int cnt = static_cast<int>(vs / sizeof(SDK::CrFocusFrameInfo));
                for (int k = 0; k < cnt; ++k)
                    appendFrame(framesJson, first, "focus", f[k].xNumerator, f[k].xDenominator,
                                f[k].yNumerator, f[k].yDenominator, f[k].width, f[k].height, f[k].state, f[k].type);
            } else if (ft == SDK::CrFrameInfoType::CrFrameInfoType_FaceFrameInfo) {
                auto* f = reinterpret_cast<const SDK::CrFaceFrameInfo*>(val);
                int cnt = static_cast<int>(vs / sizeof(SDK::CrFaceFrameInfo));
                for (int k = 0; k < cnt; ++k)
                    appendFrame(framesJson, first, "face", f[k].xNumerator, f[k].xDenominator,
                                f[k].yNumerator, f[k].yDenominator, f[k].width, f[k].height, f[k].state, f[k].type);
            } else if (ft == SDK::CrFrameInfoType::CrFrameInfoType_TrackingFrameInfo) {
                auto* f = reinterpret_cast<const SDK::CrTrackingFrameInfo*>(val);
                int cnt = static_cast<int>(vs / sizeof(SDK::CrTrackingFrameInfo));
                for (int k = 0; k < cnt; ++k)
                    appendFrame(framesJson, first, "track", f[k].xNumerator, f[k].xDenominator,
                                f[k].yNumerator, f[k].yDenominator, f[k].width, f[k].height, f[k].state, f[k].type);
            }
        }
        SDK::ReleaseLiveViewProperties(m_handle, props);
    }
    framesJson += "]";
    return true;
}

bool CameraSession::setRec(bool start) {
    std::lock_guard<std::mutex> lk(m_io);
    if (!m_handle || !m_connected.load()) return false;

    int state = readRecordingStateLocked();   // -1 unknown, 0 not rec, 1 rec
    if (state == 1 && start)  return true;     // already recording
    if (state == 0 && !start) return true;     // already stopped

    // Simulate a REC button press (down -> up) which toggles recording.
    SDK::SendCommand(m_handle, SDK::CrCommandId::CrCommandId_MovieRecord,
                     SDK::CrCommandParam::CrCommandParam_Down);
    std::this_thread::sleep_for(120ms);
    SDK::SendCommand(m_handle, SDK::CrCommandId::CrCommandId_MovieRecord,
                     SDK::CrCommandParam::CrCommandParam_Up);
    return true;
}

} // namespace coll
