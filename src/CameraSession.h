#pragma once
// One Sony camera connection (PC Remote / PTP-IP over Wi-Fi).
// Wraps SDK Connect/Disconnect, status reads and rec/iso commands.
// Implements IDeviceCallback so the SDK can report connect/disconnect/errors.

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "ICrCameraObjectInfo.h"

namespace coll {

// Куда писать диагностику камер. Без этого она уходила в консоль, которой у
// программы нет (сборка без окна) — и всё, что происходило с подключением,
// ошибками SDK и подгонкой значений, пропадало бесследно.
void setLogSink(std::function<void(const std::string&)> sink);

// A selectable camera property: current encoded value + the list the camera
// allows ({encoded value, display label}).
struct CamPropOpts {
    long long                                       cur = -1;   // -1 = unknown
    std::vector<std::pair<long long, std::string>>  opts;
    // Можно ли менять свойство ПРЯМО СЕЙЧАС: камера сообщает это сама
    // (IsSetEnableCurrentValue). В режимах P/Auto выдержка и диафрагма не
    // пишутся, и раньше панель предлагала их менять впустую. Так делает и сэмпл
    // Sony: перед установкой он проверяет тот же флаг.
    bool                                            writable = false;
};

struct CamStatus {
    bool        online      = false;
    bool        rec         = false;
    int         battery     = -1;   // percent 0..100, -1 = unknown
    bool        acPower     = false;// running on external/USB power (plugged in / charging)
    int         cardMinutes = -1;   // minutes,        -1 = unknown
    bool        writing     = false;
    // Перегрев, как его сообщает сама камера (DeviceOverheatingState):
    // -1 неизвестно (камера свойство не отдаёт), 0 норма, 1 близко к перегреву,
    // 2 перегрев. Значения — из enum SDK, не выдуманные.
    int         overheat    = -1;
    std::string iso;                // "AUTO" or number string, empty = unknown
    int         isoEff      = -1;   // effective ISO number, -1 = unknown / AUTO
    // Полный список ISO, который отдаёт САМА камера: у разных тушек и режимов он
    // разный, и зашитая в панель таблица неизбежно с ним расходилась.
    // cur/значения — уже с наложенной маской 0x00FFFFFF (AUTO = 0xFFFFFF).
    CamPropOpts isoOpts;
    CamPropOpts aperture;           // FNumber (value = F*100)
    CamPropOpts shutter;            // ShutterSpeed (hi=num, lo=den)
    CamPropOpts wb;                 // WhiteBalance (mode enum)
    int         wbKelvin    = -1;   // текущая цвет. температура ББ, K (-1 = недоступно)
    // Диапазон цвет. температуры, как его объявляет камера: min/max/шаг. У ZV-E1
    // шаг 100 K — панель раньше округляла к своим пресетам и промахивалась.
    int         wbKelvinMin  = -1;
    int         wbKelvinMax  = -1;
    int         wbKelvinStep = -1;
    bool        wbKelvinWritable = false;   // ББ по кельвинам пишется только в режиме «цвет. темп-ра»
    CamPropOpts micGain;            // уровень записи микрофона (AudioInputMasterLevel, 0..31)
};

class CameraSession : public SCRSDK::IDeviceCallback {
public:
    CameraSession(int index, const SCRSDK::ICrCameraObjectInfo* enumInfo);
    ~CameraSession();

    CameraSession(const CameraSession&)            = delete;
    CameraSession& operator=(const CameraSession&) = delete;

    // Issue the (async) connect. Returns false only on immediate SDK failure.
    bool startConnect();
    // Re-issue connect for a camera that has never connected yet (initial
    // attempt failed or the pairing dialog was not confirmed). No-op once the
    // camera has connected at least once (then CrReconnecting handles drops).
    // Returns true if it actually issued a Connect this call (so the caller can
    // serialize handshakes by spacing attempts out).
    bool maybeRetryConnect();
    // Block up to timeoutMs waiting for OnConnected. Returns connected state.
    bool waitConnected(int timeoutMs);
    bool isConnected() const { return m_connected.load(); }
    // Last polled recording state (cached, lock-free) — used by the global hotkey.
    bool cachedRecording() const { return m_recCached.load(); }
    // Graceful shutdown in two phases so every camera disconnects in parallel:
    void beginDisconnect();   // ask the camera to disconnect (async, no release)
    void finishRelease();     // release the device handle (after all are down)

