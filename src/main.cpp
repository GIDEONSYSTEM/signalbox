// SignalBox — connects to all Sony cameras over Wi-Fi (PC Remote / PTP-IP),
// polls status once a second and serves a local HTTP API + static front-end:
//   GET  /status.json            -> camera status for the OBS overlay
//   POST /cmd                    -> { "cam":1|"all", "action":"rec"|"iso", "value":... }
//   POST /restart                -> clean restart (relaunches itself)
//   POST /shutdown               -> clean shutdown, same path as the old 'q'+Enter
//   GET  /                       -> www/cam-control-panel.html
//   GET  /<file>                 -> static file from www/
//
// Runs WITHOUT a console window (WIN32_EXECUTABLE, see CMakeLists): on start it
// opens the control panel in the default browser, and the panel's own buttons
// drive restart/shutdown. Diagnostics go to SignalBox.log next to the exe.
//
// Built with the same toolchain as RemoteCli (VS2022, v143, UNICODE).

#include "HttpServer.h"
#include "CameraSession.h"
#include "platform/Platform.h" // всё, что зависит от ОС, — только через этот слой

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace SDK = SCRSDK;
using namespace std::chrono_literals;
using coll::CameraSession;
using coll::CamStatus;

// ---------------- globals ----------------
// g_cams only ever grows (cameras are added by discovery, never removed until
// shutdown), so a CameraSession* stays valid for the process lifetime even
// after the vector reallocates — readers copy raw pointers under a short lock.
static std::vector<std::unique_ptr<CameraSession>> g_cams;
static std::mutex                                  g_camsMutex;    // guards g_cams structure
static std::mutex                                  g_statusMutex;  // guards g_statusJson
static std::string                                 g_statusJson = "{\"cameras\":[]}";

// ---- MIDI input: a control-surface key toggles recording on all cameras ----
static std::mutex              g_midiMx;                 // guards g_midiQ
static std::condition_variable g_midiCv;
static std::deque<unsigned>    g_midiQ;                  // упакованное короткое сообщение

// ---- live view on-demand cache ----
// seq растёт только когда кадр РЕАЛЬНО изменился. Клиент присылает свой seq и
// получает 304 вместо повторной пересылки той же картинки — иначе при высокой
// частоте опроса однопоточный сервер тратил бы всё время на отдачу дублей.
struct LvFrame { std::string jpeg; std::string frames; long long seq = 0; };
static std::mutex               g_lvMutex;
static std::map<int, LvFrame>   g_lvCache;      // camIndex -> latest {jpeg, frames}
static std::map<int, long long> g_lvActiveMs;   // camIndex -> last /liveview request (steady ms)

static long long nowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::atomic<bool>                           g_running{true};
static std::atomic<bool>                           g_restart{false};  // true => drop marker, launcher relaunches

static const unsigned short kPort = 8787;

// ---------------- helpers ----------------
// With no console window the log file is the only place startup diagnostics
// (pairing hints, port conflicts, MIDI list) can be read after the fact.
static std::mutex g_logMutex;
static FILE*      g_logFile = nullptr;

// Consoleful runs print to the console (plat::writeConsole does it as real
// Unicode, независимо от кодовой страницы), а в GUI-подсистеме консоли нет
// вовсе — там всё видно только в логе, поэтому пишем туда всегда.
static void writeConsoleUtf8(const std::string& s) {
    plat::writeConsole(s);
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) { std::fwrite(s.data(), 1, s.size(), g_logFile); std::fflush(g_logFile); }
}

static void consolePrintf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n) : sizeof(buf) - 1;
    writeConsoleUtf8(std::string(buf, len));
}

// Request a full restart. The app relaunches ITSELF on exit (see shutdown),
// so this works no matter how it was started (bat, shortcut to bat, shortcut
// to exe, ...). Ctrl+C / 'q' don't set this, so they just quit.
static void requestRestart() {
    g_restart.store(true);
    g_running.store(false);
}

// Fresh log next to the exe on every start (previous run kept as .log.prev).
static void openLogFile() {
    const std::string cur  = plat::joinPath(plat::exeDir(), "SignalBox.log");
    const std::string prev = plat::joinPath(plat::exeDir(), "SignalBox.log.prev");
    plat::replaceFile(cur, prev);
    // Файл должен остаться читаемым, пока мы в него пишем — без консоли это
    // единственный способ увидеть, что происходит (см. openForSharedWrite).
    FILE* f = plat::openForSharedWrite(cur);
    if (f) {
        // UTF-8 BOM so Notepad shows the Russian messages instead of mojibake.
        static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
        std::fwrite(kBom, 1, sizeof(kBom), f);
        std::fflush(f);
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_logFile = f;
    }
}

// Open the control panel in the default browser. This is the app's only UI:
// there is no console window, so the panel is how the user sees and stops it.
static void openPanelInBrowser() {
    const std::string url = "http://127.0.0.1:" + std::to_string(kPort) + "/";
    // Не открылось — с этим ничего не сделать автоматически, но без консоли
    // сказать об этом можно только в лог.
    if (plat::openUrl(url))
        consolePrintf("Панель открыта в браузере по умолчанию.\n");
    else
        consolePrintf("Не удалось открыть браузер. Открой вручную: %s\n", url.c_str());
}

// ---------------- restart / shutdown safety ----------------
static std::atomic<bool> g_relaunchDone{false};

static void relaunchSelf() {
    if (g_relaunchDone.exchange(true)) return;      // never spawn twice
    consolePrintf("Перезапуск...\n");
    plat::releaseSingleInstance();   // hand the slot over so the fresh copy starts at once
    // --restarted: the panel is already open in the browser, so the fresh
    // instance must not pop up a second tab.
    if (!plat::launch(plat::exePath(), "--restarted", plat::exeDir()))
        consolePrintf("Не удалось перезапуститься.\n");
}

// The worker threads can sit inside blocking SDK calls — Connect() to a camera
// that has not been paired yet blocks for a long time — so joining them can hang
// the whole shutdown. That left zombie SignalBox processes behind: port released,
// process never exiting, cameras still held. Bound the graceful path and, if it
// overruns, take the process down for real.
static void startShutdownWatchdog(int seconds) {
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        consolePrintf("Штатное завершение затянулось (%d с) — принудительный выход.\n", seconds);
        if (g_restart.load()) relaunchSelf();
        plat::removeTrayIcon();          // иначе в трее останется мёртвый значок
        plat::terminateSelf(0);
    }).detach();
}

// ---------------- брандмауэр ----------------
// Камеры отвечают на обнаружение ВХОДЯЩИМ UDP, а панель отдаётся по входящему
// TCP — без разрешения ПК, работающий в одной студии, замолкает в другой.
// Всё системное (COM-интерфейс правил и элевация) живёт в платформенном слое;
// здесь остаётся политика: спросить один раз, запомнить отказ, написать в лог.

// Marker next to the exe: the user said "no", don't nag on every start.
static std::string fwSkipPath() { return plat::joinPath(plat::exeDir(), "firewall-skip.txt"); }

// Normal startup path: ask once, then hand the actual change to Windows' own
// elevation prompt. Never changes anything without both confirmations.
static void maybeOfferFirewallRules() {
    if (!plat::firewallSupported()) return;
    if (plat::fileExists(fwSkipPath())) return;
    if (plat::firewallHasRuleForSelf()) {
        consolePrintf("[firewall] Разрешение уже выдано.\n");
        return;
    }
    consolePrintf("[firewall] Разрешения для SignalBox нет — спрашиваю пользователя.\n");

    const bool agreed = plat::askYesNo("SignalBox — доступ в сеть",
        "Разрешить SignalBox приём подключений в локальной сети?\n\n"
        "Это нужно, чтобы находились камеры и чтобы панель открывалась "
        "с телефона и других компьютеров.\n\n"
        "Разрешение будет выдано для всех типов сетей — иначе в другой студии, "
        "где сеть определится иначе, камеры перестанут находиться.\n\n"
        "Потребуется подтверждение администратора Windows.");

    if (!agreed) {
        static const char kNote[] =
            "\xEF\xBB\xBF"
            "Пользователь отказался добавлять правило брандмауэра.\r\n"
            "Удали этот файл, чтобы SignalBox спросил снова.\r\n";
        plat::writeFile(fwSkipPath(), kNote, sizeof(kNote) - 1);
        consolePrintf("[firewall] Пользователь отказался. Больше не спрашиваю (см. firewall-skip.txt).\n");
        return;
    }

    // Саму правку делает элевированная копия — ОС показывает свой запрос прав.
    consolePrintf(plat::firewallRequestElevatedAdd()
                      ? "[firewall] Правила добавлены (TCP+UDP, все сети).\n"
                      : "[firewall] Добавить правила не удалось.\n");
}

// ---------------- обновление через GitHub Releases ----------------
// Студии получают программу архивом, и менять его вручную неудобно. Спрашиваем у
// GitHub последний релиз, сравниваем версии и ставим — по кнопке или автоматически,
// если включено. Всё на WinHTTP: он встроен в Windows, внешних библиотек не нужно.
// Нет интернета — молча пропускаем, работа офлайн не должна страдать.
static bool        jsonGet(const std::string&, const std::string&, std::string&);   // определены ниже
static std::string jsonEscape(const std::string&);

static const char*    kAppVersion = "1.0.5";
// ЗАПОЛНИТЬ после создания репозитория, формат "владелец/репозиторий".
// Пустая строка = проверка обновлений выключена.
static const char*    kUpdateRepo = "GIDEONSYSTEM/signalbox";

struct UpdateInfo {
    std::string current = kAppVersion;
    std::string latest;          // версия из последнего релиза
    std::string url;             // прямая ссылка на архив
    std::string error;           // текст для панели, если что-то пошло не так
    bool        available = false;
    bool        checked   = false;
};
static std::mutex        g_updMutex;
static UpdateInfo        g_upd;
static std::atomic<bool> g_updBusy{false};      // идёт проверка или установка
static std::atomic<bool> g_autoUpdate{false};

