#include "BmdCamera.h"

#include "Json.h"
#include "net/HttpClient.h"
#include "net/JsonRead.h"
#include "platform/Platform.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

using namespace std::chrono_literals;

namespace {

std::function<void(const std::string&)> g_logSink;

void blog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n) : sizeof(buf) - 1;
    const std::string s(buf, len);
    if (g_logSink) g_logSink(s);
    else           plat::writeConsole(s);
}

constexpr const char* kApi = "/control/api/v1";

// Полный опрос — раз в kFullEvery проходов: модель, формат, питание, экспозиция
// меняются редко, а запись и таймкод нужны быстро.
constexpr int kFullEvery = 5;

} // namespace

namespace bmd {

void setLogSink(std::function<void(const std::string&)> sink) { g_logSink = std::move(sink); }

BmdCamera::BmdCamera(int index, const Found& f)
    : m_index(index),
      m_idLabel("CAM " + std::to_string(index)),
      m_uniqueId(f.uniqueId),
      m_model(f.productName.empty() ? std::string("Blackmagic") : f.productName),
      m_deviceName(f.deviceName),
      m_host(f.host),
      m_port(f.port) {
    m_thread = std::thread([this] { pollLoop(); });
}

BmdCamera::~BmdCamera() {
    m_run.store(false);
    if (m_thread.joinable()) m_thread.join();
}

std::string BmdCamera::address() const {
    std::lock_guard<std::mutex> lk(m_hostMx);
    return m_host;
}

void BmdCamera::updateHost(const std::string& host, int port) {
    std::lock_guard<std::mutex> lk(m_hostMx);
    if (m_host != host || m_port != port) {
        blog("[BMD %s] адрес сменился: %s -> %s\n", m_deviceName.c_str(), m_host.c_str(), host.c_str());
        m_host = host;
        m_port = port;
    }
}

void BmdCamera::beginDisconnect() { m_run.store(false); }

void BmdCamera::finishRelease() {
    m_run.store(false);
    if (m_thread.joinable()) m_thread.join();
}

std::string BmdCamera::statusJson() {
    std::lock_guard<std::mutex> lk(m_cacheMx);
    if (!m_cache.empty()) return m_cache;
    // Ещё ни одного удачного опроса — отдаём минимальную карточку, чтобы камера
    // всё равно была видна в панели.
    std::string j = "{";
    j += "\"key\":\""    + jsonw::esc(key()) + "\",";
    j += "\"vendor\":\"" + std::string(cam::vendorTag(cam::Vendor::Bmd)) + "\",";
    j += "\"id\":\""     + jsonw::esc(m_idLabel) + "\",";
    j += "\"model\":\""  + jsonw::esc(m_model) + "\",";
    j += "\"name\":\""   + jsonw::esc(m_deviceName) + "\",";
    j += "\"ip\":\""     + jsonw::esc(address()) + "\",";
    j += "\"online\":false,\"rec\":false,";
    j += "\"apiOff\":"   + std::string(jsonw::boolStr(m_apiOff.load()));
    j += "}";
    return j;
}

void BmdCamera::pollLoop() {
    int tick = 0;
    while (m_run.load()) {
        pollOnce(tick % kFullEvery == 0);
        ++tick;
        for (int k = 0; k < 10 && m_run.load(); ++k) std::this_thread::sleep_for(100ms);
    }
}

void BmdCamera::pollOnce(bool full) {
    std::string host; int port;
    { std::lock_guard<std::mutex> lk(m_hostMx); host = m_host; port = m_port; }
    if (host.empty()) return;

    auto api = [&](const char* path) { return net::get(host, port, std::string(kApi) + path, 2500); };

    // Запись и таймкод — каждый проход.
    net::Resp rec = api("/transports/0/record");

    // 🔴 Камера видна в сети, но control-API не отвечает — почти наверняка на ней
    // не включён Web Media Manager. Это НЕ «камера сломалась»: в §15.1 показано,
    // что при выключенном управлении любой путь отвечает 307/404 одинаково.
    if (rec.status == 0 || rec.status == 404) {
        const bool wasOnline = m_online.exchange(false);
        const bool apiOff    = (rec.status == 404);
        if (m_apiOff.exchange(apiOff) != apiOff && apiOff)
            blog("[BMD %s] найдена, но REST-управление не включено (Web Media Manager).\n",
                 m_deviceName.c_str());
        if (wasOnline)
            blog("[BMD %s] связь потеряна (%s).\n", m_deviceName.c_str(),
                 rec.status == 404 ? "API выключен" : rec.err.c_str());
        std::lock_guard<std::mutex> lk(m_cacheMx);
        m_cache.clear();
        return;
    }

    if (!m_online.exchange(true))
        blog("[BMD %s] ПОДКЛЮЧЕНА (%s, %s).\n", m_deviceName.c_str(), m_model.c_str(), host.c_str());
    m_apiOff.store(false);

    const bool recording = jsonr::boolean(rec.body, "recording");
    m_rec.store(recording);
    const std::string tc = jsonr::str(api("/transports/0/timecode").body, "display");

    // Редко меняющееся — раз в kFullEvery проходов; между ними берём прежнее.
    if (full) {
        const std::string fmt = api("/system/format").body;
        m_slow.codec = jsonr::str(fmt, "codec");
        m_slow.fps   = jsonr::str(fmt, "frameRate");
        m_slow.w     = jsonr::numIn(fmt, "recordResolution", "width");
        m_slow.h     = jsonr::numIn(fmt, "recordResolution", "height");
        m_slow.dynRange    = jsonr::str(api("/system/dynamicRange").body, "dynamicRange");
        m_slow.tally = jsonr::str(api("/camera/tallyStatus").body, "status");
        const std::string pw = api("/camera/power").body;
        m_slow.powerSrc = jsonr::str(pw, "source");
        m_slow.milliVolt       = jsonr::num(pw, "milliVolt");
        m_slow.iso   = jsonr::num(api("/video/iso").body,  "iso");
        m_slow.gain  = jsonr::num(api("/video/gain").body, "gain");
        m_slow.wb    = jsonr::num(api("/video/whiteBalance").body, "whiteBalance");
        m_slow.tint  = jsonr::num(api("/video/whiteBalanceTint").body, "whiteBalanceTint");
        const std::string sh = api("/video/shutter").body;
        m_slow.shutter = jsonr::num(sh, "shutterSpeed");
        m_slow.shutterMeas  = jsonr::str(api("/video/shutter/measurement").body, "measurement");
        const std::string iris = api("/lens/iris").body;
        bool f = false;
        m_slow.iris = jsonr::real(iris, "apertureStop", &f);
        if (!f) m_slow.iris = -1.0;
        m_slow.irisControllable = jsonr::boolean(api("/lens/iris/description").body, "controllable");
    }

    // Фрагмент /status.json — СВОЙ, не сониевский: здесь нет перегрева и уровня
    // микрофона, зато есть таймкод, tally и питание. Панель ветвится по vendor.
    std::string j = "{";
    j += "\"key\":\""     + jsonw::esc(key()) + "\",";
    j += "\"vendor\":\""  + std::string(cam::vendorTag(cam::Vendor::Bmd)) + "\",";
    j += "\"id\":\""      + jsonw::esc(m_idLabel) + "\",";
    j += "\"model\":\""   + jsonw::esc(m_model) + "\",";
    j += "\"name\":\""    + jsonw::esc(m_deviceName) + "\",";
    j += "\"ip\":\""      + jsonw::esc(host) + "\",";
    j += "\"online\":true,";
    j += "\"rec\":"       + std::string(jsonw::boolStr(recording)) + ",";
    j += "\"apiOff\":false,";
    j += "\"timecode\":\"" + jsonw::esc(tc) + "\",";
    j += "\"codec\":\""   + jsonw::esc(m_slow.codec) + "\",";
    j += "\"fps\":\""     + jsonw::esc(m_slow.fps) + "\",";
    j += "\"resolution\":" + ((m_slow.w > 0 && m_slow.h > 0)
             ? "[" + std::to_string(m_slow.w) + "," + std::to_string(m_slow.h) + "]" : std::string("null")) + ",";
    j += "\"dynamicRange\":\"" + jsonw::esc(m_slow.dynRange) + "\",";
    j += "\"tally\":\""   + jsonw::esc(m_slow.tally) + "\",";
    j += "\"powerSource\":\"" + jsonw::esc(m_slow.powerSrc) + "\",";
    j += "\"milliVolt\":" + jsonw::numOrNull(m_slow.milliVolt) + ",";
    j += "\"iso\":"       + jsonw::numOrNull(m_slow.iso) + ",";
    j += "\"gain\":"      + (m_slow.gain == -1 ? std::string("null") : std::to_string(m_slow.gain)) + ",";
    j += "\"wb\":"        + jsonw::numOrNull(m_slow.wb) + ",";
    j += "\"tint\":"      + (m_slow.tint == -1 ? std::string("null") : std::to_string(m_slow.tint)) + ",";
    j += "\"shutter\":"   + jsonw::numOrNull(m_slow.shutter) + ",";
    j += "\"shutterMeasurement\":\"" + jsonw::esc(m_slow.shutterMeas) + "\",";
    if (m_slow.iris >= 0) {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", m_slow.iris);   // без %f-локали: точка литеральная ниже
        std::string t(b);
        for (char& c : t) if (c == ',') c = '.';                   // RU-локаль печатает запятую (§3)
        j += "\"iris\":" + t + ",";
    } else {
        j += "\"iris\":null,";
    }
    j += "\"irisControllable\":" + std::string(jsonw::boolStr(m_slow.irisControllable));
    j += "}";

    std::lock_guard<std::mutex> lk(m_cacheMx);
    m_cache = std::move(j);
}

// Шаг 2 — только чтение. Запись (REC, экспозиция, ББ) появится на шаге 3-4,
// вместе со сверкой применения: у BM код 204 не означает, что подействовало.
bool BmdCamera::command(const std::string& action, const std::string& value) {
    (void)value;
    blog("[BMD %s] действие «%s»: управление BM пока не подключено (только чтение).\n",
         m_deviceName.c_str(), action.c_str());
    return false;
}

} // namespace bmd
