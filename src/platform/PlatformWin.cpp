// Реализация платформенного слоя для Windows.
//
// Единственное место в проекте, где живут windows.h, Winsock, WinHTTP, COM,
// winmm и Shell API. Всё остальное общается с ОС только через Platform.h.

#include "Platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>        // до windows.h, иначе подтянется winsock 1
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>        // ShellExecuteW: открыть панель в браузере
#include <share.h>           // _SH_DENYNO: лог должен читаться на живой программе
#include <iphlpapi.h>        // GetAdaptersAddresses / SendARP / GetIpNetTable2
#include <winhttp.h>         // HTTPS-клиент: встроен в Windows, внешних библиотек не нужно
#include <mmsystem.h>        // MIDI-вход (winmm)
#include <netfw.h>           // COM-интерфейс брандмауэра Windows

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <thread>
#include <vector>

namespace plat {
namespace {

// ---- конверсия строк (наружу торчит только utf8FromWide) ----
std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &w[0], len);
    return w;
}

std::function<void()> g_interrupt;
HANDLE                g_singleInstance = nullptr;

std::function<void(const std::string&)> g_log;

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    g_log(std::string(buf, (static_cast<size_t>(n) < sizeof(buf)) ? static_cast<size_t>(n)
                                                                 : sizeof(buf) - 1));
}

BOOL WINAPI ctrlHandler(DWORD) {
    if (g_interrupt) g_interrupt();
    return TRUE;
}

// Аргумент командной строки в кавычках, если внутри есть пробелы.
std::wstring quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

} // namespace

// ---------------- инициализация ----------------
bool init() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void shutdown() {
    WSACleanup();
}

void onInterrupt(std::function<void()> handler) {
    g_interrupt = std::move(handler);
}

void setLogger(std::function<void(const std::string&)> sink) {
    g_log = std::move(sink);
}