static std::string settingsPath() { return plat::joinPath(plat::exeDir(), "settings.json"); }

static void loadSettings() {
    std::string s;
    if (!plat::readFile(settingsPath(), s)) return;
    std::string v;
    if (jsonGet(s, "autoUpdate", v)) g_autoUpdate.store(v == "true" || v == "1");
}

static void saveSettings() {
    plat::writeFile(settingsPath(),
                    std::string("{\"autoUpdate\":") + (g_autoUpdate.load() ? "true" : "false") + "}\r\n");
}

// "v1.2.10" vs "1.3" -> -1 / 0 / 1. Сравниваем числами по частям, а не строками:
// иначе "1.10" оказалось бы меньше "1.9".
static int cmpVersion(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<int> v;
        std::string cur;
        for (size_t i = (s.size() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '.') { v.push_back(std::atoi(cur.c_str())); cur.clear(); }
            else if (std::isdigit(static_cast<unsigned char>(s[i]))) cur += s[i];
        }
        return v;
    };
    const std::vector<int> x = parts(a), y = parts(b);
    const size_t n = x.size() > y.size() ? x.size() : y.size();
    for (size_t i = 0; i < n; ++i) {
        const int xi = i < x.size() ? x[i] : 0, yi = i < y.size() ? y[i] : 0;
        if (xi != yi) return xi < yi ? -1 : 1;
    }
    return 0;
}

// Спросить у GitHub последний релиз. Разбираем ровно два поля, полноценный JSON-парсер
// ради этого не тащим.
static void checkUpdateOnce(bool announce) {
    if (!kUpdateRepo || !*kUpdateRepo) return;          // репозиторий не настроен
    if (g_updBusy.exchange(true)) return;

    const std::string api = std::string("https://api.github.com/repos/") + kUpdateRepo +
                            "/releases/latest";
    std::string json;
    UpdateInfo info;
    info.current = kAppVersion;

    int status = 0;
    if (!plat::httpGet(api, &json, "", &status)) {
        // 404 у GitHub значит «релизов ещё нет» (или репозитория) — это не сетевая ошибка,
        // и на старте проекта пользователь увидит именно её.
        if (status == 404)      info.error = "релизов пока нет";
        else if (status == 403) info.error = "GitHub временно ограничил запросы";
        else if (status)        info.error = "GitHub ответил " + std::to_string(status);
        else                    info.error = "нет связи с GitHub";
    } else {
        std::string tag;
        jsonGet(json, "tag_name", tag);
        // среди активов выбираем первый .zip
        std::string url;
        const std::string key = "\"browser_download_url\"";
        for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + 1)) {
            const size_t q1 = json.find('"', json.find(':', p) + 1);
            const size_t q2 = (q1 == std::string::npos) ? q1 : json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            const std::string u = json.substr(q1 + 1, q2 - q1 - 1);
            if (u.size() > 4 && _stricmp(u.c_str() + u.size() - 4, ".zip") == 0) { url = u; break; }
        }
        bool tagLooksLikeVersion = false;
        for (unsigned char c : tag) if (std::isdigit(c)) { tagLooksLikeVersion = true; break; }

        if (tag.empty() || url.empty()) {
            info.error = "в релизе нет архива .zip";
        } else if (!tagLooksLikeVersion) {
            // Иначе тег молча посчитался бы нулевой версией и обновления бы «не находились».
            info.error = "тег релиза \"" + tag + "\" не похож на версию — нужно, например, v1.0.1";
        } else {
            info.latest    = tag;
            info.url       = url;
            info.available = cmpVersion(kAppVersion, tag) < 0;
        }
    }
    info.checked = true;
    { std::lock_guard<std::mutex> lk(g_updMutex); g_upd = info; }
    g_updBusy.store(false);

    if (announce) {
        if (!info.error.empty())
            consolePrintf("[update] Проверка не удалась: %s\n", info.error.c_str());
        else if (info.available)
            consolePrintf("[update] Доступна версия %s (сейчас %s).\n", info.latest.c_str(), kAppVersion);
        else
            consolePrintf("[update] Установлена последняя версия (%s).\n", kAppVersion);
    }
}

static std::string updateStagingDir() {
    return plat::joinPath(plat::tempDir(), "SignalBox-update");
}

// Скачать архив, распаковать и запустить распакованную копию в режиме --apply-update:
// она дождётся нашего выхода, заменит файлы и стартует уже обновлённую программу.
// Так не приходится перезаписывать файлы, которые сейчас заняты (exe и DLL Sony).
static bool startUpdateInstall(std::string& err) {
    UpdateInfo info;
    { std::lock_guard<std::mutex> lk(g_updMutex); info = g_upd; }
    if (!info.available || info.url.empty()) { err = "обновление недоступно"; return false; }
    if (g_updBusy.exchange(true))            { err = "обновление уже идёт";  return false; }

    const std::string stage = updateStagingDir();
    const std::string zip   = plat::joinPath(stage, "update.zip");
    const std::string src   = plat::joinPath(stage, "SignalBox");   // внутри архива папка SignalBox\

    bool ok = false;
    do {
        // чистая площадка
        plat::removeTree(stage);
        if (!plat::makeDir(stage)) { err = "не удалось создать временную папку"; break; }

        consolePrintf("[update] Качаю %s\n", info.url.c_str());
        if (!plat::httpGet(info.url, nullptr, zip, nullptr)) {
            err = "не удалось скачать архив"; break;
        }

        if (!plat::extractArchive(zip, stage) || !plat::fileExists(plat::joinPath(src, "SignalBox.exe"))) {
            err = "архив распаковался неправильно"; break;
        }

        const std::string args = "--apply-update \"" + plat::exeDir() + "\" " +
                                 std::to_string(plat::currentPid());
        if (!plat::launch(plat::joinPath(src, "SignalBox.exe"), args, src)) {
            err = "не удалось запустить установщик"; break;
        }
        consolePrintf("[update] Ставлю версию %s, выключаюсь...\n", info.latest.c_str());
        ok = true;
    } while (false);

    g_updBusy.store(false);
    if (ok) g_running.store(false);        // отпускаем файлы: установщик ждёт нашего выхода
    return ok;
}

// Режим установщика: ждём выхода старого процесса, копируем файлы, запускаем обновлённую копию.
static int applyUpdate(const std::string& cmdLine) {
    // ... --apply-update "<target>" <pid>
    const size_t k = cmdLine.find("--apply-update");
    if (k == std::string::npos) return 1;
    size_t q1 = cmdLine.find('"', k);
    size_t q2 = (q1 == std::string::npos) ? q1 : cmdLine.find('"', q1 + 1);
    if (q2 == std::string::npos) return 1;
    const std::string target = cmdLine.substr(q1 + 1, q2 - q1 - 1);
    const unsigned long pid = std::strtoul(cmdLine.c_str() + q2 + 1, nullptr, 10);

    plat::waitForProcess(pid, 30000);
    std::this_thread::sleep_for(700ms);           // дать ОС отпустить DLL

    // Копируем поверх, не удаляя лишнее в приёмнике: данные установки
    // (cameras.txt, groups.json, settings.json) в архив не входят и должны пережить обновление.
    if (!plat::copyTree(plat::joinPath(updateStagingDir(), "SignalBox"), target)) return 1;

    plat::launch(plat::joinPath(target, "SignalBox.exe"), "", target);
    return 0;
}

static std::string updateJson() {
    UpdateInfo i;
    { std::lock_guard<std::mutex> lk(g_updMutex); i = g_upd; }
    std::string j = "{\"current\":\"" + jsonEscape(i.current) + "\"";
    j += ",\"latest\":\""    + jsonEscape(i.latest) + "\"";
    j += ",\"available\":"   + std::string(i.available ? "true" : "false");
    j += ",\"checked\":"     + std::string(i.checked ? "true" : "false");
    j += ",\"busy\":"        + std::string(g_updBusy.load() ? "true" : "false");
    j += ",\"auto\":"        + std::string(g_autoUpdate.load() ? "true" : "false");
    j += ",\"configured\":"  + std::string((kUpdateRepo && *kUpdateRepo) ? "true" : "false");
    j += ",\"error\":\""     + jsonEscape(i.error) + "\"}";
    return j;
}

// ---------------- single instance ----------------
// Several copies (dev build, an unpacked portable folder, a stale launch) all
// fight for port 8787: whichever binds first serves ITS OWN www/, so the panel
// can silently come from an outdated folder. One instance only.
static const char* kInstanceName = "SignalBox_SingleInstance";

// Имя этого ПК показывается в подсказке про связывание: на камере в списке
// устройств пользователь ищет именно его.
static std::string pcName() {
    const std::string n = plat::computerName();
    return n.empty() ? std::string("(этот ПК)") : n;
}

// Rank an IPv4 so we pick the real home/LAN address (what a phone uses), not a
// VPN/virtual adapter (e.g. 198.18.x test range) or link-local/loopback.
static int ipScore(const std::string& ip) {
    auto starts = [&](const char* p){ return ip.rfind(p, 0) == 0; };
    if (starts("192.168.")) return 100;
    if (starts("10."))      return 90;
    if (starts("172.")) { int o2 = std::atoi(ip.c_str() + 4); if (o2 >= 16 && o2 <= 31) return 80; }
    if (starts("169.254.")) return 5;    // link-local
    if (starts("198.18.") || starts("198.19.")) return 2;   // RFC2544 test range (VPN/virtual)
    if (starts("127."))     return 0;    // loopback
    return 40;
}

