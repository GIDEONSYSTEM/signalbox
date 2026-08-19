#include "BmdCamera.h"

#include "Json.h"
#include "net/HttpClient.h"
#include "net/JsonRead.h"
#include "platform/Platform.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::function<void(const std::string&)> g_logSink;
std::function<void()>                   g_onChanged;

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
void setOnChanged(std::function<void()> cb)              { g_onChanged = std::move(cb); }

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
    m_ws.close();
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

// Свойства, на которые подписываемся. Таймкода тут намеренно нет: он шлёт
// 12-13 событий в секунду даже на простое и забил бы канал — его дешевле раз в
// секунду дочитать одним запросом.
static const char* const kSubscribe[] = {
    "/transports/0/record", "/video/iso", "/video/gain", "/video/whiteBalance",
    "/video/whiteBalanceTint", "/video/shutter", "/camera/tallyStatus",
    "/camera/power", "/system/format", "/system/dynamicRange", "/lens/iris",
};

bool BmdCamera::openWs() {
    std::string host; int port;
    { std::lock_guard<std::mutex> lk(m_hostMx); host = m_host; port = m_port; }
    if (host.empty()) return false;
    if (!m_ws.open(host, port, std::string(kApi) + "/event/websocket", 3000)) return false;

    std::string msg;
    m_ws.recvText(msg, 1500);                       // websocketOpened

    std::string req = "{\"type\":\"request\",\"id\":1,\"data\":{\"action\":\"subscribe\",\"properties\":[";
    for (size_t i = 0; i < sizeof(kSubscribe) / sizeof(kSubscribe[0]); ++i) {
        if (i) req += ",";
        req += "\"" + std::string(kSubscribe[i]) + "\"";
    }
    req += "]}}";
    if (!m_ws.sendText(req)) { m_ws.close(); return false; }

    // Ответ на подписку содержит И текущие значения — отдельный опрос не нужен.
    if (m_ws.recvText(msg, 3000) && msg.find("\"success\":true") != std::string::npos) {
        applyEvent(msg);
        blog("[BMD %s] подписка на события установлена.\n", m_deviceName.c_str());
        return true;
    }
    m_ws.close();
    return false;
}

// Разобрать сообщение подписки. Годятся оба вида: событие об одном свойстве и
// ответ на subscribe, где значения лежат пачкой — поля ищем по именам, а какое
// свойство пришло, видно по самим именам.
bool BmdCamera::applyEvent(const std::string& m) {
    bool touched = false;
    auto setNum = [&](const char* key, long long& dst) {
        const long long v = jsonr::num(m, key, LLONG_MIN);
        if (v != LLONG_MIN) { dst = v; touched = true; }
    };
    if (m.find("\"recording\"") != std::string::npos) {
        m_rec.store(jsonr::boolean(m, "recording")); touched = true;
    }
    setNum("iso", m_slow.iso);
    setNum("gain", m_slow.gain);
    setNum("whiteBalance", m_slow.wb);
    setNum("whiteBalanceTint", m_slow.tint);
    setNum("shutterSpeed", m_slow.shutter);
    setNum("milliVolt", m_slow.milliVolt);
    if (m.find("\"source\"") != std::string::npos)       { m_slow.powerSrc = jsonr::str(m, "source"); touched = true; }
    if (m.find("\"tallyStatus\"") != std::string::npos ||
        m.find("\"/camera/tallyStatus\"") != std::string::npos) {
        const std::string s = jsonr::str(m, "status");
        if (!s.empty()) { m_slow.tally = s; touched = true; }
    }
    if (m.find("\"codec\"") != std::string::npos) {
        m_slow.codec = jsonr::str(m, "codec");
        m_slow.fps   = jsonr::str(m, "frameRate");
        m_slow.w     = jsonr::numIn(m, "recordResolution", "width");
        m_slow.h     = jsonr::numIn(m, "recordResolution", "height");
        touched = true;
    }
    if (m.find("\"dynamicRange\"") != std::string::npos) { m_slow.dynRange = jsonr::str(m, "dynamicRange"); touched = true; }
    if (m.find("\"apertureStop\"") != std::string::npos) {
        bool f = false; const double v = jsonr::real(m, "apertureStop", &f);
        if (f) { m_slow.iris = v; touched = true; }
    }
    return touched;
}