    int          index()   const { return m_index; }
    std::string  idLabel() const;            // "CAM 1"
    std::string  modelUtf8() const;          // e.g. "ZV-E1"
    std::string  ipUtf8()  const;
    std::string  macUtf8() const;
    unsigned int lastError() const { return m_lastError.load(); }

    // Query live status from the camera (safe to call from poll thread).
    CamStatus readStatus();


    // Commands (called from HTTP thread). Return true if the command was issued.
    bool setIso(const std::string& value);   // "AUTO" or a number like "1600"
    bool setRec(bool start);                  // idempotent: presses REC only if needed
    bool setAperture(const std::string& enc); // enc = exact value from /status.json opts
    bool setShutter(const std::string& enc);
    bool setWb(const std::string& enc);
    bool setWbKelvin(const std::string& value); // switch WB to Color Temp + set Kelvin
    bool setMicGain(const std::string& value);  // уровень записи микрофона, 0..31

    // Live view (on-demand). jpegOut = latest JPEG; framesJson = focus/face/tracking
    // frame rectangles (normalized). Returns false if a frame isn't ready yet.
    bool grabLiveView(std::string& jpegOut, std::string& framesJson);

    // ---- IDeviceCallback ---- (defined in .cpp so they can log)
    void OnConnected(SCRSDK::DeviceConnectionVersioin version) override;
    void OnDisconnected(CrInt32u error) override;
    void OnError(CrInt32u error) override;
    // All other callbacks use the empty defaults from IDeviceCallback.

private:
    CamStatus readStatusLocked();            // assumes m_io held
    int       readRecordingStateLocked();    // -1 unknown, 0 not rec, 1 rec
    bool      setEncoded(CrInt32u code, const std::string& value, SCRSDK::CrDataType type);
    bool      setEncodedLocked(CrInt32u code, long long v, SCRSDK::CrDataType type);

    // 🔴 Камера применяет НЕ ТО значение, которое просили, когда SDK объявляет
    // список шире, чем камера поддерживает в текущем режиме. У ZV-E1 при 25/50p
    // в списке остаются «киношные» 1/48 и 1/96, которых на самом деле нет, и
    // промах равен числу таких значений между текущим и запрошенным (проверено
    // на живой камере: 1/40 -> просим 1/50 -> получаем 1/60).
    // Поэтому после установки сверяем результат при следующем опросе и досылаем
    // команду. Сходится за одну-две попытки: чем ближе, тем меньше «пустых»
    // значений между. Число попыток ограничено — если камера значение просто не
    // принимает, мы не должны спорить с ней вечно и мешать человеку крутить
    // колесо на тушке.
    struct PendingSet {
        long long          want  = 0;    // ожидаемое значение в том же виде, в каком его читает readStatus
        long long          write = 0;    // что именно отправлять (у ISO это код с битами режима)
        SCRSDK::CrDataType type{};
        int                tries = 0;
    };
    void      rememberTargetLocked(CrInt32u code, long long want, long long write,
                                   SCRSDK::CrDataType type);
    void      reconcileLocked(const CamStatus& s);   // assumes m_io held

    int                        m_index;
    SCRSDK::ICrCameraObjectInfo* m_info   = nullptr;   // deep copy, owned
    SCRSDK::CrDeviceHandle     m_handle   = 0;
    std::atomic<bool>          m_connected{false};
    std::atomic<bool>          m_everConnected{false};  // connected at least once
    std::atomic<bool>          m_attempting{false};     // a Connect is in flight (awaiting callback)
    std::atomic<long long>     m_attemptMs{0};          // steady_clock ms of last attempt
    std::atomic<unsigned int>  m_lastError{0};
    int                        m_readFailStreak = 0;    // consecutive failed property reads (m_io)
    int                        m_overheatLogged = -1;   // последнее записанное в лог состояние нагрева (m_io)
    CamStatus                  m_lastGood;              // last good snapshot, for debounce (m_io)
    std::vector<CrInt8u>       m_lvBuf;                 // reusable live-view image buffer (m_io)
    bool                       m_osdEnabled = false;    // OSDImageMode turned on once (m_io)
    std::atomic<bool>          m_recCached{false};      // last polled rec state (lock-free read)
    std::map<CrInt32u, PendingSet> m_pending;           // код свойства -> чего ждём (m_io)
    std::mutex                 m_io;                    // serializes SDK calls on this handle
};

} // namespace coll