// Fallback when the adapter table is unavailable: every IPv4 this host answers
// with, ranked by address range only.
static std::string localIPv4ByHostname() {
    std::string best; int bestScore = -1;
    for (const std::string& ip : plat::hostIPv4Addresses()) {
        const int sc = ipScore(ip);
        if (sc > bestScore) { bestScore = sc; best = ip; }
    }
    return best;
}

// The LAN address other devices use to reach this panel: it fills status.json's
// "server", which drives both the address line and the QR code.
//
// Deliberately NOT the "route to the internet" (a UDP connect to 8.8.8.8): on a PC
// with a VPN that probe returns the tunnel address, which no phone on the studio
// LAN can reach — measured here, it picked a VPN's 172.18.0.1 over the real
// 192.168.0.176. Instead rank the actual adapter table, where a virtual/tunnel
// adapter is plainly distinguishable: it reports a virtual IfType and has no real
// default gateway, while the physical NIC on the camera LAN has one.
// Every candidate and its weight is logged, so a bad pick elsewhere is diagnosable.
static std::string localIPv4() {
    static const std::string cached = []() -> std::string {
        const std::vector<plat::NetInterface> ifaces = plat::listIPv4Interfaces();

        std::string best;
        int bestScore = -1000000;
        if (ifaces.empty()) {
            consolePrintf("[net] Список сетевых интерфейсов пуст или недоступен.\n");
        }
        for (const plat::NetInterface& n : ifaces) {
            int sc = ipScore(n.ip);
            if (n.virtualIf)  sc -= 60;      // VPN / Hyper-V / Docker style adapter
            if (n.hasGateway) sc += 25;      // has a way off its own subnet -> a real LAN
            consolePrintf("[net]   %s — %s (тип %u%s%s) -> вес %d\n",
                          n.name.c_str(), n.ip.c_str(), n.type,
                          n.virtualIf ? ", виртуальный" : "",
                          n.hasGateway ? ", шлюз есть" : ", без шлюза", sc);
            if (sc > bestScore) { bestScore = sc; best = n.ip; }
        }

        if (best.empty()) {
            best = localIPv4ByHostname();
            consolePrintf("[net] Запасной способ определения адреса: %s\n",
                          best.empty() ? "(не найден)" : best.c_str());
        }
        consolePrintf("[net] Для доступа с других устройств выбран: %s\n",
                      best.empty() ? "(нет — шаринг недоступен)" : best.c_str());
        return best;
    }();
    return cached;
}

static std::string jsonEscape(const std::string& in) {
    std::string o; o.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += c;
        }
    }
    return o;
}

// Minimal JSON scalar extractor for our well-known /cmd bodies.
static bool jsonGet(const std::string& body, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return false;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && (body[p]==' '||body[p]=='\t'||body[p]=='\n'||body[p]=='\r')) ++p;
    if (p >= body.size()) return false;
    if (body[p] == '"') {
        size_t q = body.find('"', p + 1);
        if (q == std::string::npos) return false;
        out = body.substr(p + 1, q - p - 1);
    } else {
        size_t q = p;
        while (q < body.size() &&
               body[q]!=','&&body[q]!='}'&&body[q]!=' '&&body[q]!='\r'&&body[q]!='\n'&&body[q]!='\t')
            ++q;
        out = body.substr(p, q - p);
    }
    return true;
}

static std::string contentTypeFor(const std::string& path) {
    auto ends = [&](const char* s){ size_t n = std::strlen(s);
        return path.size() >= n && _stricmp(path.c_str() + path.size() - n, s) == 0; };
    if (ends(".html") || ends(".htm")) return "text/html; charset=utf-8";
    if (ends(".js"))   return "application/javascript; charset=utf-8";
    if (ends(".css"))  return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".png"))  return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".svg"))  return "image/svg+xml";
    if (ends(".ico"))  return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";      // www/fonts/ — panel ships its own fonts
    if (ends(".woff"))  return "font/woff";
    return "application/octet-stream";
}

static bool readFileBinary(const std::string& full, std::string& out) {
    return plat::readFile(full, out);
}

// Map raw SDK model names to friendly display names.
static std::string friendlyModel(const std::string& m) {
    if (m == "ZV-E10M2") return "ZV-E10 II";   // SDK reports it as "ZV-E10M2"
    return m;
}

// Serialize a selectable property: {"cur":N|null,"opts":[[enc,"label"],...]}.
static std::string propOptsJson(const coll::CamPropOpts& p) {
    std::string j = "{\"cur\":" + (p.cur < 0 ? std::string("null") : std::to_string(p.cur)) + ",\"opts\":[";
    for (size_t k = 0; k < p.opts.size(); ++k) {
        if (k) j += ",";
        j += "[" + std::to_string(p.opts[k].first) + ",\"" + jsonEscape(p.opts[k].second) + "\"]";
    }
    j += "]}";
    return j;
}

// Build /status.json from the live cameras (dynamic count). Reads each camera's
// status fresh. Called by the poll thread once a second.
static std::string buildStatusJson() {
    std::vector<CameraSession*> cams;
    { std::lock_guard<std::mutex> lk(g_camsMutex);
      cams.reserve(g_cams.size());
      for (auto& c : g_cams) cams.push_back(c.get()); }

    // LAN-адрес пульта для шаринга на другие устройства (кэшируем — считаем 1 раз).
    static const std::string server = []() {
        std::string lan = localIPv4();
        return lan.empty() ? std::string() : ("http://" + lan + ":" + std::to_string(kPort) + "/");
    }();

    std::string j = "{\"server\":\"" + server + "\",\"cameras\":[";
    for (size_t i = 0; i < cams.size(); ++i) {
        CamStatus s = cams[i]->readStatus();
        if (i) j += ",";
        j += "{";
        j += "\"id\":\""    + jsonEscape(cams[i]->idLabel()) + "\",";
        j += "\"model\":\"" + jsonEscape(friendlyModel(cams[i]->modelUtf8())) + "\",";
        j += "\"ip\":\""    + jsonEscape(cams[i]->ipUtf8()) + "\",";
        j += "\"online\":"  + std::string(s.online ? "true" : "false") + ",";
        j += "\"rec\":"     + std::string(s.rec ? "true" : "false") + ",";
        j += "\"battery\":" + (s.battery < 0 ? std::string("null") : std::to_string(s.battery)) + ",";
        j += "\"acPower\":" + std::string(s.acPower ? "true" : "false") + ",";
        j += "\"cardMinutes\":" + (s.cardMinutes < 0 ? std::string("null") : std::to_string(s.cardMinutes)) + ",";
        j += "\"writing\":" + std::string(s.writing ? "true" : "false") + ",";
        j += "\"iso\":"     + (s.iso.empty() ? std::string("null") : "\"" + jsonEscape(s.iso) + "\"") + ",";
        j += "\"isoEff\":"  + (s.isoEff < 0 ? std::string("null") : std::to_string(s.isoEff)) + ",";
        j += "\"aperture\":" + propOptsJson(s.aperture) + ",";
        j += "\"shutter\":"  + propOptsJson(s.shutter) + ",";
        j += "\"wb\":"       + propOptsJson(s.wb) + ",";
        j += "\"wbKelvin\":" + (s.wbKelvin < 0 ? std::string("null") : std::to_string(s.wbKelvin)) + ",";
        j += "\"micGain\":"  + propOptsJson(s.micGain);
        j += "}";
    }
    j += "]}";
    return j;
}

// ---------------- manual cameras (cameras.txt) ----------------
// Auto-discovery is SSDP multicast, and plenty of studio networks drop it: a
// separate 2.4 GHz / guest SSID, client isolation, or an AP that will not bridge
// multicast between bands. The camera is then perfectly reachable by IP yet stays
// invisible to EnumCameraObjects. cameras.txt names such cameras directly —
// CreateCameraObjectInfoEthernetConnection builds a camera object from IP+MAC
// with no enumeration at all.
struct ManualCam {
    std::string                    ip;
    std::string                    modelName;
    SDK::CrCameraDeviceModelList   model;
    bool                           ssh = false;
};

// Models offerable in the panel. `token` is what goes into cameras.txt — it must
// stay space-free, the file format splits on whitespace. `label` is what the user
// picks in the drop-down.
struct ModelRow {
    const char*                  token;
    const char*                  label;
    SDK::CrCameraDeviceModelList model;
};
static const ModelRow kModels[] = {
    {"ZV-E1",    "ZV-E1",     SDK::CrCameraDeviceModel_ZV_E1},
    {"ZV-E10M2", "ZV-E10 II", SDK::CrCameraDeviceModel_ZV_E10M2},
    {"FX3",      "FX3",       SDK::CrCameraDeviceModel_ILME_FX3},
    {"FX3A",     "FX3A",      SDK::CrCameraDeviceModel_ILME_FX3A},
    {"FX30",     "FX30",      SDK::CrCameraDeviceModel_ILME_FX30},
    {"FX6",      "FX6",       SDK::CrCameraDeviceModel_ILME_FX6},
    {"FX2",      "FX2",       SDK::CrCameraDeviceModel_ILME_FX2},
    {"A1",       "A1",        SDK::CrCameraDeviceModel_ILCE_1},
    {"A7M4",     "A7 IV",     SDK::CrCameraDeviceModel_ILCE_7M4},
    {"A7SM3",    "A7S III",   SDK::CrCameraDeviceModel_ILCE_7SM3},
    {"A7RM5",    "A7R V",     SDK::CrCameraDeviceModel_ILCE_7RM5},
    {"A9M3",     "A9 III",    SDK::CrCameraDeviceModel_ILCE_9M3},
    {"A6700",    "A6700",     SDK::CrCameraDeviceModel_ILCE_6700},
};

static std::string modelKeyOf(const std::string& raw) {
    std::string k;
    for (unsigned char c : raw)
        if (std::isalnum(c)) k += static_cast<char>(std::toupper(c));
    return k;
}

