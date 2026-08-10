#pragma once
// One Sony camera connection (PC Remote / PTP-IP over Wi-Fi).
// Wraps SDK Connect/Disconnect, status reads and rec/iso commands.
// Implements IDeviceCallback so the SDK can report connect/disconnect/errors.

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "ICrCameraObjectInfo.h"

namespace coll {

// A selectable camera property: current encoded value + the list the camera
// allows ({encoded value, display label}).
struct CamPropOpts {
    long long                                       cur = -1;   // -1 = unknown
    std::vector<std::pair<long long, std::string>>  opts;
};

struct CamStatus {
    bool        online      = false;
    bool        rec         = false;
    int         battery     = -1;   // percent 0..100, -1 = unknown
    bool        acPower     = false;// running on external/USB power (plugged in / charging)
    int         cardMinutes = -1;   // minutes,        -1 = unknown
    bool        writing     = false;
    std::string iso;                // "AUTO" or number string, empty = unknown
    int         isoEff      = -1;   // effective ISO number, -1 = unknown / AUTO
    CamPropOpts aperture;           // FNumber (value = F*100)
    CamPropOpts shutter;            // ShutterSpeed (hi=num, lo=den)
    CamPropOpts wb;                 // WhiteBalance (mode enum)
    int         wbKelvin    = -1;   // текущая цвет. температура ББ, K (-1 = недоступно)
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

    int                        m_index;
    SCRSDK::ICrCameraObjectInfo* m_info   = nullptr;   // deep copy, owned
    SCRSDK::CrDeviceHandle     m_handle   = 0;
    std::atomic<bool>          m_connected{false};
    std::atomic<bool>          m_everConnected{false};  // connected at least once
    std::atomic<bool>          m_attempting{false};     // a Connect is in flight (awaiting callback)
    std::atomic<long long>     m_attemptMs{0};          // steady_clock ms of last attempt
    std::atomic<unsigned int>  m_lastError{0};
    int                        m_readFailStreak = 0;    // consecutive failed property reads (m_io)
    CamStatus                  m_lastGood;              // last good snapshot, for debounce (m_io)
    std::vector<CrInt8u>       m_lvBuf;                 // reusable live-view image buffer (m_io)
    bool                       m_osdEnabled = false;    // OSDImageMode turned on once (m_io)
    std::atomic<bool>          m_recCached{false};      // last polled rec state (lock-free read)
    std::mutex                 m_io;                    // serializes SDK calls on this handle
};

} // namespace coll
