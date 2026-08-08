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

#include "HttpServer.h"        // pulls in winsock2 before windows.h
#include "CameraSession.h"

#include <windows.h>
#include <mmsystem.h>        // MIDI input (winmm)
#include <shellapi.h>        // ShellExecuteW: open the panel in the browser
#include <share.h>           // _SH_DENYWR: keep the log readable while running
#include <iphlpapi.h>        // GetAdaptersAddresses: pick the real LAN interface
#include <netfw.h>           // Windows Firewall COM API: check/add our inbound rules
#include <winhttp.h>         // HTTPS-клиент для обновлений (встроен в Windows)

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

// ---- MIDI input (winmm): a control-surface key toggles recording on all cameras ----
static std::vector<HMIDIIN>    g_midiIn;                 // open MIDI input handles
static std::mutex              g_midiMx;                 // guards g_midiQ
static std::condition_variable g_midiCv;
static std::deque<DWORD>       g_midiQ;                  // packed short messages (dwParam1)

// ---- live view on-demand cache ----
struct LvFrame { std::string jpeg; std::string frames; };
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

// Print to the console as real Unicode (UTF-16). This does NOT depend on the
// console output code page, which the Sony SDK resets on Connect — so it stays
// readable for the whole run. Falls back to raw UTF-8 bytes when redirected.
// Always mirrored into the log file.
static void writeConsoleUtf8(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h && h != INVALID_HANDLE_VALUE) {          // no console at all in GUI subsystem
        if (GetConsoleMode(h, &mode)) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            if (wlen > 0) {
                std::wstring w(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], wlen);
                DWORD written = 0;
                WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
            }
        } else {
            DWORD wr = 0;
            WriteFile(h, s.c_str(), static_cast<DWORD>(s.size()), &wr, nullptr);
        }
    }
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

static BOOL WINAPI ctrlHandler(DWORD) {
    g_running.store(false);   // Ctrl+C / window close: plain quit (no restart marker)
    return TRUE;
}

// Request a full restart. The app relaunches ITSELF on exit (see shutdown),
// so this works no matter how it was started (bat, shortcut to bat, shortcut
// to exe, ...). Ctrl+C / 'q' don't set this, so they just quit.
static void requestRestart() {
    g_restart.store(true);
    g_running.store(false);
}

static std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

// Fresh log next to the exe on every start (previous run kept as .log.prev).
static void openLogFile() {
    const std::wstring cur  = exeDir() + L"\\SignalBox.log";
    const std::wstring prev = exeDir() + L"\\SignalBox.log.prev";
    MoveFileExW(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    // _wfsopen + _SH_DENYNO (not _wfopen_s, which locks the file exclusively):
    // the log must stay open to readers while SignalBox is running — that is the
    // whole point of it now that there is no console to watch. DENYNO (and not
    // DENYWR) so that even readers asking for FileShare.Read — Notepad, PowerShell
    // ReadAllText — can open it while we keep writing.
    FILE* f = _wfsopen(cur.c_str(), L"wb", _SH_DENYNO);
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
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%u/", static_cast<unsigned>(kPort));
    // ShellExecute returns >32 on success; anything else means no browser was
    // launched, and with no console the log is the only place to say so.
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) > 32) {
        consolePrintf("Панель открыта в браузере по умолчанию.\n");
    } else {
        consolePrintf("Не удалось открыть браузер (код %lld). Открой вручную: http://127.0.0.1:%u/\n",
                      static_cast<long long>(reinterpret_cast<INT_PTR>(rc)), static_cast<unsigned>(kPort));
    }
}

// ---------------- tray icon ----------------
// The panel is served to a browser, so without this the app would have no
// presence on the desktop at all: closing the tab would leave it running with
// no obvious way to stop it (and killing it from Task Manager is exactly what
// strands a camera session -> 0x820A).
static const UINT WM_TRAYICON      = WM_APP + 1;
static const UINT IDM_TRAY_OPEN    = 1001;
static const UINT IDM_TRAY_RESTART = 1002;
static const UINT IDM_TRAY_QUIT    = 1003;

static HWND              g_trayWnd = nullptr;
static NOTIFYICONDATAW   g_nid{};
static std::atomic<bool> g_trayAdded{false};

static void trayRemove() {
    if (g_trayAdded.exchange(false)) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void showTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING,    IDM_TRAY_OPEN,    L"Открыть панель");
    AppendMenuW(m, MF_STRING,    IDM_TRAY_RESTART, L"Перезапустить");
    AppendMenuW(m, MF_SEPARATOR, 0,                nullptr);
    AppendMenuW(m, MF_STRING,    IDM_TRAY_QUIT,    L"Выключить SignalBox");
    SetForegroundWindow(hwnd);                 // else the menu won't close on click-away
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);         // documented TrackPopupMenu workaround
    DestroyMenu(m);
}

static LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP)        showTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) openPanelInBrowser();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRAY_OPEN:    openPanelInBrowser(); break;
        case IDM_TRAY_RESTART: requestRestart();     break;   // same as the panel button
        case IDM_TRAY_QUIT:    g_running.store(false); break; // same clean path as 'q'
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Owns the icon and its message pump; the rest of the app never blocks on it.
static void trayWorker() {
    const wchar_t* kCls = L"SignalBoxTrayWnd";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    g_trayWnd = CreateWindowExW(0, kCls, L"SignalBox", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_trayWnd) {
        consolePrintf("[tray] Не удалось создать окно значка (код %lu).\n", GetLastError());
        return;
    }
    consolePrintf("[tray] Окно значка создано (hwnd=%p).\n", static_cast<void*>(g_trayWnd));

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_trayWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // res/app.rc
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"SignalBox");
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded.store(true);
        consolePrintf("[tray] Значок добавлен в область уведомлений.\n");
    } else {
        consolePrintf("[tray] Не удалось добавить значок (код %lu).\n", GetLastError());
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    trayRemove();
}

// ---------------- restart / shutdown safety ----------------
static void releaseSingleInstance();          // defined with the single-instance guard below
static std::atomic<bool> g_relaunchDone{false};

static void relaunchSelf() {
    if (g_relaunchDone.exchange(true)) return;      // never spawn twice
    consolePrintf("Перезапуск...\n");
    releaseSingleInstance();   // hand the slot over so the fresh copy starts at once
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring wd = exeDir();
    // --restarted: the panel is already open in the browser, so the fresh
    // instance must not pop up a second tab.
    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --restarted";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exePath, cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, wd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        consolePrintf("Не удалось перезапуститься (код %lu).\n", GetLastError());
    }
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
        trayRemove();
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

// ---------------- Windows Firewall ----------------
// Cameras answer discovery with INBOUND UDP, and the panel is served over
// inbound TCP. Windows creates its allow-rule only for the profile ticked in the
// one-off popup, so a PC that works in one studio goes silent in another the
// moment that network is classified differently (Private vs Public). We check
// for our own rule and, with the user's consent, add one for ALL profiles.
static const wchar_t* kFwRuleName = L"SignalBox";

static std::wstring exeFullPath() {
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::wstring(p);
}

// Marker next to the exe: the user said "no", don't nag on every start.
static std::wstring fwSkipPath() { return exeDir() + L"\\firewall-skip.txt"; }

// Reading the rule list needs no elevation.
static bool firewallHasRuleFor(const std::wstring& exePath) {
    bool found = false;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))) && policy) {
        INetFwRules* rules = nullptr;
        if (SUCCEEDED(policy->get_Rules(&rules)) && rules) {
            IUnknown* unk = nullptr;
            if (SUCCEEDED(rules->get__NewEnum(&unk)) && unk) {
                IEnumVARIANT* en = nullptr;
                if (SUCCEEDED(unk->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(&en))) && en) {
                    VARIANT v; VariantInit(&v);
                    ULONG got = 0;
                    while (!found && en->Next(1, &v, &got) == S_OK && got) {
                        INetFwRule* rule = nullptr;
                        if (v.vt == VT_DISPATCH && v.pdispVal &&
                            SUCCEEDED(v.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                      reinterpret_cast<void**>(&rule))) && rule) {
                            BSTR app = nullptr;
                            NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                            NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
                            VARIANT_BOOL enabled = VARIANT_FALSE;
                            rule->get_ApplicationName(&app);
                            rule->get_Direction(&dir);
                            rule->get_Action(&act);
                            rule->get_Enabled(&enabled);
                            if (app && enabled && dir == NET_FW_RULE_DIR_IN && act == NET_FW_ACTION_ALLOW &&
                                _wcsicmp(app, exePath.c_str()) == 0)
                                found = true;
                            if (app) SysFreeString(app);
                            rule->Release();
                        }
                        VariantClear(&v);
                    }
                    en->Release();
                }
                unk->Release();
            }
            rules->Release();
        }
        policy->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return found;
}

static bool addOneFwRule(INetFwRules* rules, const std::wstring& exePath, NET_FW_IP_PROTOCOL proto) {
    INetFwRule* rule = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwRule), reinterpret_cast<void**>(&rule))) || !rule)
        return false;
    BSTR name = SysAllocString(kFwRuleName);
    BSTR desc = SysAllocString(L"Панель управления камерами и обнаружение камер в локальной сети");
    BSTR app  = SysAllocString(exePath.c_str());
    BSTR grp  = SysAllocString(kFwRuleName);
    rule->put_Name(name);
    rule->put_Description(desc);
    rule->put_ApplicationName(app);
    rule->put_Grouping(grp);
    rule->put_Protocol(proto);
    rule->put_Direction(NET_FW_RULE_DIR_IN);
    rule->put_Action(NET_FW_ACTION_ALLOW);
    rule->put_Profiles(NET_FW_PROFILE2_ALL);        // ключевое: любая сеть, не только текущая
    rule->put_Enabled(VARIANT_TRUE);
    const HRESULT hr = rules->Add(rule);
    SysFreeString(name); SysFreeString(desc); SysFreeString(app); SysFreeString(grp);
    rule->Release();
    return SUCCEEDED(hr);
}