// Accepts the canonical token plus the obvious human spellings ("ZV-E10 II",
// "ilce-7m4", "zv_e1"), so a hand-edited cameras.txt keeps working.
static bool modelFromName(const std::string& raw, SDK::CrCameraDeviceModelList& out) {
    const std::string k = modelKeyOf(raw);
    for (const ModelRow& r : kModels)
        if (k == modelKeyOf(r.token) || k == modelKeyOf(r.label)) { out = r.model; return true; }
    struct Alias { const char* key; SDK::CrCameraDeviceModelList model; };
    static const Alias kAliases[] = {
        {"ZVE10II",  SDK::CrCameraDeviceModel_ZV_E10M2},
        {"ILMEFX3",  SDK::CrCameraDeviceModel_ILME_FX3},
        {"ILMEFX30", SDK::CrCameraDeviceModel_ILME_FX30},
        {"ILCE7M4",  SDK::CrCameraDeviceModel_ILCE_7M4},
        {"ILCE7SM3", SDK::CrCameraDeviceModel_ILCE_7SM3},
        {"ILCE7RM5", SDK::CrCameraDeviceModel_ILCE_7RM5},
        {"ILCE9M3",  SDK::CrCameraDeviceModel_ILCE_9M3},
        {"ILCE6700", SDK::CrCameraDeviceModel_ILCE_6700},
    };
    for (const Alias& a : kAliases) if (k == a.key) { out = a.model; return true; }
    return false;
}

// Canonical token for storing in cameras.txt (so the panel always writes the
// spelling the parser prefers).
static bool modelTokenFor(const std::string& raw, std::string& token) {
    const std::string k = modelKeyOf(raw);
    for (const ModelRow& r : kModels)
        if (k == modelKeyOf(r.token) || k == modelKeyOf(r.label)) { token = r.token; return true; }
    SDK::CrCameraDeviceModelList m;
    if (!modelFromName(raw, m)) return false;
    for (const ModelRow& r : kModels) if (r.model == m) { token = r.token; return true; }
    return false;
}

// Real MAC via ARP when the camera answers; otherwise a value derived from the
// IP. Per the SDK docs the MAC is only an identifier for the camera object and
// need not match the body — it just has to be unique per object.
static void macForIp(CrInt32u ipLe, CrInt8u out[6]) {
    if (plat::arpMac(static_cast<uint32_t>(ipLe), out)) return;
    out[0] = 0x02; out[1] = 0x00;                       // locally-administered
    std::memcpy(out + 2, &ipLe, 4);
}

static std::string manualCamsPath() { return plat::joinPath(plat::exeDir(), "cameras.txt"); }

static void ensureManualCamsFile() {
    const std::string p = manualCamsPath();
    if (plat::fileExists(p)) return;
    static const char kTemplate[] =
        "\xEF\xBB\xBF"
        "# Камеры, добавляемые вручную — по IP, без автопоиска.\r\n"
        "#\r\n"
        "# Нужно только если камера НЕ появляется сама. Обычная причина:\r\n"
        "# автопоиск идёт мультикастом, а сеть его не пропускает (отдельная\r\n"
        "# гостевая сеть или SSID 2.4 ГГц, изоляция клиентов на роутере).\r\n"
        "# Признак: с ПК проходит ping до камеры, но в панели её нет.\r\n"
        "#\r\n"
        "# Формат:  IP  МОДЕЛЬ  [ssh]\r\n"
        "#   IP     — адрес камеры: Меню -> Сеть -> Wi-Fi -> Отобр. инф. Wi-Fi\r\n"
        "#   МОДЕЛЬ — ZV-E1, ZV-E10M2, FX3, FX30, FX6, FX2, A1, A7M4, A7SM3,\r\n"
        "#            A7RM5, A9M3, A6700\r\n"
        "#   ssh    — добавь это слово, ТОЛЬКО если на камере включена\r\n"
        "#            аутентификация доступа (Меню -> Сеть -> Опции сети ->\r\n"
        "#            Настр. аутент. доступа). Без неё слово не нужно.\r\n"
        "#\r\n"
        "# Примеры (убери # в начале строки, чтобы включить):\r\n"
        "# 192.168.1.55  ZV-E1\r\n"
        "# 192.168.1.56  ZV-E10M2\r\n"
        "#\r\n"
        "# Файл перечитывается при каждом сканировании сети — правку\r\n"
        "# подхватит без перезапуска.\r\n";
    plat::writeFile(p, kTemplate, sizeof(kTemplate) - 1);
}

static std::vector<ManualCam> readManualCams() {
    std::vector<ManualCam> out;
    std::string text;
    if (!plat::readFile(manualCamsPath(), text)) return out;
    std::istringstream f(text);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF)
            line.erase(0, 3);                                   // UTF-8 BOM
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream is(line);
        std::string ip, model, flag;
        if (!(is >> ip >> model)) continue;
        ManualCam mc;
        mc.ip        = ip;
        mc.modelName = model;
        if (!modelFromName(model, mc.model)) {
            consolePrintf("[cameras.txt] %s: неизвестная модель \"%s\" — строка пропущена.\n",
                          ip.c_str(), model.c_str());
            continue;
        }
        while (is >> flag) if (flag == "ssh" || flag == "SSH") mc.ssh = true;
        out.push_back(mc);
    }
    return out;
}

// cameras.txt is edited from the panel as well as by hand, and the discovery
// thread re-reads it every scan — serialise all of that.
static std::mutex g_manualFileMutex;
// Set when the list changes so the discovery loop stops waiting out its 5s sleep
// and picks the camera up straight away.
static std::atomic<bool> g_rescanNow{false};

static std::vector<std::string> manualFileLines() {
    std::vector<std::string> lines;
    std::string text;
    if (!plat::readFile(manualCamsPath(), text)) return lines;
    std::istringstream f(text);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool writeManualFile(const std::vector<std::string>& lines) {
    std::string out = "\xEF\xBB\xBF";                           // keep it Notepad-friendly
    for (std::string s : lines) {
        if (!s.empty() && static_cast<unsigned char>(s[0]) == 0xEF && s.size() >= 3)
            s.erase(0, 3);                                       // strip an inherited BOM
        out += s;
        out += "\r\n";
    }
    return plat::writeFile(manualCamsPath(), out);
}

// First whitespace-separated token of a non-comment line, "" for comments/blanks.
static std::string entryIpOf(const std::string& line) {
    std::string l = line;
    if (!l.empty() && static_cast<unsigned char>(l[0]) == 0xEF && l.size() >= 3) l.erase(0, 3);
    const size_t hash = l.find('#');
    if (hash != std::string::npos) l.erase(hash);
    std::istringstream is(l);
    std::string ip;
    return (is >> ip) ? ip : std::string();
}

// err is a ready-to-show Russian message.
static bool manualAdd(const std::string& ip, const std::string& model, bool ssh, std::string& err) {
    uint32_t probe = 0;
    if (!plat::ipv4ToNumber(ip, probe)) { err = "Это не похоже на IP-адрес"; return false; }
    std::string token;
    if (!modelTokenFor(model, token)) { err = "Неизвестная модель камеры"; return false; }

    std::lock_guard<std::mutex> lk(g_manualFileMutex);
    std::vector<std::string> lines = manualFileLines();
    for (const std::string& l : lines)
        if (entryIpOf(l) == ip) { err = "Камера с таким адресом уже добавлена"; return false; }
    lines.push_back(ip + "  " + token + (ssh ? "  ssh" : ""));
    if (!writeManualFile(lines)) { err = "Не удалось записать cameras.txt"; return false; }
    g_rescanNow.store(true);
    consolePrintf("[cameras.txt] Добавлено из панели: %s %s%s\n",
                  ip.c_str(), token.c_str(), ssh ? " ssh" : "");
    return true;
}

static bool manualRemove(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_manualFileMutex);
    std::vector<std::string> lines = manualFileLines();
    std::vector<std::string> kept;
    bool removed = false;
    for (const std::string& l : lines) {
        if (entryIpOf(l) == ip) { removed = true; continue; }
        kept.push_back(l);
    }
    if (!removed) return false;
    if (!writeManualFile(kept)) return false;
    consolePrintf("[cameras.txt] Удалено из панели: %s\n", ip.c_str());
    return true;
}

// GET /cameras.json — model list for the drop-down + what is in cameras.txt now.
static std::string manualCamsJson() {
    std::string j = "{\"models\":[";
    for (size_t i = 0; i < sizeof(kModels) / sizeof(kModels[0]); ++i) {
        if (i) j += ",";
        j += "{\"token\":\"" + jsonEscape(kModels[i].token) +
             "\",\"label\":\"" + jsonEscape(kModels[i].label) + "\"}";
    }
    j += "],\"manual\":[";
    std::vector<ManualCam> cams;
    { std::lock_guard<std::mutex> lk(g_manualFileMutex); cams = readManualCams(); }
    for (size_t i = 0; i < cams.size(); ++i) {
        if (i) j += ",";
        std::string token;
        if (!modelTokenFor(cams[i].modelName, token)) token = cams[i].modelName;
        j += "{\"ip\":\"" + jsonEscape(cams[i].ip) +
             "\",\"model\":\"" + jsonEscape(token) +
             "\",\"ssh\":" + (cams[i].ssh ? "true" : "false") + "}";
    }
    j += "]}";
    return j;
}