void BmdCamera::refreshTimecode() {
    std::string host; int port;
    { std::lock_guard<std::mutex> lk(m_hostMx); host = m_host; port = m_port; }
    if (host.empty()) return;
    const net::Resp r = net::get(host, port, std::string(kApi) + "/transports/0/timecode", 2500);
    if (r.ok()) m_tc = jsonr::str(r.body, "display");
}

void BmdCamera::pollLoop() {
    int restTick = 0;
    while (m_run.load()) {
        if (!m_wsReady) {
            // Подписки нет: берём состояние обычным опросом и пробуем поднять её.
            // Здесь же выясняется, включено ли на камере REST-управление.
            bool waiting;
            { std::lock_guard<std::mutex> lk(m_pendMx); waiting = !m_pending.empty(); }
            pollOnce(waiting || restTick % kFullEvery == 0);
            ++restTick;
            if (m_online.load()) m_wsReady = openWs();
            if (!m_wsReady)
                for (int k = 0; k < 10 && m_run.load(); ++k) std::this_thread::sleep_for(100ms);
            continue;
        }

        // Подписка жива: ждём событие. Таймаут в секунду заодно служит тактом
        // для таймкода — на него мы намеренно не подписаны.
        std::string msg;
        if (m_ws.recvText(msg, 1000)) {
            if (applyEvent(msg)) rebuildCache();
            bool waiting;
            { std::lock_guard<std::mutex> lk(m_pendMx); waiting = !m_pending.empty(); }
            if (waiting) reconcile();     // подтверждение пришло push'ом — сверяем сразу
            continue;
        }
        if (!m_ws.connected()) {
            m_wsReady = false;
            blog("[BMD %s] подписка оборвалась — возвращаюсь к опросу.\n", m_deviceName.c_str());
            continue;
        }
        refreshTimecode();
        rebuildCache();
        bool waiting;
        { std::lock_guard<std::mutex> lk(m_pendMx); waiting = !m_pending.empty(); }
        if (waiting) reconcile();
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
    // не включён Web Media Manager. Это НЕ «камера сломалась»: при выключенном
    // управлении камера отвечает на ЛЮБОЙ путь 307-редиректом на тот же путь со
    // слэшем, а по нему — 404 (§15.1).
    // ⚠️ Проверять надо именно «ответ не 2xx», а не только 404: редиректы мы не
    // ходим, поэтому от выключенной камеры приходит 307, и если считать провалом
    // лишь 0 и 404, камера попадёт в панель как рабочая, но с пустыми полями —
    // ровно это и наблюдалось на двух камерах студии.
    if (!rec.ok()) {
        const bool wasOnline = m_online.exchange(false);
        const bool apiOff    = (rec.status != 0);   // ответил по HTTP, но не успехом
        if (m_apiOff.exchange(apiOff) != apiOff && apiOff)
            blog("[BMD %s] найдена, но REST-управление не включено (Web Media Manager).\n",
                 m_deviceName.c_str());
        if (wasOnline)
            blog("[BMD %s] связь потеряна (%s).\n", m_deviceName.c_str(),
                 apiOff ? "REST-управление выключено" : rec.err.c_str());
        std::lock_guard<std::mutex> lk(m_cacheMx);
        m_cache.clear();
        return;
    }

    if (!m_online.exchange(true))
        blog("[BMD %s] ПОДКЛЮЧЕНА (%s, %s).\n", m_deviceName.c_str(), m_model.c_str(), host.c_str());
    m_apiOff.store(false);

    m_rec.store(jsonr::boolean(rec.body, "recording"));
    m_tc = jsonr::str(api("/transports/0/timecode").body, "display");

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
        m_slow.autoExposure = jsonr::str(api("/video/autoExposure").body, "mode");
        // Списки допустимых значений — только от камеры, никаких своих таблиц.
        m_slow.isoOpts     = jsonr::numArray(api("/video/supportedISOs").body,  "supportedISOs");
        m_slow.gainOpts    = jsonr::numArray(api("/video/supportedGains").body, "supportedGains");
        m_slow.shutterOpts = jsonr::numArray(api("/video/supportedShutters").body, "shutterSpeeds");
        const std::string wbd = api("/video/whiteBalance/description").body;
        m_slow.wbMin = jsonr::numIn(wbd, "whiteBalance", "min");
        m_slow.wbMax = jsonr::numIn(wbd, "whiteBalance", "max");
        const std::string td = api("/video/whiteBalanceTint/description").body;
        m_slow.tintMin = jsonr::numIn(td, "whiteBalanceTint", "min", 0);
        m_slow.tintMax = jsonr::numIn(td, "whiteBalanceTint", "max", 0);
    }

    rebuildCache();
    if (full) reconcile();
}