// Runs in the elevated one-shot instance (--firewall). Adds TCP + UDP inbound
// allow rules for this exe, scoped to the program only — nothing else is touched.
static bool addFirewallRules(const std::wstring& exePath) {
    bool ok = false;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))) && policy) {
        INetFwRules* rules = nullptr;
        if (SUCCEEDED(policy->get_Rules(&rules)) && rules) {
            const bool tcp = addOneFwRule(rules, exePath, NET_FW_IP_PROTOCOL_TCP);
            const bool udp = addOneFwRule(rules, exePath, NET_FW_IP_PROTOCOL_UDP);
            ok = tcp && udp;
            rules->Release();
        }
        policy->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

// Normal startup path: ask once, then hand the actual change to Windows' own
// elevation prompt. Never changes anything without both confirmations.
static void maybeOfferFirewallRules() {
    const std::wstring exe = exeFullPath();
    if (GetFileAttributesW(fwSkipPath().c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (firewallHasRuleFor(exe)) {
        consolePrintf("[firewall] Разрешение уже выдано.\n");
        return;
    }
    consolePrintf("[firewall] Разрешения для SignalBox нет — спрашиваю пользователя.\n");

    const int answer = MessageBoxW(nullptr,
        L"Разрешить SignalBox приём подключений в локальной сети?\n\n"
        L"Это нужно, чтобы находились камеры и чтобы панель открывалась "
        L"с телефона и других компьютеров.\n\n"
        L"Разрешение будет выдано для всех типов сетей — иначе в другой студии, "
        L"где сеть определится иначе, камеры перестанут находиться.\n\n"
        L"Потребуется подтверждение администратора Windows.",
        L"SignalBox — доступ в сеть", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);

    if (answer != IDYES) {
        FILE* f = _wfsopen(fwSkipPath().c_str(), L"wb", _SH_DENYNO);
        if (f) {
            static const char kNote[] =
                "\xEF\xBB\xBF"
                "Пользователь отказался добавлять правило брандмауэра.\r\n"
                "Удали этот файл, чтобы SignalBox спросил снова.\r\n";
            std::fwrite(kNote, 1, sizeof(kNote) - 1, f);
            std::fclose(f);
        }
        consolePrintf("[firewall] Пользователь отказался. Больше не спрашиваю (см. firewall-skip.txt).\n");
        return;
    }

    // Elevate a one-shot copy of ourselves; Windows shows the UAC prompt.
    SHELLEXECUTEINFOW si{};
    si.cbSize       = sizeof(si);
    si.fMask        = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb       = L"runas";
    si.lpFile       = exe.c_str();
    si.lpParameters = L"--firewall";
    si.lpDirectory  = exeDir().c_str();
    si.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&si) || !si.hProcess) {
        consolePrintf("[firewall] Не удалось запросить права администратора (код %lu).\n", GetLastError());
        return;
    }
    WaitForSingleObject(si.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(si.hProcess, &code);
    CloseHandle(si.hProcess);
    consolePrintf(code == 0 ? "[firewall] Правила добавлены (TCP+UDP, все сети).\n"
                            : "[firewall] Добавить правила не удалось (код %lu).\n", code);
}

// ---------------- обновление через GitHub Releases ----------------
// Студии получают программу архивом, и менять его вручную неудобно. Спрашиваем у
// GitHub последний релиз, сравниваем версии и ставим — по кнопке или автоматически,
// если включено. Всё на WinHTTP: он встроен в Windows, внешних библиотек не нужно.
// Нет интернета — молча пропускаем, работа офлайн не должна страдать.
static bool        jsonGet(const std::string&, const std::string&, std::string&);   // определены ниже
static std::string jsonEscape(const std::string&);

static const char*    kAppVersion = "1.0.0";
// ЗАПОЛНИТЬ после создания репозитория, формат "владелец/репозиторий".
// Пустая строка = проверка обновлений выключена.
static const wchar_t* kUpdateRepo = L"GIDEONSYSTEM/signalbox";

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

static std::wstring settingsPath() { return exeDir() + L"\\settings.json"; }

static void loadSettings() {
    std::ifstream f(settingsPath(), std::ios::binary);
    if (!f) return;
    std::string s, line;
    while (std::getline(f, line)) s += line;
    std::string v;
    if (jsonGet(s, "autoUpdate", v)) g_autoUpdate.store(v == "true" || v == "1");
}

static void saveSettings() {
    FILE* f = _wfsopen(settingsPath().c_str(), L"wb", _SH_DENYNO);
    if (!f) return;
    const std::string s = std::string("{\"autoUpdate\":") + (g_autoUpdate.load() ? "true" : "false") + "}\r\n";
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

// GET по HTTPS: тело в body и/или в файл. Редиректы WinHTTP отрабатывает сам —
// ссылки GitHub на архивы как раз редиректят на CDN.
static bool httpFetch(const std::wstring& url, std::string* body, const std::wstring& destFile,
                      DWORD* statusOut = nullptr) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET hs = WinHttpOpen(L"SignalBox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return false;
    DWORD timeout = 15000;
    WinHttpSetTimeouts(hs, timeout, timeout, timeout, timeout);

    bool ok = false;
    if (HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0)) {
        const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(hr, hdr, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hr, nullptr)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                if (statusOut) *statusOut = code;
                if (code == 200) {
                    FILE* f = destFile.empty() ? nullptr : _wfsopen(destFile.c_str(), L"wb", _SH_DENYNO);
                    if (destFile.empty() || f) {
                        ok = true;
                        for (;;) {
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable(hr, &avail)) { ok = false; break; }
                            if (!avail) break;
                            std::vector<char> buf(avail);
                            DWORD got = 0;
                            if (!WinHttpReadData(hr, buf.data(), avail, &got)) { ok = false; break; }
                            if (body) body->append(buf.data(), got);
                            if (f)    std::fwrite(buf.data(), 1, got, f);
                        }
                    }
                    if (f) std::fclose(f);
                }
            }
            WinHttpCloseHandle(hr);
        }
        WinHttpCloseHandle(hc);
    }
    WinHttpCloseHandle(hs);
    return ok;
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

    const std::wstring api = L"https://api.github.com/repos/" + std::wstring(kUpdateRepo) +
                             L"/releases/latest";
    std::string json;
    UpdateInfo info;
    info.current = kAppVersion;

    DWORD status = 0;
    if (!httpFetch(api, &json, L"", &status)) {
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

static bool runAndWait(const std::wstring& cmd, DWORD timeoutMs, DWORD* exitCode) {
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return false;
    const DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return w == WAIT_OBJECT_0;
}

static std::wstring updateStagingDir() {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"SignalBox-update";
}

// Скачать архив, распаковать и запустить распакованную копию в режиме --apply-update:
// она дождётся нашего выхода, заменит файлы и стартует уже обновлённую программу.
// Так не приходится перезаписывать файлы, которые сейчас заняты (exe и DLL Sony).
static bool startUpdateInstall(std::string& err) {
    UpdateInfo info;
    { std::lock_guard<std::mutex> lk(g_updMutex); info = g_upd; }
    if (!info.available || info.url.empty()) { err = "обновление недоступно"; return false; }
    if (g_updBusy.exchange(true))            { err = "обновление уже идёт";  return false; }

    const std::wstring stage = updateStagingDir();
    const std::wstring zip   = stage + L"\\update.zip";
    const std::wstring src   = stage + L"\\SignalBox";      // внутри архива папка SignalBox\

    bool ok = false;
    do {
        // чистая площадка
        runAndWait(L"cmd.exe /c rd /s /q \"" + stage + L"\"", 20000, nullptr);
        if (!CreateDirectoryW(stage.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = "не удалось создать временную папку"; break;
        }
        const std::wstring wurl(info.url.begin(), info.url.end());
        consolePrintf("[update] Качаю %s\n", info.url.c_str());
        if (!httpFetch(wurl, nullptr, zip)) { err = "не удалось скачать архив"; break; }

        DWORD rc = 1;
        // tar.exe входит в состав Windows 10/11 и умеет zip
        runAndWait(L"tar.exe -xf \"" + zip + L"\" -C \"" + stage + L"\"", 120000, &rc);
        if (rc != 0 || GetFileAttributesW((src + L"\\SignalBox.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "архив распаковался неправильно"; break;
        }

        wchar_t pid[32]; swprintf_s(pid, L"%lu", GetCurrentProcessId());
        const std::wstring cmd = L"\"" + src + L"\\SignalBox.exe\" --apply-update \"" +
                                 exeDir() + L"\" " + pid;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, src.c_str(), &si, &pi)) {
            err = "не удалось запустить установщик"; break;
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        consolePrintf("[update] Ставлю версию %s, выключаюсь...\n", info.latest.c_str());
        ok = true;
    } while (false);

    g_updBusy.store(false);
    if (ok) g_running.store(false);        // отпускаем файлы: установщик ждёт нашего выхода
    return ok;
}

// Режим установщика: ждём выхода старого процесса, копируем файлы, запускаем обновлённую копию.
static int applyUpdate(const std::wstring& cmdLine) {
    // ... --apply-update "<target>" <pid>
    const size_t k = cmdLine.find(L"--apply-update");
    if (k == std::wstring::npos) return 1;
    size_t q1 = cmdLine.find(L'"', k);
    size_t q2 = (q1 == std::wstring::npos) ? q1 : cmdLine.find(L'"', q1 + 1);
    if (q2 == std::wstring::npos) return 1;
    const std::wstring target = cmdLine.substr(q1 + 1, q2 - q1 - 1);
    const DWORD pid = static_cast<DWORD>(_wtoi(cmdLine.c_str() + q2 + 1));

    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
    std::this_thread::sleep_for(700ms);           // дать ОС отпустить DLL

    const std::wstring src = updateStagingDir() + L"\\SignalBox";
    DWORD rc = 16;
    runAndWait(L"robocopy \"" + src + L"\" \"" + target +
               L"\" /E /NFL /NDL /NJH /NJS /NP /R:3 /W:1", 180000, &rc);
    if (rc >= 8) return 1;                        // у robocopy успех — это код < 8

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring exe = target + L"\\SignalBox.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(L'\0');
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, target.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
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
static HANDLE g_singleInstance = nullptr;

// waitForPrevious: on a self-restart the outgoing process still holds the slot
// for a moment, so wait it out instead of refusing to start.
static bool acquireSingleInstance(bool waitForPrevious) {
    const int tries = waitForPrevious ? 75 : 1;          // ~15s
    for (int i = 0; i < tries; ++i) {
        HANDLE h = CreateMutexW(nullptr, TRUE, L"Local\\SignalBox_SingleInstance");
        if (!h) return true;                             // can't tell — never block startup
        if (GetLastError() != ERROR_ALREADY_EXISTS) { g_singleInstance = h; return true; }
        CloseHandle(h);
        if (i + 1 < tries) std::this_thread::sleep_for(200ms);
    }
    return false;
}

static void releaseSingleInstance() {
    if (!g_singleInstance) return;
    ReleaseMutex(g_singleInstance);
    CloseHandle(g_singleInstance);
    g_singleInstance = nullptr;
}

static std::string pcName() {
    wchar_t buf[256]; DWORD n = 256;
    if (GetComputerNameW(buf, &n)) {
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) { std::string s(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], len, nullptr, nullptr); return s; }
    }
    return "(этот ПК)";
}

static std::string w2u(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
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
    char host[256] = {0};
    if (gethostname(host, sizeof(host)) != 0) return {};
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return {};
    std::string best; int bestScore = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char buf[64] = {0};
        if (::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            int sc = ipScore(buf);
            if (sc > bestScore) { bestScore = sc; best = buf; }
        }
    }
    freeaddrinfo(res);
    return best;
}

// ipifcons.h values — iphlpapi.h does not always pull that header in.
static const IFTYPE kIfPPP = 23, kIfLoopback = 24, kIfPropVirtual = 53, kIfTunnel = 131;

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
        const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                            GAA_FLAG_SKIP_MULTICAST  | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG size = 15000;
        std::vector<char> buf(size);
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                       reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(size);
            rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                       reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        }

        std::string best;
        int bestScore = -1000000;
        if (rc == NO_ERROR) {
            for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) continue;      // e.g. unplugged Bluetooth
                if (a->IfType == kIfLoopback) continue;

                bool gw = false;                                    // a REAL gateway, not 0.0.0.0
                for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
                    if (g->Address.lpSockaddr && g->Address.lpSockaddr->sa_family == AF_INET &&
                        reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr)->sin_addr.s_addr != 0) {
                        gw = true; break;
                    }
                }
                const bool virt = (a->IfType == kIfTunnel || a->IfType == kIfPPP ||
                                   a->IfType == kIfPropVirtual);

                for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                    auto* s = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                    char ip[64] = {0};
                    if (!::inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip))) continue;
                    int sc = ipScore(ip);
                    if (virt) sc -= 60;      // VPN / Hyper-V / Docker style adapter
                    if (gw)   sc += 25;      // has a way off its own subnet -> a real LAN
                    consolePrintf("[net]   %s — %s (тип %lu%s%s) -> вес %d\n",
                                  w2u(a->FriendlyName).c_str(), ip, a->IfType,
                                  virt ? ", виртуальный" : "",
                                  gw ? ", шлюз есть" : ", без шлюза", sc);
                    if (sc > bestScore) { bestScore = sc; best = ip; }
                }
            }
        } else {
            consolePrintf("[net] Не удалось прочитать список адаптеров (код %lu).\n", rc);
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

static bool readFileBinary(const std::wstring& full, std::string& out) {
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
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
        j += "\"wbKelvin\":" + (s.wbKelvin < 0 ? std::string("null") : std::to_string(s.wbKelvin));
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
    ULONG mac[2] = {0, 0};
    ULONG len = 6;
    if (::SendARP(static_cast<IPAddr>(ipLe), 0, mac, &len) == NO_ERROR && len == 6) {
        std::memcpy(out, mac, 6);
        return;
    }
    out[0] = 0x02; out[1] = 0x00;                       // locally-administered
    std::memcpy(out + 2, &ipLe, 4);
}

static std::wstring manualCamsPath() { return exeDir() + L"\\cameras.txt"; }

static void ensureManualCamsFile() {
    const std::wstring p = manualCamsPath();
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfsopen(p.c_str(), L"wb", _SH_DENYNO);
    if (!f) return;
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
    std::fwrite(kTemplate, 1, sizeof(kTemplate) - 1, f);
    std::fclose(f);
}

static std::vector<ManualCam> readManualCams() {
    std::vector<ManualCam> out;
    std::ifstream f(manualCamsPath());
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
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
    std::ifstream f(manualCamsPath(), std::ios::binary);
    if (!f) return lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool writeManualFile(const std::vector<std::string>& lines) {
    FILE* f = _wfsopen(manualCamsPath().c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    std::fwrite("\xEF\xBB\xBF", 1, 3, f);                       // keep it Notepad-friendly
    for (std::string s : lines) {
        if (!s.empty() && static_cast<unsigned char>(s[0]) == 0xEF && s.size() >= 3)
            s.erase(0, 3);                                       // strip an inherited BOM
        s += "\r\n";
        std::fwrite(s.data(), 1, s.size(), f);
    }
    std::fclose(f);
    return true;
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
    IN_ADDR probe{};
    if (::inet_pton(AF_INET, ip.c_str(), &probe) != 1) { err = "Это не похоже на IP-адрес"; return false; }
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
        IN_ADDR addr{};
        if (::inet_pton(AF_INET, mc.ip.c_str(), &addr) != 1) {
            std::lock_guard<std::mutex> lk(s_warnMutex);
            if (s_warned[mc.ip]++ == 0)
                consolePrintf("[cameras.txt] \"%s\" — это не IP-адрес.\n", mc.ip.c_str());
            continue;
        }
        // Docs: 1st octet -> bits 7..0 ... 4th -> bits 31..24, i.e. exactly the
        // little-endian in_addr that inet_pton produces (192.168.0.5 = 0x0500A8C0).
        const CrInt32u ipLe = static_cast<CrInt32u>(addr.S_un.S_addr);

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
static std::wstring knownCamsPath() { return exeDir() + L"\\cameras-known.txt"; }

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
    ULONG mac[2] = {0, 0};
    ULONG len = 6;
    if (::SendARP(static_cast<IPAddr>(ipLe), 0, mac, &len) != NO_ERROR || len != 6) return false;
    macOut = macToText(reinterpret_cast<const unsigned char*>(mac));
    return true;
}

// DHCP moved the camera? Find its current address by MAC in the neighbour table.
static bool ipForMac(const std::string& wantMac, std::string& ipOut) {
    const std::string want = normMac(wantMac);
    if (want.size() != 12) return false;
    PMIB_IPNET_TABLE2 tbl = nullptr;
    if (GetIpNetTable2(AF_INET, &tbl) != NO_ERROR || !tbl) return false;
    bool found = false;
    for (ULONG i = 0; i < tbl->NumEntries && !found; ++i) {
        const MIB_IPNET_ROW2& r = tbl->Table[i];
        if (r.PhysicalAddressLength != 6) continue;
        if (normMac(macToText(r.PhysicalAddress)) != want) continue;
        char ip[64] = {0};
        if (::inet_ntop(AF_INET, &r.Address.Ipv4.sin_addr, ip, sizeof(ip))) { ipOut = ip; found = true; }
    }
    FreeMibTable(tbl);
    return found;
}

static std::vector<KnownCam> readKnownCams() {
    std::vector<KnownCam> out;
    std::ifstream f(knownCamsPath());
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
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
    FILE* f = _wfsopen(knownCamsPath().c_str(), L"wb", _SH_DENYNO);
    if (!f) return;
    static const char kHead[] =
        "\xEF\xBB\xBF"
        "# Этот файл SignalBox ведёт сам — править не нужно.\r\n"
        "# Сюда попадают камеры, которые хоть раз нашлись автопоиском.\r\n"
        "# Если потом автопоиск их не увидит (сеть режет широковещание —\r\n"
        "# частый случай при ширине канала 20/40 МГц на 2.4 ГГц), SignalBox\r\n"
        "# подключится к ним напрямую по сохранённому адресу.\r\n"
        "# Формат: MAC  IP  МОДЕЛЬ\r\n";
    std::fwrite(kHead, 1, sizeof(kHead) - 1, f);
    for (const KnownCam& k : cams) {
        const std::string line = k.mac + "  " + k.ip + "  " + k.model + "\r\n";
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
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
        IN_ADDR a{};
        bool reachable = (::inet_pton(AF_INET, ip.c_str(), &a) == 1) &&
                         arpLookup(static_cast<CrInt32u>(a.S_un.S_addr), mac);
        if (!reachable && normMac(k.mac).size() == 12) {
            std::string moved;
            if (ipForMac(k.mac, moved) && ::inet_pton(AF_INET, moved.c_str(), &a) == 1 &&
                arpLookup(static_cast<CrInt32u>(a.S_un.S_addr), mac)) {
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
        macForIp(static_cast<CrInt32u>(a.S_un.S_addr), macBytes);
        SDK::ICrCameraObjectInfo* info = nullptr;
        const SDK::CrError err = SDK::CreateCameraObjectInfoEthernetConnection(
            &info, model, static_cast<CrInt32u>(a.S_un.S_addr), macBytes, SDK::CrSSHsupport_OFF);
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

static std::wstring groupsPath() { return exeDir() + L"\\groups.json"; }

static std::string readGroupsJson() {
    std::lock_guard<std::mutex> lk(g_groupsMutex);
    std::ifstream f(groupsPath(), std::ios::binary);
    if (!f) return "{\"groups\":[]}";
    std::string s;
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount()) s.append(buf, static_cast<size_t>(f.gcount()));
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);   // BOM
    if (s.find('{') == std::string::npos) return "{\"groups\":[]}";
    return s;
}

static bool writeGroupsJson(const std::string& json) {
    std::lock_guard<std::mutex> lk(g_groupsMutex);
    FILE* f = _wfsopen(groupsPath().c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    return true;
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
        std::string mac = w2u(info->GetMACAddressChar());
        std::string ip  = w2u(info->GetIPAddressChar());
        std::string keyNew = mac.empty() ? ip : mac;
        // Запомнить: если в другой раз автопоиск её не увидит, подключимся по адресу.
        rememberCamera(mac, ip, w2u(info->GetModel()));

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
        if (ok) ++okCount;
    }

    if (action != "rec" && action != "iso" &&
        action != "aperture" && action != "shutter" && action != "wb" &&
        action != "wbkelvin") {
        r.status = 400; r.statusText = "Bad Request";
        r.body = "{\"ok\":false,\"error\":\"unknown action\"}";
        return r;
    }

    r.body = "{\"ok\":" + std::string(okCount > 0 ? "true" : "false") +
             ",\"applied\":" + std::to_string(okCount) + "}";
    return r;
}

static coll::HttpResponse handleRequest(const coll::HttpRequest& req, const std::wstring& wwwDir) {
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
        std::string jpeg, frames;
        { std::lock_guard<std::mutex> lk(g_lvMutex);
          g_lvActiveMs[idx] = nowSteadyMs();
          auto it = g_lvCache.find(idx);
          if (it != g_lvCache.end()) { jpeg = it->second.jpeg; frames = it->second.frames; } }
        if (jpeg.empty()) {
            r.status = 503; r.statusText = "Service Unavailable";
            r.body = "live view not ready"; return r;
        }
        r.contentType  = "image/jpeg";
        r.extraHeaders = "X-Cam-Frames: " + (frames.empty() ? std::string("[]") : frames) + "\r\n";
        r.body = std::move(jpeg);
        return r;
    }
    if (req.method == "GET") {
        // Map path to a file inside www/.
        std::string rel = (req.path == "/") ? "cam-control-panel.html" : req.path.substr(1);
        if (rel.find("..") != std::string::npos) {        // path traversal guard
            r.status = 403; r.statusText = "Forbidden"; r.body = "forbidden"; return r;
        }
        std::wstring wrel(rel.begin(), rel.end());
        std::wstring full = wwwDir + L"\\" + wrel;
        std::string data;
        if (readFileBinary(full, data)) {
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

// winmm calls this from a system thread — do the minimum (enqueue) here; the
// worker thread does the real work (SDK calls must not run in this callback).
static void CALLBACK midiCallback(HMIDIIN, UINT wMsg, DWORD_PTR, DWORD_PTR dwParam1, DWORD_PTR) {
    if (wMsg != MIM_DATA) return;
    { std::lock_guard<std::mutex> lk(g_midiMx); g_midiQ.push_back(static_cast<DWORD>(dwParam1)); }
    g_midiCv.notify_one();
}

// Consumes MIDI messages: logs them and toggles recording on the mapped key.
// Mapped key: Control Change, channel 1 (status 0xB0), controller #17.
static void midiWorker() {
    using clk = std::chrono::steady_clock;
    clk::time_point lastTrig{};
    while (true) {
        DWORD msg = 0;
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

static void startMidi() {
    UINT n = midiInGetNumDevs();
    if (n == 0) {
        consolePrintf("[midi] MIDI-устройств не найдено. Подключи контроллер и нажми r+Enter.\n");
        return;
    }
    consolePrintf("[midi] MIDI-входов: %u\n", n);
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPS caps{};
        if (midiInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            char name[160] = {0};
            WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name), nullptr, nullptr);
            consolePrintf("   [%u] %s\n", i, name);
        }
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, reinterpret_cast<DWORD_PTR>(midiCallback), 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            midiInStart(h);
            g_midiIn.push_back(h);
        }
    }
    consolePrintf("[midi] Запись переключается MIDI-кнопкой: CC #17, канал 1 (тумблер старт/стоп на всех).\n");
}

static void stopMidi() {
    for (HMIDIIN h : g_midiIn) { midiInStop(h); midiInReset(h); midiInClose(h); }
    g_midiIn.clear();
}

// Pulls live-view frames (~7 fps) only for cameras whose preview is currently
// open (a /liveview request within the last ~1.5 s) and caches the latest JPEG +
// frame rectangles so the HTTP handler can serve them instantly.
static void liveViewWorker() {
    const long long activeWindowMs = 1500;
    const auto      frameGap = std::chrono::milliseconds(1000 / 7);   // ~7 fps
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
                e.jpeg   = std::move(jpeg);
                e.frames = std::move(frames);
            }
        }
        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frameGap) std::this_thread::sleep_for(frameGap - elapsed);
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Work from the exe directory so the SDK finds Cr_Core.dll + CrAdapter/.
    const std::wstring dir = exeDir();
    SetCurrentDirectoryW(dir.c_str());

    const std::wstring cmdLine = GetCommandLineW();

    // Elevated one-shot: add the firewall rules and exit. Must come before the
    // single-instance guard — the normal copy is running and holding the slot.
    if (cmdLine.find(L"--firewall") != std::wstring::npos)
        return addFirewallRules(exeFullPath()) ? 0 : 1;

    // Установщик обновления: тоже до гарда — основной экземпляр ещё жив и держит слот.
    if (cmdLine.find(L"--apply-update") != std::wstring::npos)
        return applyUpdate(cmdLine);

    // Set by the relaunch in the shutdown path: the browser already shows the
    // panel, so don't open another tab.
    const bool relaunched = cmdLine.find(L"--restarted") != std::wstring::npos;

    // Before touching the log: a second copy must not rotate the running one's
    // log file. Just show the user the panel of the instance already running.
    if (!acquireSingleInstance(relaunched)) {
        openPanelInBrowser();
        return 0;
    }

    openLogFile();
    consolePrintf("SignalBox %s\n", kAppVersion);
    ensureManualCamsFile();   // drop a commented template next to the exe
    loadSettings();
    if (!relaunched) maybeOfferFirewallRules();

    // Проверка обновлений — в фоне и без блокировки: нет интернета, нет репозитория —
    // просто ничего не происходит.
    std::thread([]{
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
    }).detach();
    const std::wstring wwwDir = dir + L"\\www";

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        consolePrintf("WSAStartup failed.\n");
        return 1;
    }

    consolePrintf("SignalBox — сборщик статуса камер Sony\n");

    if (!SDK::Init()) {
        consolePrintf("Не удалось инициализировать Camera Remote SDK.\n");
        consolePrintf("Окно закроется через 8 секунд...\n");
        std::this_thread::sleep_for(std::chrono::seconds(8));
        SDK::Release();
        WSACleanup();
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
        WSACleanup();
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

    std::thread trayThr(trayWorker);     // tray icon: open panel / restart / quit

    if (!relaunched) openPanelInBrowser();

    server.run([&](const coll::HttpRequest& req) { return handleRequest(req, wwwDir); },
               g_running);

    // ---- shutdown ----
    consolePrintf("\nОстановка...\n");
    startShutdownWatchdog(8);            // never let a stuck SDK call strand the process
    g_running.store(false);
    server.stop();
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_CLOSE, 0, 0);
    if (trayThr.joinable()) trayThr.join();
    if (poller.joinable())     poller.join();
    if (discoverer.joinable()) discoverer.join();
    stopMidi();                 // stop MIDI callbacks before joining the worker
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
    WSACleanup();

    trayRemove();                        // don't leave a ghost icon in the tray
    if (g_restart.load()) {
        relaunchSelf();
    } else {
        consolePrintf("Готово.\n");
    }
    return 0;
}