// Add cameras listed in cameras.txt that aren't tracked yet. Mirrors
// discoverOnce(): registers only, connecting stays with the discovery thread.
static void addManualCamsOnce(bool announce) {
    static std::mutex               s_warnMutex;
    static std::map<std::string,int> s_warned;      // don't repeat errors every scan

    std::vector<ManualCam> entries;
    { std::lock_guard<std::mutex> lk(g_manualFileMutex); entries = readManualCams(); }

    for (const ManualCam& mc : entries) {
        uint32_t addr = 0;
        if (!plat::ipv4ToNumber(mc.ip, addr)) {
            std::lock_guard<std::mutex> lk(s_warnMutex);
            if (s_warned[mc.ip]++ == 0)
                consolePrintf("[cameras.txt] \"%s\" — это не IP-адрес.\n", mc.ip.c_str());
            continue;
        }
        // Порядок байт для SDK: 1-й октет -> биты 7..0 ... 4-й -> 31..24
        // (192.168.0.5 = 0x0500A8C0) — ровно это отдаёт plat::ipv4ToNumber.
        const CrInt32u ipLe = static_cast<CrInt32u>(addr);

        {
            std::lock_guard<std::mutex> lk(g_camsMutex);
            bool exists = false;
            for (auto& c : g_cams) if (c->ipUtf8() == mc.ip) { exists = true; break; }
            if (exists) continue;
        }

        CrInt8u mac[6] = {0};
        macForIp(ipLe, mac);

        SDK::ICrCameraObjectInfo* info = nullptr;
        const SDK::CrError err = SDK::CreateCameraObjectInfoEthernetConnection(
            &info, mc.model, ipLe, mac,
            mc.ssh ? SDK::CrSSHsupport_ON : SDK::CrSSHsupport_OFF);
        if (CR_FAILED(err) || !info) {
            std::lock_guard<std::mutex> lk(s_warnMutex);
            if (s_warned[mc.ip]++ == 0)
                consolePrintf("[cameras.txt] %s (%s): SDK не создал камеру (0x%08X).\n",
                              mc.ip.c_str(), mc.modelName.c_str(), static_cast<unsigned>(err));
            continue;
        }

        std::string fm;
        {
            std::lock_guard<std::mutex> lk(g_camsMutex);
            int idx = static_cast<int>(g_cams.size()) + 1;
            auto cam = std::make_unique<CameraSession>(idx, info);
            fm = friendlyModel(cam->modelUtf8());
            g_cams.push_back(std::move(cam));
        }
        info->Release();
        if (announce)
            consolePrintf("Камера из cameras.txt: %s (%s)\n", fm.c_str(), mc.ip.c_str());
    }
}

// ---------------- known cameras (auto-remembered) ----------------
// Discovery is SSDP multicast: unacknowledged, never retransmitted, and the first
// thing a noisy 2.4 GHz band drops (a 20/40 MHz AP in a busy room is enough).
// Unicast survives, so a camera that answered once stays perfectly reachable —
// it just stops being *announced*. So: remember every camera discovery ever found
// and, when it goes missing, reconnect straight to it by IP. Auto-discovery keeps
// running; it simply stops being the only way in.
struct KnownCam { std::string mac, ip, model; };

static std::mutex g_knownMutex;
static std::string knownCamsPath() { return plat::joinPath(plat::exeDir(), "cameras-known.txt"); }

// "aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF", "aa:bb:..." -> "AABBCCDDEEFF"
static std::string normMac(const std::string& raw) {
    std::string k;
    for (unsigned char c : raw)
        if (std::isxdigit(c)) k += static_cast<char>(std::toupper(c));
    return k;
}

static std::string macToText(const unsigned char b[6]) {
    char t[32];
    std::snprintf(t, sizeof(t), "%02X-%02X-%02X-%02X-%02X-%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    return t;
}

// Strict: false when the address does not answer ARP, i.e. nothing is there.
// Keeps us from creating sessions for cameras that are simply switched off.
static bool arpLookup(CrInt32u ipLe, std::string& macOut) {
    unsigned char mac[6] = {0};
    if (!plat::arpMac(static_cast<uint32_t>(ipLe), mac)) return false;
    macOut = macToText(mac);
    return true;
}

static std::vector<KnownCam> readKnownCams() {
    std::vector<KnownCam> out;
    std::string text;
    if (!plat::readFile(knownCamsPath(), text)) return out;
    std::istringstream f(text);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream is(line);
        KnownCam k;
        if (!(is >> k.mac >> k.ip >> k.model)) continue;
        out.push_back(k);
    }
    return out;
}

static void writeKnownCams(const std::vector<KnownCam>& cams) {
    static const char kHead[] =
        "\xEF\xBB\xBF"
        "# Этот файл SignalBox ведёт сам — править не нужно.\r\n"
        "# Сюда попадают камеры, которые хоть раз нашлись автопоиском.\r\n"
        "# Если потом автопоиск их не увидит (сеть режет широковещание —\r\n"
        "# частый случай при ширине канала 20/40 МГц на 2.4 ГГц), SignalBox\r\n"
        "# подключится к ним напрямую по сохранённому адресу.\r\n"
        "# Формат: MAC  IP  МОДЕЛЬ\r\n";
    std::string out(kHead, sizeof(kHead) - 1);
    for (const KnownCam& k : cams)
        out += k.mac + "  " + k.ip + "  " + k.model + "\r\n";
    plat::writeFile(knownCamsPath(), out);
}

// Called whenever discovery sees a camera: upsert by MAC (or IP when no MAC).
static void rememberCamera(const std::string& mac, const std::string& ip, const std::string& model) {
    if (ip.empty() || model.empty()) return;
    std::lock_guard<std::mutex> lk(g_knownMutex);
    std::vector<KnownCam> cams = readKnownCams();
    const std::string key = normMac(mac);
    bool changed = false, hit = false;
    for (KnownCam& k : cams) {
        const bool same = key.empty() ? (k.ip == ip) : (normMac(k.mac) == key);
        if (!same) continue;
        hit = true;
        if (k.ip != ip || k.model != model) { k.ip = ip; k.model = model; changed = true; }
        break;
    }
    if (!hit) {
        cams.push_back({mac.empty() ? std::string("-") : mac, ip, model});
        changed = true;
    }
    if (changed) writeKnownCams(cams);
}

// Reconnect remembered cameras that discovery did not report this round.
static void reconnectKnownOnce(bool announce) {
    std::vector<KnownCam> known;
    { std::lock_guard<std::mutex> lk(g_knownMutex); known = readKnownCams(); }
    if (known.empty()) return;

    static std::mutex               s_warnMutex;
    static std::map<std::string,int> s_warned;

    for (const KnownCam& k : known) {
        // already tracked (by MAC, else by IP)?
        {
            std::lock_guard<std::mutex> lk(g_camsMutex);
            bool have = false;
            for (auto& c : g_cams) {
                const std::string cm = normMac(c->macUtf8());
                if ((!cm.empty() && cm == normMac(k.mac)) || c->ipUtf8() == k.ip) { have = true; break; }
            }
            if (have) continue;
        }

        // Where is it now? Remembered address first, then look the MAC up.
        std::string ip = k.ip, mac;
        uint32_t addr = 0;
        bool reachable = plat::ipv4ToNumber(ip, addr) && arpLookup(static_cast<CrInt32u>(addr), mac);
        if (!reachable && normMac(k.mac).size() == 12) {
            std::string moved;
            if (plat::ipForMac(k.mac, moved) && plat::ipv4ToNumber(moved, addr) &&
                arpLookup(static_cast<CrInt32u>(addr), mac)) {
                ip = moved;
                reachable = true;
                consolePrintf("[known] %s сменила адрес: %s -> %s\n", k.model.c_str(), k.ip.c_str(), ip.c_str());
            }
        }
        if (!reachable) continue;                 // выключена или не в этой сети — молча ждём

        SDK::CrCameraDeviceModelList model;
        if (!modelFromName(k.model, model)) {
            std::lock_guard<std::mutex> lk(s_warnMutex);
            if (s_warned["m:" + k.model]++ == 0)
                consolePrintf("[known] Модель \"%s\" не поддержана для подключения по адресу.\n", k.model.c_str());
            continue;
        }

        CrInt8u macBytes[6] = {0};
        macForIp(static_cast<CrInt32u>(addr), macBytes);
        SDK::ICrCameraObjectInfo* info = nullptr;
        const SDK::CrError err = SDK::CreateCameraObjectInfoEthernetConnection(
            &info, model, static_cast<CrInt32u>(addr), macBytes, SDK::CrSSHsupport_OFF);
        if (CR_FAILED(err) || !info) {
            std::lock_guard<std::mutex> lk(s_warnMutex);
            if (s_warned["e:" + ip]++ == 0)
                consolePrintf("[known] %s (%s): SDK не создал камеру (0x%08X).\n",
                              k.model.c_str(), ip.c_str(), static_cast<unsigned>(err));
            continue;
        }
        std::string fm;
        {
            std::lock_guard<std::mutex> lk(g_camsMutex);
            int idx = static_cast<int>(g_cams.size()) + 1;
            auto cam = std::make_unique<CameraSession>(idx, info);
            fm = friendlyModel(cam->modelUtf8());
            g_cams.push_back(std::move(cam));
        }
        info->Release();
        if (announce)
            consolePrintf("Автопоиск не увидел %s (%s) — подключаюсь по сохранённому адресу.\n",
                          fm.c_str(), ip.c_str());
    }
}

// ---------------- camera groups ----------------
// Shooting halls: a studio runs several rooms with 3-5 cameras each. The panel
// owns the schema ({"groups":[{id,name,cams:[ip,...]}]}); we only persist it, so
// the OBS dock and a phone always see the same layout. Membership is keyed by IP
// because "CAM N" is assigned in discovery order and shuffles between runs.
static std::mutex g_groupsMutex;

static std::string groupsPath() { return plat::joinPath(plat::exeDir(), "groups.json"); }

static std::string readGroupsJson() {
    std::lock_guard<std::mutex> lk(g_groupsMutex);
    std::string s;
    if (!plat::readFile(groupsPath(), s)) return "{\"groups\":[]}";
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);   // BOM
    if (s.find('{') == std::string::npos) return "{\"groups\":[]}";
    return s;
}

static bool writeGroupsJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_groupsMutex);
    return plat::writeFile(groupsPath(), json);
}