// Собрать фрагмент /status.json из текущих полей. Зовётся и после REST-опроса,
// и после события подписки — форма одна, источник разный.
void BmdCamera::rebuildCache() {
    // Фрагмент /status.json — СВОЙ, не сониевский: здесь нет перегрева и уровня
    // микрофона, зато есть таймкод, tally и питание. Панель ветвится по vendor.
    std::string j = "{";
    j += "\"key\":\""     + jsonw::esc(key()) + "\",";
    j += "\"vendor\":\""  + std::string(cam::vendorTag(cam::Vendor::Bmd)) + "\",";
    j += "\"id\":\""      + jsonw::esc(m_idLabel) + "\",";
    j += "\"model\":\""   + jsonw::esc(m_model) + "\",";
    j += "\"name\":\""    + jsonw::esc(m_deviceName) + "\",";
    j += "\"ip\":\""      + jsonw::esc(address()) + "\",";
    j += "\"online\":"    + std::string(jsonw::boolStr(m_online.load())) + ",";
    j += "\"rec\":"       + std::string(jsonw::boolStr(m_rec.load())) + ",";
    j += "\"apiOff\":"    + std::string(jsonw::boolStr(m_apiOff.load())) + ",";
    j += "\"timecode\":\"" + jsonw::esc(m_tc) + "\",";
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
    j += "\"irisControllable\":" + std::string(jsonw::boolStr(m_slow.irisControllable)) + ",";
    j += "\"autoExposure\":\"" + jsonw::esc(m_slow.autoExposure) + "\",";
    auto arr = [](const std::vector<long long>& v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
        return s + "]";
    };
    j += "\"isoOpts\":"     + arr(m_slow.isoOpts) + ",";
    j += "\"gainOpts\":"    + arr(m_slow.gainOpts) + ",";
    j += "\"shutterOpts\":" + arr(m_slow.shutterOpts) + ",";
    j += "\"wbRange\":"     + ((m_slow.wbMin > 0 && m_slow.wbMax > 0)
             ? "[" + std::to_string(m_slow.wbMin) + "," + std::to_string(m_slow.wbMax) + "]"
             : std::string("null")) + ",";
    j += "\"tintRange\":[" + std::to_string(m_slow.tintMin) + "," + std::to_string(m_slow.tintMax) + "]";
    j += "}";

    { std::lock_guard<std::mutex> lk(m_cacheMx); m_cache = std::move(j); }
    if (g_onChanged) g_onChanged();     // разбудить сборку /status.json
}