// ---------------- строки ----------------
std::string utf8FromWide(const wchar_t* w) {
    if (!w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// ---------------- пути и файлы ----------------
std::string exePath() {
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return utf8FromWide(p);
}

std::string exeDir() {
    const std::string p = exePath();
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

std::string tempDir() {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::string s = utf8FromWide(tmp);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();   // joinPath добавит свой
    return s;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string r = a;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    size_t i = 0;
    while (i < b.size() && (b[i] == '\\' || b[i] == '/')) ++i;
    return r + b.substr(i);
}

bool fileExists(const std::string& path) {
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool makeDir(const std::string& path) {
    if (CreateDirectoryW(wide(path).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeTree(const std::string& path) {
    if (path.empty() || !fileExists(path)) return true;
    runAndWait("cmd.exe /c rd /s /q \"" + path + "\"", 20000, nullptr);
    return !fileExists(path);
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = _wfsopen(wide(path).c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t bytes) {
    FILE* f = _wfsopen(wide(path).c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    const bool ok = (bytes == 0) || (std::fwrite(data, 1, bytes, f) == bytes);
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
    return writeFile(path, data.data(), data.size());
}

bool replaceFile(const std::string& from, const std::string& to) {
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// _wfsopen + _SH_DENYNO, а не fopen: файл должен оставаться открытым на чтение
// для Notepad и PowerShell, пока мы в него пишем.
FILE* openForSharedWrite(const std::string& path) {
    return _wfsopen(wide(path).c_str(), L"wb", _SH_DENYNO);
}

bool setWorkingDir(const std::string& path) {
    return SetCurrentDirectoryW(wide(path).c_str()) != 0;
}

// tar.exe входит в состав Windows 10/11 и умеет zip — отдельная библиотека не нужна.
bool extractArchive(const std::string& zipPath, const std::string& destDir) {
    int rc = 1;
    runAndWait("tar.exe -xf \"" + zipPath + "\" -C \"" + destDir + "\"", 120000, &rc);
    return rc == 0;
}

// robocopy /E: копирует дерево, но НЕ удаляет в приёмнике лишнее (нет /PURGE) —
// ровно то, что нужно обновлению. Успех у robocopy — это код меньше 8.
bool copyTree(const std::string& srcDir, const std::string& dstDir) {
    int rc = 16;
    runAndWait("robocopy \"" + srcDir + "\" \"" + dstDir + "\" /E /NFL /NDL /NJH /NJS /NP /R:3 /W:1",
               180000, &rc);
    return rc < 8;
}

// ---------------- консоль ----------------
// Печатаем настоящим Unicode (UTF-16): так вывод не зависит от кодовой страницы
// консоли, которую SDK Sony сбрасывает при Connect. Перенаправленный поток —
// сырые UTF-8 байты. В GUI-подсистеме консоли нет вовсе, и функция молчит.
void writeConsole(const std::string& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = wide(s);
        if (!w.empty()) {
            DWORD written = 0;
            WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
        }
    } else {
        DWORD wr = 0;
        WriteFile(h, s.c_str(), static_cast<DWORD>(s.size()), &wr, nullptr);
    }
}

// ---------------- процессы ----------------
std::string commandLine() { return utf8FromWide(GetCommandLineW()); }

unsigned long currentPid() { return GetCurrentProcessId(); }

bool waitForProcess(unsigned long pid, int timeoutMs) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return true;                     // процесса уже нет — считаем, что дождались
    const DWORD w = WaitForSingleObject(h, static_cast<DWORD>(timeoutMs));
    CloseHandle(h);
    return w == WAIT_OBJECT_0;
}

bool launch(const std::string& exe, const std::string& args, const std::string& workDir) {
    const std::wstring wexe = wide(exe);
    std::wstring cmd = quoted(wexe);
    if (!args.empty()) cmd += L" " + wide(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    const std::wstring wdir = wide(workDir);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wexe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        wdir.empty() ? nullptr : wdir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool runAndWait(const std::string& command, int timeoutMs, int* exitCode) {
    std::wstring cmd = wide(command);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return false;
    const DWORD w = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));
    if (exitCode) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        *exitCode = static_cast<int>(code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return w == WAIT_OBJECT_0;
}

void terminateSelf(int code) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) {}                              // сюда не доходит, но компилятор требует
}

// ---------------- единственный экземпляр ----------------
bool acquireSingleInstance(const std::string& name, bool waitForPrevious) {
    const std::wstring key = L"Local\\" + wide(name);
    const int tries = waitForPrevious ? 75 : 1;          // ~15 с
    for (int i = 0; i < tries; ++i) {
        HANDLE h = CreateMutexW(nullptr, TRUE, key.c_str());
        if (!h) return true;                             // не смогли проверить — не мешаем старту
        if (GetLastError() != ERROR_ALREADY_EXISTS) { g_singleInstance = h; return true; }
        CloseHandle(h);
        if (i + 1 < tries) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

void releaseSingleInstance() {
    if (!g_singleInstance) return;
    ReleaseMutex(g_singleInstance);
    CloseHandle(g_singleInstance);
    g_singleInstance = nullptr;
}

// ---------------- сеть ----------------
namespace {
// Значения из ipifcons.h — iphlpapi.h тянет этот заголовок не всегда.
const IFTYPE kIfPPP = 23, kIfLoopback = 24, kIfPropVirtual = 53, kIfTunnel = 131;
} // namespace

std::vector<NetInterface> listIPv4Interfaces() {
    std::vector<NetInterface> out;
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
    if (rc != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;      // напр. отключённый Bluetooth
        if (a->IfType == kIfLoopback) continue;

        bool gw = false;                                    // именно НАСТОЯЩИЙ шлюз, не 0.0.0.0
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
            NetInterface n;
            n.name       = utf8FromWide(a->FriendlyName);
            n.ip         = ip;
            n.virtualIf  = virt;
            n.hasGateway = gw;
            n.type       = a->IfType;
            out.push_back(n);
        }
    }
    return out;
}

std::vector<std::string> hostIPv4Addresses() {
    std::vector<std::string> out;
    char host[256] = {0};
    if (gethostname(host, sizeof(host)) != 0) return out;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char buf[64] = {0};
        if (::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) out.push_back(buf);
    }
    freeaddrinfo(res);
    return out;
}

bool ipv4ToNumber(const std::string& ip, uint32_t& out) {
    IN_ADDR a{};
    if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    out = static_cast<uint32_t>(a.S_un.S_addr);
    return true;
}

bool arpMac(uint32_t ipLe, unsigned char out[6]) {
    ULONG mac[2] = {0, 0};
    ULONG len = 6;
    if (::SendARP(static_cast<IPAddr>(ipLe), 0, mac, &len) != NO_ERROR || len != 6) return false;
    std::memcpy(out, mac, 6);
    return true;
}

bool ipForMac(const std::string& macText, std::string& ipOut) {
    auto normalize = [](const std::string& raw) {
        std::string k;
        for (unsigned char c : raw)
            if (std::isxdigit(c)) k += static_cast<char>(std::toupper(c));
        return k;
    };
    const std::string want = normalize(macText);
    if (want.size() != 12) return false;

    PMIB_IPNET_TABLE2 tbl = nullptr;
    if (GetIpNetTable2(AF_INET, &tbl) != NO_ERROR || !tbl) return false;
    bool found = false;
    for (ULONG i = 0; i < tbl->NumEntries && !found; ++i) {
        const MIB_IPNET_ROW2& r = tbl->Table[i];
        if (r.PhysicalAddressLength != 6) continue;
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                      r.PhysicalAddress[0], r.PhysicalAddress[1], r.PhysicalAddress[2],
                      r.PhysicalAddress[3], r.PhysicalAddress[4], r.PhysicalAddress[5]);
        if (want != hex) continue;
        char ip[64] = {0};
        if (::inet_ntop(AF_INET, &r.Address.Ipv4.sin_addr, ip, sizeof(ip))) { ipOut = ip; found = true; }
    }
    FreeMibTable(tbl);
    return found;
}

// ---------------- HTTP-клиент ----------------
bool httpGet(const std::string& url, std::string* body, const std::string& destFile,
             int* statusOut) {
    const std::wstring wurl = wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

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
                if (statusOut) *statusOut = static_cast<int>(code);
                if (code == 200) {
                    FILE* f = destFile.empty() ? nullptr : openForSharedWrite(destFile);
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

// ---------------- оболочка ОС ----------------
bool openUrl(const std::string& url) {
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;           // документированный признак успеха
}

bool askYesNo(const std::string& title, const std::string& text) {
    return MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(),
                       MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
}

std::string computerName() {
    wchar_t buf[256]; DWORD n = 256;
    if (GetComputerNameW(buf, &n)) return utf8FromWide(buf);
    return {};
}

// ---------------- значок в трее и цикл событий ----------------
// Панель живёт в браузере, и без значка у программы не было бы вообще никакого
// присутствия на рабочем столе: закрыл вкладку — она работает, а выключить её
// можно разве что из диспетчера задач. Ровно это и оставляет камеру с повисшей
// сессией PC Remote (0x820A).
namespace {

const UINT WM_TRAYICON      = WM_APP + 1;
const UINT IDM_TRAY_OPEN    = 1001;
const UINT IDM_TRAY_RESTART = 1002;
const UINT IDM_TRAY_QUIT    = 1003;

TrayActions      g_tray;
HWND             g_trayWnd = nullptr;
NOTIFYICONDATAW  g_nid{};
std::atomic<bool> g_trayAdded{false};

void showTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING,    IDM_TRAY_OPEN,    L"Открыть панель");
    AppendMenuW(m, MF_STRING,    IDM_TRAY_RESTART, L"Перезапустить");
    AppendMenuW(m, MF_SEPARATOR, 0,                nullptr);
    AppendMenuW(m, MF_STRING,    IDM_TRAY_QUIT,    L"Выключить SignalBox");
    SetForegroundWindow(hwnd);                 // иначе меню не закроется по клику мимо
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);         // документированный обход бага TrackPopupMenu
    DestroyMenu(m);
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP)               showTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK && g_tray.onOpenPanel) g_tray.onOpenPanel();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRAY_OPEN:    if (g_tray.onOpenPanel) g_tray.onOpenPanel(); break;
        case IDM_TRAY_RESTART: if (g_tray.onRestart)   g_tray.onRestart();   break;
        case IDM_TRAY_QUIT:    if (g_tray.onQuit)      g_tray.onQuit();      break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void removeTrayIcon() {
    if (g_trayAdded.exchange(false)) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void runEventLoop(const TrayActions& actions, const std::function<bool()>& keepRunning) {
    g_tray = actions;

    const wchar_t* kCls = L"SignalBoxTrayWnd";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);

    // Окно только для сообщений (HWND_MESSAGE): на панели задач его нет.
    // ⚠ Именно поэтому проверять значок через FindWindow бесполезно — такие окна
    // так не находятся. Достоверный признак — строки лога ниже.
    g_trayWnd = CreateWindowExW(0, kCls, L"SignalBox", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_trayWnd) {
        logf("[tray] Не удалось создать окно значка (код %lu).\n", GetLastError());
    } else {
        logf("[tray] Окно значка создано (hwnd=%p).\n", static_cast<void*>(g_trayWnd));
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
            logf("[tray] Значок добавлен в область уведомлений.\n");
        } else {
            logf("[tray] Не удалось добавить значок (код %lu).\n", GetLastError());
        }
    }

    // Цикл не блокируется навсегда: раз в ~150 мс просыпаемся и смотрим, не пора
    // ли выходить. Выход инициирует кто угодно — панель, трей, Ctrl+C.
    bool quit = false;
    while (!quit && keepRunning()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit || !keepRunning()) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 150, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    removeTrayIcon();
    if (g_trayWnd) { DestroyWindow(g_trayWnd); g_trayWnd = nullptr; }
}

// ---------------- MIDI-вход ----------------
namespace {
std::vector<HMIDIIN> g_midiIn;
std::function<void(unsigned, unsigned, unsigned)> g_midiSink;

void CALLBACK midiCallback(HMIDIIN, UINT wMsg, DWORD_PTR, DWORD_PTR dwParam1, DWORD_PTR) {
    if (wMsg != MIM_DATA || !g_midiSink) return;
    const DWORD m = static_cast<DWORD>(dwParam1);
    g_midiSink(m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF);
}
} // namespace

bool midiStart(std::function<void(unsigned, unsigned, unsigned)> onMessage) {
    g_midiSink = std::move(onMessage);
    const UINT n = midiInGetNumDevs();
    if (n == 0) {
        logf("[midi] MIDI-устройств не найдено. Подключи контроллер и перезапусти.\n");
        return false;
    }
    logf("[midi] MIDI-входов: %u\n", n);
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            logf("   [%u] %s\n", i, utf8FromWide(caps.szPname).c_str());
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, reinterpret_cast<DWORD_PTR>(midiCallback), 0,
                       CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            midiInStart(h);
            g_midiIn.push_back(h);
        }
    }
    return !g_midiIn.empty();
}

void midiStop() {
    for (HMIDIIN h : g_midiIn) { midiInStop(h); midiInReset(h); midiInClose(h); }
    g_midiIn.clear();
}

// ---------------- брандмауэр ----------------
namespace {

const wchar_t* kFwRuleName = L"SignalBox";

// Чтение списка правил элевации не требует.
bool hasInboundAllowRule(const std::wstring& exePath) {
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

bool addOneRule(INetFwRules* rules, const std::wstring& exePath, NET_FW_IP_PROTOCOL proto) {
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

} // namespace

bool firewallSupported() { return true; }

bool firewallHasRuleForSelf() { return hasInboundAllowRule(wide(exePath())); }

bool firewallAddRulesForSelf() {
    const std::wstring exe = wide(exePath());
    bool ok = false;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))) && policy) {
        INetFwRules* rules = nullptr;
        if (SUCCEEDED(policy->get_Rules(&rules)) && rules) {
            const bool tcp = addOneRule(rules, exe, NET_FW_IP_PROTOCOL_TCP);
            const bool udp = addOneRule(rules, exe, NET_FW_IP_PROTOCOL_UDP);
            ok = tcp && udp;
            rules->Release();
        }
        policy->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

// Саму правку отдаём системному запросу прав: элевируем одноразовую копию себя,
// UAC показывает Windows. Без двух подтверждений не меняется ничего.
bool firewallRequestElevatedAdd() {
    const std::wstring exe  = wide(exePath());
    const std::wstring wdir = wide(exeDir());
    SHELLEXECUTEINFOW si{};
    si.cbSize       = sizeof(si);
    si.fMask        = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb       = L"runas";
    si.lpFile       = exe.c_str();
    si.lpParameters = L"--firewall";
    si.lpDirectory  = wdir.c_str();
    si.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&si) || !si.hProcess) {
        logf("[firewall] Не удалось запросить права администратора (код %lu).\n", GetLastError());
        return false;
    }
    WaitForSingleObject(si.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(si.hProcess, &code);
    CloseHandle(si.hProcess);
    return code == 0;
}

} // namespace plat