// Enumerate the network and add any camera not already tracked. Safe to call
// repeatedly; cameras in a studio come online at different times.
static void discoverOnce(bool announce) {
    SDK::ICrEnumCameraObjectInfo* list = nullptr;
    if (CR_FAILED(SDK::EnumCameraObjects(&list, 2)) || !list) {   // 2s scan; periodic re-scan compensates
        if (list) list->Release();
        return;
    }
    CrInt32u n = list->GetCount();
    for (CrInt32u i = 0; i < n; ++i) {
        const SDK::ICrCameraObjectInfo* info = list->GetCameraObjectInfo(i);
        std::string mac = plat::utf8From(info->GetMACAddressChar());
        std::string ip  = plat::utf8From(info->GetIPAddressChar());
        std::string keyNew = mac.empty() ? ip : mac;
        // Запомнить: если в другой раз автопоиск её не увидит, подключимся по адресу.
        rememberCamera(mac, ip, plat::utf8From(info->GetModel()));

        std::string fm, where;
        bool addedNew = false;
        {
            std::lock_guard<std::mutex> lk(g_camsMutex);
            bool exists = false;
            for (auto& c : g_cams) {
                std::string k = c->macUtf8().empty() ? c->ipUtf8() : c->macUtf8();
                if (!keyNew.empty() && k == keyNew) { exists = true; break; }
            }
            if (exists) continue;
            int idx = static_cast<int>(g_cams.size()) + 1;
            auto cam = std::make_unique<CameraSession>(idx, info);
            fm = friendlyModel(cam->modelUtf8());
            where = ip.empty() ? mac : ip;
            g_cams.push_back(std::move(cam));
            addedNew = true;
        }
        // Connecting is done (serialized) by the discovery thread, not here, so
        // we never fire several Connect handshakes at once.
        if (addedNew && announce)
            consolePrintf("Обнаружена камера: %s (%s)\n", fm.c_str(), where.c_str());
    }
    list->Release();
}

// Console summary of which cameras are connected and which still need the
// pairing confirmation on the camera body. Called from the discovery thread;
// prints only when the connected-set changes, so it appears once at startup and
// again each time a camera is paired/dropped.
static void maybePrintCamSummary() {
    static std::string s_lastSig;
    struct Row { int idx; std::string model; std::string where; bool conn; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(g_camsMutex);
        for (auto& c : g_cams) {
            std::string where = c->ipUtf8().empty() ? c->macUtf8() : c->ipUtf8();
            rows.push_back({ c->index(), friendlyModel(c->modelUtf8()), where, c->isConnected() });
        }
    }
    if (rows.empty()) return;

    std::string sig;
    for (auto& r : rows) sig += std::to_string(r.idx) + (r.conn ? "+" : "-");
    if (sig == s_lastSig) return;     // nothing changed since last print
    s_lastSig = sig;

    int connected = 0, waiting = 0;
    for (auto& r : rows) (r.conn ? connected : waiting)++;
    consolePrintf("\n--- Камеры: подключено %d, ждут связывания %d ---\n", connected, waiting);
    const std::string pc = pcName();
    for (auto& r : rows) {
        if (r.conn) {
            consolePrintf("  CAM %d  %s (%s): подключена\n",
                          r.idx, r.model.c_str(), r.where.c_str());
        } else {
            consolePrintf("  CAM %d  %s (%s): НУЖНО СВЯЗЫВАНИЕ — на камере: Меню -> Сеть ->\n"
                          "         Функции удалённой съёмки -> Связывание, подтверди \"%s\"\n",
                          r.idx, r.model.c_str(), r.where.c_str(), pc.c_str());
        }
    }
    consolePrintf("\n");
}

// Pull a flat array of strings out of our own small JSON bodies: "key":["a","b"].
static bool jsonGetStringArray(const std::string& body, const std::string& key,
                               std::vector<std::string>& out) {
    const std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return false;
    p = body.find('[', p + pat.size());
    if (p == std::string::npos) return false;
    const size_t end = body.find(']', p);
    if (end == std::string::npos) return false;
    for (size_t q = p + 1; q < end; ) {
        const size_t a = body.find('"', q);
        if (a == std::string::npos || a > end) break;
        const size_t b = body.find('"', a + 1);
        if (b == std::string::npos || b > end) break;
        out.push_back(body.substr(a + 1, b - a - 1));
        q = b + 1;
    }
    return true;
}

// Resolve which cameras a /cmd targets ("all" or a 1-based number).
static std::vector<CameraSession*> resolveTargets(const std::string& camStr) {
    std::vector<CameraSession*> out;
    std::lock_guard<std::mutex> lk(g_camsMutex);
    if (camStr == "all" || camStr == "\"all\"") {
        for (auto& c : g_cams) out.push_back(c.get());
    } else {
        int idx = std::atoi(camStr.c_str());
        for (auto& c : g_cams) if (c->index() == idx) out.push_back(c.get());
    }
    return out;
}

// Group targeting: a list of camera IPs (stable across restarts, unlike "CAM N",
// which is assigned in discovery order). One request for the whole group so a
// group REC starts every camera in the same pass instead of one HTTP call each.
static std::vector<CameraSession*> resolveTargetsByIp(const std::vector<std::string>& ips) {
    std::vector<CameraSession*> out;
    std::lock_guard<std::mutex> lk(g_camsMutex);
    for (const std::string& ip : ips)
        for (auto& c : g_cams)
            if (c->ipUtf8() == ip) { out.push_back(c.get()); break; }
    return out;
}

static coll::HttpResponse handleCmd(const std::string& body) {
    coll::HttpResponse r;
    r.contentType = "application/json; charset=utf-8";

    std::string camStr, action, value;
    std::vector<std::string> camIps;
    const bool byIps = jsonGetStringArray(body, "cams", camIps) && !camIps.empty();
    if ((!byIps && !jsonGet(body, "cam", camStr)) || !jsonGet(body, "action", action)) {
        r.status = 400; r.statusText = "Bad Request";
        r.body = "{\"ok\":false,\"error\":\"missing cam/action\"}";
        return r;
    }
    jsonGet(body, "value", value);

    auto targets = byIps ? resolveTargetsByIp(camIps) : resolveTargets(camStr);
    if (targets.empty()) {
        r.status = 404; r.statusText = "Not Found";
        r.body = "{\"ok\":false,\"error\":\"camera not found\"}";
        return r;
    }

    int okCount = 0;
    for (CameraSession* c : targets) {
        bool ok = false;
        if (action == "rec")            ok = c->setRec(value == "start");
        else if (action == "iso")       ok = c->setIso(value);
        else if (action == "aperture")  ok = c->setAperture(value);
        else if (action == "shutter")   ok = c->setShutter(value);
        else if (action == "wb")        ok = c->setWb(value);
        else if (action == "wbkelvin")  ok = c->setWbKelvin(value);
        else if (action == "micgain")   ok = c->setMicGain(value);
        if (ok) ++okCount;
    }

    if (action != "rec" && action != "iso" &&
        action != "aperture" && action != "shutter" && action != "wb" &&
        action != "wbkelvin" && action != "micgain") {
        r.status = 400; r.statusText = "Bad Request";
        r.body = "{\"ok\":false,\"error\":\"unknown action\"}";
        return r;
    }

    r.body = "{\"ok\":" + std::string(okCount > 0 ? "true" : "false") +
             ",\"applied\":" + std::to_string(okCount) + "}";
    return r;
}