// Записать целое свойство и запомнить, чего ждём (сверку делает reconcile()).
bool BmdCamera::writeNum(const std::string& path, const std::string& field, long long v) {
    std::string host; int port;
    { std::lock_guard<std::mutex> lk(m_hostMx); host = m_host; port = m_port; }
    if (host.empty()) return false;

    const std::string body = "{\"" + field + "\":" + std::to_string(v) + "}";
    const net::Resp r = net::put(host, port, std::string(kApi) + path, body, 3000);

    // 403 — камера прямо говорит «сейчас это менять нельзя» (например, значением
    // управляет автоэкспозиция). Это не наша ошибка и настаивать бессмысленно.
    if (r.status == 403) {
        blog("[BMD %s] %s: камера сейчас не даёт менять это значение.\n", m_deviceName.c_str(), field.c_str());
        return false;
    }
    if (!r.ok()) {
        blog("[BMD %s] %s=%lld: не принято (код %d%s%s).\n", m_deviceName.c_str(), field.c_str(),
             v, r.status, r.err.empty() ? "" : ", ", r.err.c_str());
        return false;
    }
    { std::lock_guard<std::mutex> lk(m_pendMx);
      Pending p; p.path = path; p.field = field; p.want = v; p.tries = 0;
      m_pending[field] = p; }
    return true;
}

// Сверяем то, что просили, с тем, что камера показывает на самом деле.
void BmdCamera::reconcile() {
    std::vector<Pending> retry;
    {
        std::lock_guard<std::mutex> lk(m_pendMx);
        for (auto it = m_pending.begin(); it != m_pending.end(); ) {
            long long cur = -1;
            const std::string& f = it->second.field;
            if      (f == "iso")              cur = m_slow.iso;
            else if (f == "gain")             cur = m_slow.gain;
            else if (f == "whiteBalance")     cur = m_slow.wb;
            else if (f == "whiteBalanceTint") cur = m_slow.tint;
            else if (f == "shutterSpeed")     cur = m_slow.shutter;

            if (cur == it->second.want) { it = m_pending.erase(it); continue; }
            if (++it->second.tries > kSetRetries) {
                blog("[BMD %s] %s: просили %lld, камера держит %lld — больше не настаиваю.\n",
                     m_deviceName.c_str(), f.c_str(), it->second.want, cur);
                it = m_pending.erase(it);
                continue;
            }
            retry.push_back(it->second);
            ++it;
        }
    }
    std::string host; int port;
    { std::lock_guard<std::mutex> lk(m_hostMx); host = m_host; port = m_port; }
    for (const Pending& p : retry) {
        const std::string body = "{\"" + p.field + "\":" + std::to_string(p.want) + "}";
        net::put(host, port, std::string(kApi) + p.path, body, 3000);
    }
}

// Действия, которые понимает камера Blackmagic. Имена те же, что у Sony, чтобы
// групповые команды и привязки не зависели от марки.
bool BmdCamera::command(const std::string& action, const std::string& value) {
    const long long v = std::atoll(value.c_str());
    if (action == "iso")      return writeNum("/video/iso", "iso", v);
    if (action == "gain")     return writeNum("/video/gain", "gain", v);
    if (action == "wbkelvin") return writeNum("/video/whiteBalance", "whiteBalance", v);
    if (action == "tint")     return writeNum("/video/whiteBalanceTint", "whiteBalanceTint", v);
    // Выдержка: пишем скоростью (1/N). У камеры есть и режим угла, приоритет по
    // спеке — shutterSpeed выше shutterAngle.
    if (action == "shutter")  return writeNum("/video/shutter", "shutterSpeed", v);
    if (action == "rec") {
        blog("[BMD %s] запись пока не подключена (нужна проверка на камере с картой).\n",
             m_deviceName.c_str());
        return false;
    }
    blog("[BMD %s] действие «%s» эта камера не умеет.\n", m_deviceName.c_str(), action.c_str());
    return false;
}

} // namespace bmd