static coll::HttpResponse handleRequest(const coll::HttpRequest& req, const std::string& wwwDir) {
    coll::HttpResponse r;

    if (req.method == "GET" && req.path == "/status.json") {
        r.contentType = "application/json; charset=utf-8";
        { std::lock_guard<std::mutex> lk(g_statusMutex); r.body = g_statusJson; }
        return r;
    }
    if (req.method == "POST" && req.path == "/cmd") {
        return handleCmd(req.body);
    }
    if (req.method == "GET" && req.path == "/update.json") {
        r.contentType = "application/json; charset=utf-8";
        r.body = updateJson();
        return r;
    }
    if (req.method == "POST" && req.path == "/update/check") {
        std::thread([]{ checkUpdateOnce(true); }).detach();   // сеть — не в HTTP-потоке
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        rr.body = "{\"ok\":true}";
        return rr;
    }
    if (req.method == "POST" && req.path == "/update/install") {
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        // Отвечаем сразу, качаем и ставим в фоне: скачивание длинное, а сервер однопоточный.
        std::thread([]{
            std::string err;
            if (!startUpdateInstall(err))
                consolePrintf("[update] Установка не удалась: %s\n", err.c_str());
        }).detach();
        rr.body = "{\"ok\":true}";
        return rr;
    }
    if (req.method == "POST" && req.path == "/update/settings") {
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        std::string v;
        if (jsonGet(req.body, "auto", v)) {
            g_autoUpdate.store(v == "true" || v == "1");
            saveSettings();
            consolePrintf("[update] Автообновление: %s\n", g_autoUpdate.load() ? "включено" : "выключено");
        }
        rr.body = "{\"ok\":true,\"auto\":" + std::string(g_autoUpdate.load() ? "true" : "false") + "}";
        return rr;
    }
    if (req.method == "GET" && req.path == "/groups.json") {
        r.contentType = "application/json; charset=utf-8";
        r.body = readGroupsJson();
        return r;
    }
    if (req.method == "POST" && req.path == "/groups/save") {
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        // The panel is the only writer and owns the schema — just sanity-check
        // that this looks like a JSON object of a sane size before persisting.
        const std::string& b = req.body;
        const size_t open  = b.find_first_not_of(" \t\r\n");
        const size_t close = b.find_last_not_of(" \t\r\n");
        if (b.size() > 262144 || open == std::string::npos ||
            b[open] != '{' || close == std::string::npos || b[close] != '}') {
            rr.status = 400; rr.statusText = "Bad Request";
            rr.body = "{\"ok\":false,\"error\":\"ожидался JSON-объект\"}";
            return rr;
        }
        if (!writeGroupsJson(b)) {
            rr.status = 500; rr.statusText = "Internal Server Error";
            rr.body = "{\"ok\":false,\"error\":\"не удалось записать groups.json\"}";
            return rr;
        }
        rr.body = "{\"ok\":true}";
        return rr;
    }
    if (req.method == "GET" && req.path == "/cameras.json") {
        r.contentType = "application/json; charset=utf-8";
        r.body = manualCamsJson();
        return r;
    }
    if (req.method == "POST" && req.path == "/cameras/add") {
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        std::string ip, model, sshStr;
        if (!jsonGet(req.body, "ip", ip) || !jsonGet(req.body, "model", model)) {
            rr.status = 400; rr.statusText = "Bad Request";
            rr.body = "{\"ok\":false,\"error\":\"нужны ip и model\"}";
            return rr;
        }
        jsonGet(req.body, "ssh", sshStr);
        std::string err;
        if (!manualAdd(ip, model, sshStr == "true" || sshStr == "1", err)) {
            rr.status = 400; rr.statusText = "Bad Request";
            rr.body = "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}";
            return rr;
        }
        rr.body = "{\"ok\":true}";
        return rr;
    }
    if (req.method == "POST" && req.path == "/cameras/remove") {
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        std::string ip;
        if (!jsonGet(req.body, "ip", ip)) {
            rr.status = 400; rr.statusText = "Bad Request";
            rr.body = "{\"ok\":false,\"error\":\"нужен ip\"}";
            return rr;
        }
        const bool ok = manualRemove(ip);
        rr.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
                  (ok ? "" : ",\"error\":\"такой записи нет\"") + "}";
        return rr;
    }
    if (req.method == "POST" && req.path == "/restart") {
        requestRestart();
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        rr.body = "{\"ok\":true,\"restarting\":true}";
        return rr;
    }
    if (req.method == "POST" && req.path == "/shutdown") {
        // Same clean path as the old 'q'+Enter: no restart marker, so the
        // shutdown sequence below disconnects every camera and exits. The
        // response still goes out — the server loop only re-checks the flag
        // after the current request has been answered and its socket closed.
        consolePrintf("Выключение по команде с панели...\n");
        g_running.store(false);
        coll::HttpResponse rr;
        rr.contentType = "application/json; charset=utf-8";
        rr.body = "{\"ok\":true,\"shutdown\":true}";
        return rr;
    }
    if (req.method == "GET" && req.path.rfind("/liveview/", 0) == 0) {
        // /liveview/<n>.jpg -> latest cached JPEG + frames header. Marks the
        // camera "active" so the worker keeps pulling frames (on-demand).
        int idx = std::atoi(req.path.c_str() + std::string("/liveview/").size());
        // ?seq=N — какой кадр уже есть у клиента
        long long haveSeq = -1;
        {
            const size_t q = req.path.find("seq=");
            if (q != std::string::npos) haveSeq = _atoi64(req.path.c_str() + q + 4);
        }
        std::string jpeg, frames;
        long long seq = 0;
        { std::lock_guard<std::mutex> lk(g_lvMutex);
          g_lvActiveMs[idx] = nowSteadyMs();
          auto it = g_lvCache.find(idx);
          if (it != g_lvCache.end()) {
              seq = it->second.seq;
              if (seq != haveSeq) { jpeg = it->second.jpeg; frames = it->second.frames; }
          } }
        if (seq && seq == haveSeq) {                  // кадр не менялся — ничего не шлём
            r.status = 304; r.statusText = "Not Modified";
            r.extraHeaders = "X-Cam-Seq: " + std::to_string(seq) + "\r\n"
                             "Access-Control-Expose-Headers: X-Cam-Frames, X-Cam-Seq\r\n";
            r.contentType.clear();
            return r;
        }
        if (jpeg.empty()) {
            r.status = 503; r.statusText = "Service Unavailable";
            r.body = "live view not ready"; return r;
        }
        r.contentType  = "image/jpeg";
        r.extraHeaders = "X-Cam-Frames: " + (frames.empty() ? std::string("[]") : frames) + "\r\n"
                         "X-Cam-Seq: " + std::to_string(seq) + "\r\n"
                         "Access-Control-Expose-Headers: X-Cam-Frames, X-Cam-Seq\r\n";
        r.body = std::move(jpeg);
        return r;
    }
    if (req.method == "GET") {
        // Map path to a file inside www/.
        std::string rel = (req.path == "/") ? "cam-control-panel.html" : req.path.substr(1);
        if (rel.find("..") != std::string::npos) {        // path traversal guard
            r.status = 403; r.statusText = "Forbidden"; r.body = "forbidden"; return r;
        }
        std::string data;
        if (readFileBinary(plat::joinPath(wwwDir, rel), data)) {
            r.contentType = contentTypeFor(rel);
            r.body = std::move(data);
            return r;
        }
        r.status = 404; r.statusText = "Not Found";
        r.contentType = "text/plain; charset=utf-8";
        r.body = "404: " + rel + " not found. Put the front-end HTML files in the www/ folder.";
        return r;
    }

    r.status = 405; r.statusText = "Method Not Allowed"; r.body = "method not allowed";
    return r;
}

// ---------------- main ----------------
// ---------------- MIDI (winmm) ----------------
// Toggle recording on all cameras (shared by any trigger source).
static void toggleRecAll(const char* src) {
    auto targets = resolveTargets("all");
    bool anyRec = false;
    for (CameraSession* c : targets)
        if (c->isConnected() && c->cachedRecording()) { anyRec = true; break; }
    bool start = !anyRec;
    int ok = 0;
    for (CameraSession* c : targets) if (c->setRec(start)) ++ok;
    consolePrintf("[%s] %s записи на всех камерах: отправлено %d из %zu.\n",
                  src, start ? "Старт" : "Стоп", ok, targets.size());
}

// Consumes MIDI messages: toggles recording on the mapped key.
// Mapped key: Control Change, channel 1 (status 0xB0), controller #17.
static void midiWorker() {
    using clk = std::chrono::steady_clock;
    clk::time_point lastTrig{};
    while (true) {
        unsigned msg = 0;
        {
            std::unique_lock<std::mutex> lk(g_midiMx);
            g_midiCv.wait_for(lk, 200ms, [] { return !g_midiQ.empty() || !g_running.load(); });
            if (!g_running.load() && g_midiQ.empty()) break;
            if (g_midiQ.empty()) continue;
            msg = g_midiQ.front(); g_midiQ.pop_front();
        }
        unsigned status = msg & 0xFF, d1 = (msg >> 8) & 0xFF, d2 = (msg >> 16) & 0xFF;
        // Mapped key: CC (0xB0) channel 1, controller #17. The button sends value 1
        // on press and 0 on release, so trigger ONLY on press (d2 != 0) and ignore
        // the release — a hold of any length is exactly one toggle, the next toggle
        // needs a fresh full press. Small debounce guards against contact bounce.
        if (status == 0xB0 && d1 == 17 && d2 != 0) {
            auto now = clk::now();
            if (now - lastTrig >= 80ms) { lastTrig = now; toggleRecAll("midi"); }
        }
    }
}

// Драйвер зовёт нас из системного потока — там делаем минимум (кладём в очередь),
// всю работу берёт midiWorker: вызовы SDK в этом колбэке недопустимы.
static void startMidi() {
    const bool ok = plat::midiStart([](unsigned status, unsigned d1, unsigned d2) {
        { std::lock_guard<std::mutex> lk(g_midiMx);
          g_midiQ.push_back(status | (d1 << 8) | (d2 << 16)); }
        g_midiCv.notify_one();
    });
    if (ok)
        consolePrintf("[midi] Запись переключается MIDI-кнопкой: CC #17, канал 1 (тумблер старт/стоп на всех).\n");
}

// Тянет кадры только для камер с открытым превью (запрос /liveview за последние
// ~1.5 с) и держит последний JPEG в кэше, чтобы HTTP-обработчик отдавал мгновенно.
// Частота — максимум, который отдаёт камера: спим лишь чуть-чуть, чтобы не крутить
// пустой цикл. Реальный потолок задаёт сама камера и Wi-Fi, а не эта константа.
static void liveViewWorker() {
    const long long activeWindowMs = 1500;
    const auto      frameGap = std::chrono::milliseconds(5);   // не ограничиваем, только уступаем CPU
    while (g_running.load()) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<CameraSession*> cams;
        { std::lock_guard<std::mutex> lk(g_camsMutex);
          for (auto& c : g_cams) cams.push_back(c.get()); }
        long long now = nowSteadyMs();
        for (CameraSession* c : cams) {
            if (!c->isConnected()) continue;
            bool active;
            { std::lock_guard<std::mutex> lk(g_lvMutex);
              auto it = g_lvActiveMs.find(c->index());
              active = (it != g_lvActiveMs.end()) && (now - it->second < activeWindowMs); }
            if (!active) continue;
            std::string jpeg, frames;
            if (c->grabLiveView(jpeg, frames)) {
                std::lock_guard<std::mutex> lk(g_lvMutex);
                auto& e = g_lvCache[c->index()];
                // Камера нередко отдаёт тот же самый кадр несколько раз подряд.
                // Считаем его новым только если картинка действительно изменилась —
                // иначе клиент будет качать дубли.
                const bool changed = (e.jpeg.size() != jpeg.size()) || (e.jpeg != jpeg);
                e.frames = std::move(frames);
                if (changed) { e.jpeg = std::move(jpeg); ++e.seq; }
            }
        }
        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frameGap) std::this_thread::sleep_for(frameGap - elapsed);
    }
}

int main() {
    plat::init();
    plat::onInterrupt([] { g_running.store(false); });   // Ctrl+C / закрытие окна: просто выход
    plat::setLogger([](const std::string& s) { writeConsoleUtf8(s); });

    // Work from the exe directory so the SDK finds Cr_Core.dll + CrAdapter/.
    const std::string dir = plat::exeDir();
    plat::setWorkingDir(dir);

    const std::string cmdLine = plat::commandLine();

    // Elevated one-shot: add the firewall rules and exit. Must come before the
    // single-instance guard — the normal copy is running and holding the slot.
    if (cmdLine.find("--firewall") != std::string::npos)
        return plat::firewallAddRulesForSelf() ? 0 : 1;

    // Установщик обновления: тоже до гарда — основной экземпляр ещё жив и держит слот.
    if (cmdLine.find("--apply-update") != std::string::npos)
        return applyUpdate(cmdLine);

    // Set by the relaunch in the shutdown path: the browser already shows the
    // panel, so don't open another tab.
    const bool relaunched = cmdLine.find("--restarted") != std::string::npos;

    // Before touching the log: a second copy must not rotate the running one's
    // log file. Just show the user the panel of the instance already running.
    if (!plat::acquireSingleInstance(kInstanceName, relaunched)) {
        openPanelInBrowser();
        return 0;
    }

    openLogFile();
    consolePrintf("SignalBox %s\n", kAppVersion);
    ensureManualCamsFile();   // drop a commented template next to the exe
    loadSettings();
    if (!relaunched) maybeOfferFirewallRules();

    // Проверка обновлений — в фоне и без блокировки: нет интернета, нет репозитория —
    // просто ничего не происходит. Первый раз сразу при запуске, дальше периодически:
    // студия может держать программу включённой сутками, и о новой версии надо узнать
    // не только в момент старта.
    std::thread([]{
        const int intervalSec = 4 * 60 * 60;          // раз в 4 часа
        while (g_running.load()) {
            checkUpdateOnce(true);
            if (g_autoUpdate.load()) {
                bool avail = false;
                { std::lock_guard<std::mutex> lk(g_updMutex); avail = g_upd.available; }
                if (avail) {
                    std::string err;
                    if (!startUpdateInstall(err))
                        consolePrintf("[update] Автообновление не удалось: %s\n", err.c_str());
                }
            }
            for (int i = 0; i < intervalSec && g_running.load(); ++i)
                std::this_thread::sleep_for(1s);
        }
    }).detach();
    const std::string wwwDir = plat::joinPath(dir, "www");

    consolePrintf("SignalBox — сборщик статуса камер Sony\n");

    if (!SDK::Init()) {
        consolePrintf("Не удалось инициализировать Camera Remote SDK.\n");
        consolePrintf("Окно закроется через 8 секунд...\n");
        std::this_thread::sleep_for(std::chrono::seconds(8));
        SDK::Release();
        plat::shutdown();
        return 1;
    }

    {
        CrInt32u v = SDK::GetSDKVersion();
        consolePrintf("SDK %d.%d.%02d\n", (v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF);
    }

    // Start the HTTP server FIRST so the overlay/panel are reachable instantly,
    // regardless of how long the camera search takes.
    coll::HttpServer server;
    bool bound = false;
    for (int i = 0; i < 25 && !bound; ++i) {       // retry: on self-restart the old port may still be freeing
        if (server.start(kPort)) { bound = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!bound) {
        consolePrintf("Не удалось открыть порт %u (занят другим приложением?).\n", kPort);
        consolePrintf("Окно закроется через 8 секунд...\n");
        std::this_thread::sleep_for(std::chrono::seconds(8));
        SDK::Release();
        plat::shutdown();
        return 1;
    }

    // Poll thread: rebuild /status.json from the live cameras ~1/sec.
    std::thread poller([]() {
        while (g_running.load()) {
            std::string j = buildStatusJson();
            { std::lock_guard<std::mutex> lk(g_statusMutex); g_statusJson = std::move(j); }
            for (int k = 0; k < 10 && g_running.load(); ++k)
                std::this_thread::sleep_for(100ms);
        }
    });

    // Discovery thread: first scan runs immediately, then every ~5s. Adds new
    // cameras and connects ones not yet connected ONE AT A TIME (a 1s gap after
    // each real attempt) so we never run several connection handshakes at once.
    std::thread discoverer([]() {
        while (g_running.load()) {
            discoverOnce(true);
            addManualCamsOnce(true);      // cameras.txt: networks that block discovery
            reconnectKnownOnce(true);     // ранее найденные — по сохранённому адресу
            std::vector<CameraSession*> cams;
            { std::lock_guard<std::mutex> lk(g_camsMutex);
              for (auto& c : g_cams) cams.push_back(c.get()); }
            for (auto* c : cams) {
                if (!g_running.load()) break;
                if (c->maybeRetryConnect())                       // issued a Connect?
                    std::this_thread::sleep_for(1000ms);          // serialize handshakes
            }
            maybePrintCamSummary();                               // startup + on-change summary
            for (int k = 0; k < 50 && g_running.load(); ++k) {    // ~5s between scans
                if (g_rescanNow.exchange(false)) break;           // panel added a camera
                std::this_thread::sleep_for(100ms);
            }
        }
    });

    // Console command thread: 'r' = restart, 'q' = quit. Detached because
    // std::getline blocks; on EOF (no console stdin) the loop just ends.
    std::thread input([]() {
        std::string line;
        while (g_running.load() && std::getline(std::cin, line)) {
            std::string s;
            for (char ch : line) {              // keep only ASCII letters (strip BOM/spaces/digits)
                unsigned char u = static_cast<unsigned char>(ch);
                if (u < 0x80 && std::isalpha(u)) s += static_cast<char>(std::tolower(u));
            }
            if (s == "r" || s == "restart") {
                consolePrintf("Перезапуск по команде из консоли...\n");
                requestRestart();
                break;
            } else if (s == "q" || s == "x" || s == "quit" || s == "exit") {
                consolePrintf("Выход по команде из консоли...\n");
                g_running.store(false);
                break;
            } else if (!s.empty()) {
                consolePrintf("Команды: r + Enter — перезапуск, q + Enter — выход.\n");
            }
        }
    });
    // MIDI control: a mapped key on the MIDI surface toggles recording on all
    // cameras, globally (independent of window focus). Replaces the keyboard hotkey.
    startMidi();
    std::thread midiThr(midiWorker);
    std::thread lvThr(liveViewWorker);   // live view: pulls frames only for open previews

    consolePrintf("Сервер запущен. Пульт (Custom Browser Dock в OBS):\n");
    consolePrintf("  http://127.0.0.1:%u/\n", kPort);

    std::string lan = localIPv4();
    if (!lan.empty()) {
        consolePrintf("\nС телефона/планшета в той же Wi-Fi:\n");
        consolePrintf("  http://%s:%u/\n", lan.c_str(), kPort);
        consolePrintf("  (если не открывается — разреши SignalBox в брандмауэре Windows)\n");
    }
    consolePrintf("\nИщу камеры в сети, подключаюсь автоматически (подтверждай связывание на камерах).\n");
    consolePrintf("Перезапуск и выключение — кнопками на самой панели.\n\n");

    // HTTP-сервер уходит в рабочий поток, а главный отдаётся циклу событий ОС:
    // значок в трее должен жить именно на главном потоке (на macOS это требование
    // AppKit, на Windows — просто так же удобно).
    std::thread serverThr([&]() {
        server.run([&](const coll::HttpRequest& req) { return handleRequest(req, wwwDir); },
                   g_running);
    });

    if (!relaunched) openPanelInBrowser();

    plat::TrayActions tray;
    tray.onOpenPanel = []{ openPanelInBrowser(); };
    tray.onRestart   = []{ requestRestart(); };            // то же, что кнопка на панели
    tray.onQuit      = []{ g_running.store(false); };      // тот же чистый путь, что 'q'
    plat::runEventLoop(tray, []{ return g_running.load(); });

    // ---- shutdown ----
    g_running.store(false);
    // Сначала дать серверу доработать текущий запрос и выйти из цикла, и только
    // потом закрывать сокет — иначе закрываем дескриптор под чужим select().
    // ⚠ Ждать сервер надо ДО запуска сторожа: браузер держит открытые соединения,
    // и приём последнего запроса может занять до 4 с (SO_RCVTIMEO). Пока это
    // ожидание было внутри восьмисекундного окна, штатное выключение не
    // укладывалось и процесс каждый раз добивался принудительно.
    if (serverThr.joinable()) serverThr.join();
    server.stop();

    consolePrintf("\nОстановка...\n");
    startShutdownWatchdog(8);            // never let a stuck SDK call strand the process
    if (poller.joinable())     poller.join();
    if (discoverer.joinable()) discoverer.join();
    plat::midiStop();           // stop MIDI callbacks before joining the worker
    g_midiCv.notify_all();      // wake the worker so it can observe g_running=false
    if (midiThr.joinable())     midiThr.join();
    if (lvThr.joinable())       lvThr.join();

    {
        std::lock_guard<std::mutex> lk(g_camsMutex);
        // Phase 1: ask EVERY camera to disconnect at once.
        for (auto& c : g_cams) c->beginDisconnect();
        // Phase 2: wait (bounded) until they all actually report disconnected,
        // so no camera is left holding a stale PC-Remote session.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            bool any = false;
            for (auto& c : g_cams) if (c->isConnected()) { any = true; break; }
            if (!any) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Phase 3: release device handles, then finalize the SDK.
        for (auto& c : g_cams) c->finishRelease();
        g_cams.clear();
    }
    SDK::Release();
    plat::shutdown();

    plat::removeTrayIcon();              // don't leave a ghost icon in the tray
    if (g_restart.load()) {
        relaunchSelf();
    } else {
        consolePrintf("Готово.\n");
    }
    return 0;
}
