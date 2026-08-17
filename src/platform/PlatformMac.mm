// Реализация платформенного слоя для macOS (Apple Silicon и Intel).
//
// Единственное место в проекте, где живут AppKit, CoreMIDI, BSD-сетевые
// интерфейсы и таблица маршрутизации. Всё остальное общается с ОС только через
// Platform.h — ровно как PlatformWin.cpp делает это на Windows.
//
// ⚠️ ЭТОТ ФАЙЛ ЕЩЁ НЕ СОБИРАЛСЯ. Писался на Windows-машине без Mac под рукой;
// первую сборку на M1 надо считать частью работы, а не проверкой готового.
//
// Отличия от Windows, о которых стоит знать заранее:
//   * SDK Sony под macOS собран БЕЗ UNICODE, поэтому CrChar там — обычный char.
//     Разруливает перегрузка plat::utf8From (см. Platform.h);
//   * своего брандмауэра с правилами «на приложение» здесь нет: система сама
//     спрашивает при первом входящем соединении, поэтому firewallSupported()
//     возвращает false и приложение ничего не спрашивает;
//   * значок в строке меню (NSStatusItem) поднимается ТОЛЬКО с главного потока —
//     ради этого HTTP-сервер и переехал в рабочий поток.

#include "Platform.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>          // RegisterEventHotKey: системные горячие клавиши без разрешений
#import <Foundation/Foundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/if_ether.h>     // sockaddr_inarp: таблица ARP
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>             // flock: замок единственного экземпляра
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

// ---- пункты меню значка ----
// Классы Objective-C обязаны жить на уровне файла: внутрь namespace их положить
// нельзя. Поэтому действия храним в статиках, а plat::runEventLoop их заполняет.
static std::function<void()> gOnOpenPanel;
static std::function<void()> gOnRestart;
static std::function<void()> gOnQuit;

@interface SignalBoxTrayTarget : NSObject
- (void)openPanel:(id)sender;
- (void)restart:(id)sender;
- (void)quit:(id)sender;
@end

@implementation SignalBoxTrayTarget
- (void)openPanel:(id)sender { (void)sender; if (gOnOpenPanel) gOnOpenPanel(); }
- (void)restart:(id)sender   { (void)sender; if (gOnRestart)   gOnRestart();   }
- (void)quit:(id)sender      { (void)sender; if (gOnQuit)      gOnQuit();      }
@end

namespace plat {
namespace {

std::function<void()> g_interrupt;
std::function<void(const std::string&)> g_log;
int  g_lockFd = -1;                       // файл-замок вместо мьютекса Windows

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

void signalHandler(int) {
    if (g_interrupt) g_interrupt();       // внутри — только выставить atomic-флаг
}

NSString* ns(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// Запустить программу через posix_spawn. wait=false — не ждём.
bool spawnProcess(const std::vector<std::string>& argv, const std::string& workDir,
                  bool wait, int timeoutMs, int* exitCode) {
    if (argv.empty()) return false;

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // Рабочую папку posix_spawn напрямую не умеет — меняем её у потомка через
    // file actions (доступно с macOS 10.15).
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    if (!workDir.empty()) posix_spawn_file_actions_addchdir_np(&acts, workDir.c_str());

    // 🔴 Потомок обязан стартовать с ЧИСТОЙ таблицей дескрипторов. По умолчанию
    // POSIX отдаёт ему всё открытое, а среди нашего открытого есть файл-замок
    // «единственный экземпляр». flock держится за описанием файла и переживает
    // exec, поэтому замок уезжал по цепочке: уходящая копия -> установщик
    // обновления -> запущенная им свежая копия. Свежая копия видела ЗАНЯТЫЙ
    // замок (занятый ею же самой, через унаследованный дескриптор), считала себя
    // вторым экземпляром и через 15 секунд ожидания выходила. Со стороны:
    // «обновилось, но не запустилось». Замерено lsof: на файле-замке висели ДВА
    // дескриптора одного и того же процесса.
    // POSIX_SPAWN_CLOEXEC_DEFAULT закрывает у потомка всё, кроме явно
    // перечисленного, и чинит этот класс ошибок целиком — заодно слушающий сокет
    // и открытый лог тоже больше никуда не утекают.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    // stdin/stdout/stderr при этом флаге тоже закрылись бы: dup2 сам в себя —
    // принятый способ сказать «эти три оставить».
    posix_spawn_file_actions_adddup2(&acts, 0, 0);
    posix_spawn_file_actions_adddup2(&acts, 1, 1);
    posix_spawn_file_actions_adddup2(&acts, 2, 2);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &acts, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&acts);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return false;
    if (!wait) return true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int status = 0;
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            if (exitCode) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            return true;
        }
        if (got < 0) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Прогнать команду через /bin/sh -c: нужно там, где на Windows была командная
// строка целиком (распаковка архива, копирование дерева).
bool shellCommand(const std::string& command, int timeoutMs, int* exitCode) {
    return spawnProcess({"/bin/sh", "-c", command}, "", true, timeoutMs, exitCode);
}

std::string normalizeMac(const std::string& raw) {
    std::string k;
    for (unsigned char c : raw)
        if (std::isxdigit(c)) k += static_cast<char>(std::toupper(c));
    return k;
}

} // namespace

// ---------------- инициализация ----------------
bool init() {
    // Запись в оборванное соединение не должна убивать процесс. SO_NOSIGPIPE в
    // HttpServer закрывает сокеты, это — всё остальное.
    ::signal(SIGPIPE, SIG_IGN);
    struct sigaction sa {};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);              // без ::, это макрос, а не функция
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    return true;
}

void shutdown() {}

const char* platformTag() { return "mac"; }

void onInterrupt(std::function<void()> handler) { g_interrupt = std::move(handler); }

void setLogger(std::function<void(const std::string&)> sink) { g_log = std::move(sink); }

// ---------------- строки ----------------
// На macOS wchar_t 4-байтный (UTF-32). Сюда попадаем только если SDK когда-либо
// соберут с UNICODE; штатный путь — перегрузка для char* в Platform.h.
std::string utf8From(const wchar_t* w) {
    if (!w) return {};
    @autoreleasepool {
        NSString* s = [[NSString alloc] initWithBytes:w
                                               length:std::wcslen(w) * sizeof(wchar_t)
                                             encoding:NSUTF32LittleEndianStringEncoding];
        return s ? std::string([s UTF8String]) : std::string();
    }
}

// ---------------- пути и файлы ----------------
std::string exePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);          // первый вызов сообщает нужный размер
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    char resolved[PATH_MAX] = {0};
    if (::realpath(buf.data(), resolved)) return resolved;
    return buf.data();
}

std::string exeDir() {
    const std::string p = exePath();
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

// Три папки, которые внутри .app-бандла расходятся. Все три написаны так, чтобы
// работать и без бандла — на случай запуска голого Contents/MacOS/SignalBox
// (и чтобы отладочная сборка не требовала бандла вокруг себя).

// bundlePath у голого бинарника — просто папка с ним, у бандла — сам .app.
// Отличаем по расширению, а не по факту «непусто».
// 🐞 Путь ОБЯЗАН быть разрешён через realpath, как и exePath(). NSBundle отдаёт
// его таким, каким приложение запустили, а exePath() — уже развёрнутым. Внутри
// TMPDIR это разные строки: `/var/folders/…` против `/private/var/folders/…`,
// потому что /var — символическая ссылка на /private/var.
// Чем это кончалось: установщик обновления считает путь к бинарнику ОТНОСИТЕЛЬНО
// installRoot() (exeRelToInstall в main.cpp). Префикс не совпадал, относительный
// путь молча превращался в абсолютный, и обновлённую копию запускали по адресу
// вида `/Applications/SignalBox.app/private/var/…` — то есть никуда. Файлы при
// этом копировались, поэтому выглядело как «обновилось, но не запустилось».
// Найдено на живом обновлении 1.0.6 -> 1.0.8; воспроизводится только из TMPDIR,
// из папки без символических ссылок всё работало.
std::string installRoot() {
    @autoreleasepool {
        NSString* p = [[NSBundle mainBundle] bundlePath];
        std::string s = p ? [p UTF8String] : std::string();
        if (!s.empty()) {
            char resolved[PATH_MAX] = {0};
            if (::realpath(s.c_str(), resolved)) s = resolved;
        }
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".app") == 0) return s;
        return exeDir();
    }
}

std::string resourceDir() {
    @autoreleasepool {
        NSString* p = [[NSBundle mainBundle] resourcePath];
        if (p) {
            const std::string s = [p UTF8String];
            // У голого бинарника resourcePath укажет на несуществующую
            // <папка>/Contents/Resources — тогда ресурсы лежат просто рядом.
            if (fileExists(s)) return s;
        }
        return exeDir();
    }
}

// ~/Library/Application Support/SignalBox — единственное место, куда программе
// в бандле можно писать. Считаем один раз: путь за время работы не меняется, а
// дёргается он часто (знакомые камеры переписываются раз в секунду).
std::string dataDir() {
    static const std::string cached = [] {
        @autoreleasepool {
            NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
                NSApplicationSupportDirectory, NSUserDomainMask, YES);
            if (dirs.count == 0) return exeDir();          // некуда — пусть будет рядом
            const std::string dir = joinPath([dirs[0] UTF8String], "SignalBox");
            if (!makeDir(dir)) return exeDir();            // не создалась — тоже рядом
            return dir;
        }
    }();
    return cached;
}

std::string tempDir() {
    @autoreleasepool {
        std::string s = [NSTemporaryDirectory() UTF8String];
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s.empty() ? std::string("/tmp") : s;
    }
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string r = a;
    if (r.back() != '/') r += '/';
    size_t i = 0;
    while (i < b.size() && b[i] == '/') ++i;
    return r + b.substr(i);
}

bool fileExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool makeDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

bool removeTree(const std::string& path) {
    if (path.empty() || !fileExists(path)) return true;
    @autoreleasepool {
        [[NSFileManager defaultManager] removeItemAtPath:ns(path) error:nil];
    }
    return !fileExists(path);
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = (bytes == 0) || (std::fwrite(data, 1, bytes, f) == bytes);
    std::fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
    return writeFile(path, data.data(), data.size());
}

bool replaceFile(const std::string& from, const std::string& to) {
    if (!fileExists(from)) return false;
    ::unlink(to.c_str());
    return ::rename(from.c_str(), to.c_str()) == 0;
}

// В отличие от Windows, здесь ничего просить не нужно: POSIX и так не запрещает
// читать открытый файл, а переименовать его можно даже при открытых дескрипторах.
FILE* openForSharedWrite(const std::string& path) {
    return std::fopen(path.c_str(), "wb");
}

bool setWorkingDir(const std::string& path) { return ::chdir(path.c_str()) == 0; }

bool extractArchive(const std::string& zipPath, const std::string& destDir) {
    int rc = 1;
    // ditto понимает zip и сохраняет права и ресурсы — это родной способ macOS.
    shellCommand("/usr/bin/ditto -x -k '" + zipPath + "' '" + destDir + "'", 120000, &rc);
    return rc == 0;
}

// Как robocopy /E без /PURGE: копируем содержимое поверх, ничего лишнего в
// приёмнике не удаляя — рядом с программой лежат данные студии.
bool copyTree(const std::string& srcDir, const std::string& dstDir) {
    int rc = 1;
    shellCommand("/usr/bin/rsync -a '" + srcDir + "/' '" + dstDir + "/'", 180000, &rc);
    return rc == 0;
}

std::string localTimeHms() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);                 // потокобезопасный вариант POSIX
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// ---------------- консоль ----------------
void writeConsole(const std::string& s) {
    if (s.empty()) return;
    ::fwrite(s.data(), 1, s.size(), stdout);
    ::fflush(stdout);
}

// ---------------- процессы ----------------
// Своих аргументов процесс в POSIX не хранит в виде строки — собираем сами, в
// том же виде, в каком main.cpp её потом ищет (--restarted, --apply-update).
// ⚠️ argv в POSIX плоский: границы аргументов при сборке строки теряются, и
// склейка через пробел даёт строку, которую нельзя разобрать обратно. Аргумент
// с пробелом поэтому возвращаем в кавычках — так же выглядит строка на Windows,
// где её отдаёт GetCommandLineW дословно.
// 🐞 Пока этого не было, установщик обновления не находил путь к цели (он ищет
// кавычки) и молча выходил: обновление на macOS не работало вовсе. Разбор в
// applyUpdate теперь переживает и отсутствие кавычек, но врать про «командную
// строку целиком» слой всё равно не должен.
std::string commandLine() {
    @autoreleasepool {
        std::string line;
        for (NSString* a in [[NSProcessInfo processInfo] arguments]) {
            const char* utf8 = [a UTF8String];
            const std::string arg = utf8 ? utf8 : "";
            if (!line.empty()) line += ' ';
            if (arg.find(' ') != std::string::npos) { line += '"'; line += arg; line += '"'; }
            else                                      line += arg;
        }
        return line;
    }
}

unsigned long currentPid() { return static_cast<unsigned long>(::getpid()); }

bool waitForProcess(unsigned long pid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    // kill(pid, 0) — проверка «жив ли», не посылая сигнала. Процесс чужой
    // (это нас запустил уходящий экземпляр), поэтому waitpid тут не годится.
    while (::kill(static_cast<pid_t>(pid), 0) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool launch(const std::string& exe, const std::string& args, const std::string& workDir) {
    std::vector<std::string> argv{exe};
    // Аргументы у нас простые: либо один флаг, либо флаг + путь в кавычках.
    std::string cur;
    bool inQuotes = false;
    for (char c : args) {
        if (c == '"') { inQuotes = !inQuotes; continue; }
        if (c == ' ' && !inQuotes) { if (!cur.empty()) { argv.push_back(cur); cur.clear(); } continue; }
        cur += c;
    }
    if (!cur.empty()) argv.push_back(cur);
    return spawnProcess(argv, workDir, false, 0, nullptr);
}

bool runAndWait(const std::string& command, int timeoutMs, int* exitCode) {
    return shellCommand(command, timeoutMs, exitCode);
}

void terminateSelf(int code) {
    ::_exit(code);                         // без раскрутки стека: штатный путь уже завис
}

// ---------------- единственный экземпляр ----------------
// Аналог именованного мьютекса — эксклюзивный flock на файле в /tmp. Замок
// снимает сама ОС, даже если процесс убили, — файл-маркер так не умеет.
bool acquireSingleInstance(const std::string& name, bool waitForPrevious) {
    const std::string path = joinPath(tempDir(), name + ".lock");
    const int tries = waitForPrevious ? 75 : 1;          // ~15 с
    for (int i = 0; i < tries; ++i) {
        // 🔴 O_CLOEXEC обязателен. flock держится за ОПИСАНИЕМ открытого файла, а
        // оно переживает fork/exec: без этого флага любой запущенный нами процесс
        // (установщик обновления!) уносит замок с собой и держит его, пока жив.
        // Кончалось это тем, что обновлённая копия считала себя вторым
        // экземпляром и мгновенно выходила — «обновилось, но не запустилось».
        if (g_lockFd < 0) g_lockFd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (g_lockFd < 0) return true;                   // не смогли проверить — не мешаем старту
        if (::flock(g_lockFd, LOCK_EX | LOCK_NB) == 0) return true;
        if (i + 1 < tries) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ::close(g_lockFd);
    g_lockFd = -1;
    return false;
}

void releaseSingleInstance() {
    if (g_lockFd < 0) return;
    ::flock(g_lockFd, LOCK_UN);
    ::close(g_lockFd);
    g_lockFd = -1;
}

// ---------------- сеть ----------------
std::vector<NetInterface> listIPv4Interfaces() {
    std::vector<NetInterface> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0 || !list) return out;

    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP)) continue;                 // выключенный интерфейс
        if (a->ifa_flags & IFF_LOOPBACK) continue;

        char ip[64] = {0};
        auto* s = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        if (!::inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip))) continue;

        NetInterface n;
        n.name = a->ifa_name ? a->ifa_name : "";
        n.ip   = ip;
        // Аналог «виртуального IfType» Windows: utun — VPN-туннели, ppp — модемы,
        // bridge/vmnet/ap — виртуальные сети Docker, Parallels, VMware, точки доступа.
        const std::string nm = n.name;
        n.virtualIf = nm.rfind("utun", 0) == 0 || nm.rfind("ppp", 0) == 0 ||
                      nm.rfind("ipsec", 0) == 0 || nm.rfind("bridge", 0) == 0 ||
                      nm.rfind("vmnet", 0) == 0 || nm.rfind("vnic", 0) == 0 ||
                      nm.rfind("ap", 0) == 0 || nm.rfind("awdl", 0) == 0 ||
                      nm.rfind("llw", 0) == 0;
        n.hasGateway = (a->ifa_flags & IFF_POINTOPOINT) == 0;   // уточняется ниже
        n.type = a->ifa_flags;                                  // в лог, как IfType на Windows
        out.push_back(n);
    }
    ::freeifaddrs(list);

    // Шлюз по умолчанию есть ровно у одного интерфейса — узнаём его имя у
    // таблицы маршрутизации и отмечаем только его. Это то же самое, что даёт
    // GAA_FLAG_INCLUDE_GATEWAYS на Windows.
    std::string defaultIf;
    {
        int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
        size_t need = 0;
        if (::sysctl(mib, 6, nullptr, &need, nullptr, 0) == 0 && need > 0) {
            std::vector<char> buf(need);
            if (::sysctl(mib, 6, buf.data(), &need, nullptr, 0) == 0) {
                char* end = buf.data() + need;
                for (char* p = buf.data(); p + sizeof(rt_msghdr) <= end; ) {
                    auto* rtm = reinterpret_cast<rt_msghdr*>(p);
                    if (rtm->rtm_msglen == 0) break;
                    auto* dst = reinterpret_cast<sockaddr*>(rtm + 1);
                    if ((rtm->rtm_flags & RTF_GATEWAY) && dst->sa_family == AF_INET &&
                        reinterpret_cast<sockaddr_in*>(dst)->sin_addr.s_addr == 0) {
                        char nameBuf[IF_NAMESIZE] = {0};
                        if (::if_indextoname(rtm->rtm_index, nameBuf)) defaultIf = nameBuf;
                        break;                                  // маршрут по умолчанию — 0.0.0.0
                    }
                    p += rtm->rtm_msglen;
                }
            }
        }
    }
    for (NetInterface& n : out) n.hasGateway = (!defaultIf.empty() && n.name == defaultIf);
    return out;
}

std::vector<std::string> hostIPv4Addresses() {
    std::vector<std::string> out;
    for (const NetInterface& n : listIPv4Interfaces()) out.push_back(n.ip);
    return out;
}

bool ipv4ToNumber(const std::string& ip, uint32_t& out) {
    in_addr a {};
    if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    out = static_cast<uint32_t>(a.s_addr);      // little-endian: 192.168.0.5 -> 0x0500A8C0
    return true;
}

// Своего SendARP в macOS нет — читаем таблицу соседей ядра (arp -a делает то же).
// Записи там появляются только после обмена пакетами, поэтому «тихий» адрес
// может не найтись, пока по нему никто не постучался. Это то же поведение, что
// и на Windows: не ответил ARP — считаем, что устройства нет.
bool arpMac(uint32_t ipLe, unsigned char out[6]) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_LLINFO};
    size_t need = 0;
    if (::sysctl(mib, 6, nullptr, &need, nullptr, 0) != 0 || need == 0) return false;
    std::vector<char> buf(need);
    if (::sysctl(mib, 6, buf.data(), &need, nullptr, 0) != 0) return false;

    char* end = buf.data() + need;
    for (char* p = buf.data(); p + sizeof(rt_msghdr) <= end; ) {
        auto* rtm = reinterpret_cast<rt_msghdr*>(p);
        if (rtm->rtm_msglen == 0) break;
        auto* sin = reinterpret_cast<sockaddr_inarp*>(rtm + 1);
        auto* sdl = reinterpret_cast<sockaddr_dl*>(
            reinterpret_cast<char*>(sin) + ((sin->sin_len + 3) & ~3));
        if (sin->sin_addr.s_addr == ipLe && sdl->sdl_alen == 6) {
            std::memcpy(out, LLADDR(sdl), 6);
            return true;
        }
        p += rtm->rtm_msglen;
    }
    return false;
}

bool ipForMac(const std::string& macText, std::string& ipOut) {
    const std::string want = normalizeMac(macText);
    if (want.size() != 12) return false;

    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_LLINFO};
    size_t need = 0;
    if (::sysctl(mib, 6, nullptr, &need, nullptr, 0) != 0 || need == 0) return false;
    std::vector<char> buf(need);
    if (::sysctl(mib, 6, buf.data(), &need, nullptr, 0) != 0) return false;

    char* end = buf.data() + need;
    for (char* p = buf.data(); p + sizeof(rt_msghdr) <= end; ) {
        auto* rtm = reinterpret_cast<rt_msghdr*>(p);
        if (rtm->rtm_msglen == 0) break;
        auto* sin = reinterpret_cast<sockaddr_inarp*>(rtm + 1);
        auto* sdl = reinterpret_cast<sockaddr_dl*>(
            reinterpret_cast<char*>(sin) + ((sin->sin_len + 3) & ~3));
        if (sdl->sdl_alen == 6) {
            const unsigned char* m = reinterpret_cast<const unsigned char*>(LLADDR(sdl));
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
            if (want == hex) {
                char ip[64] = {0};
                if (::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
                    ipOut = ip;
                    return true;
                }
            }
        }
        p += rtm->rtm_msglen;
    }
    return false;
}

// ---------------- HTTP-клиент ----------------
// NSURLSession сам отрабатывает редиректы (ссылки GitHub уводят на CDN) и не
// требует ни одной сторонней библиотеки — здешний аналог WinHTTP.
bool httpGet(const std::string& url, std::string* body, const std::string& destFile,
             int* statusOut) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:ns(url)];
        if (!u) return false;

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
        [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        [req setValue:@"SignalBox" forHTTPHeaderField:@"User-Agent"];
        req.timeoutInterval = 15.0;

        __block NSData*       data     = nil;
        __block NSInteger     status   = 0;
        __block bool          failed   = false;
        dispatch_semaphore_t  done     = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:req
              completionHandler:^(NSData* d, NSURLResponse* resp, NSError* err) {
                  if (err) failed = true;
                  if ([resp isKindOfClass:[NSHTTPURLResponse class]])
                      status = [(NSHTTPURLResponse*)resp statusCode];
                  data = d;
                  dispatch_semaphore_signal(done);
              }];
        [task resume];
        // Сеть дёргается из фонового потока (см. main.cpp), блокироваться здесь можно.
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30ll * NSEC_PER_SEC));

        if (statusOut) *statusOut = static_cast<int>(status);
        if (failed || status != 200 || !data) return false;

        if (body) body->append(static_cast<const char*>(data.bytes), data.length);
        if (!destFile.empty() && ![data writeToFile:ns(destFile) atomically:YES]) return false;
        return true;
    }
}

// ---------------- оболочка ОС ----------------
bool openUrl(const std::string& url) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:ns(url)];
        return u && [[NSWorkspace sharedWorkspace] openURL:u];
    }
}

bool askYesNo(const std::string& title, const std::string& text) {
    @autoreleasepool {
        NSAlert* a = [[NSAlert alloc] init];
        a.messageText     = ns(title);
        a.informativeText = ns(text);
        [a addButtonWithTitle:@"Да"];
        [a addButtonWithTitle:@"Нет"];
        return [a runModal] == NSAlertFirstButtonReturn;
    }
}

std::string computerName() {
    @autoreleasepool {
        NSString* n = [[NSHost currentHost] localizedName];
        return n ? std::string([n UTF8String]) : std::string();
    }
}

// ---------------- значок в строке меню и цикл событий ----------------
namespace {
NSStatusItem*     g_statusItem = nil;
std::atomic<bool> g_trayAdded{false};
} // namespace

void removeTrayIcon() {
    if (!g_trayAdded.exchange(false)) return;
    @autoreleasepool {
        if (g_statusItem) {
            [[NSStatusBar systemStatusBar] removeStatusItem:g_statusItem];
            g_statusItem = nil;
        }
    }
}

void runEventLoop(const TrayActions& actions, const std::function<bool()>& keepRunning) {
    gOnOpenPanel = actions.onOpenPanel;
    gOnRestart   = actions.onRestart;
    gOnQuit      = actions.onQuit;

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        // Accessory: значок в строке меню есть, иконки в Dock и главного окна нет —
        // ровно как message-only окно на Windows.
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        static SignalBoxTrayTarget* target = [[SignalBoxTrayTarget alloc] init];

        g_statusItem = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSVariableStatusItemLength];
        if (!g_statusItem) {
            logf("[tray] Не удалось создать значок в строке меню.\n");
        } else {
            // Тот же логотип Wi-Fi, что в res/icon.ico на Windows, но системным
            // символом: в строке меню полагается монохромная template-картинка —
            // она сама подстраивается под светлую и тёмную тему и под высоту
            // строки, а цветная иконка там выглядит чужеродно и мылится.
            // ⚠️ setTemplate: сообщением, а не через точку: свойство называется
            // template, а это ключевое слово C++, и файл собирается как ObjC++.
            NSImage* icon = [NSImage imageWithSystemSymbolName:@"wifi"
                                     accessibilityDescription:@"SignalBox"];
            if (icon) {
                [icon setTemplate:YES];
                g_statusItem.button.image = icon;
            } else {
                g_statusItem.button.title = @"◉";    // без символа — хоть что-то видимое
            }
            g_statusItem.button.toolTip = @"SignalBox";

            NSMenu* menu = [[NSMenu alloc] init];
            [[menu addItemWithTitle:@"Открыть панель" action:@selector(openPanel:) keyEquivalent:@""]
                setTarget:target];
            [[menu addItemWithTitle:@"Перезапустить" action:@selector(restart:) keyEquivalent:@""]
                setTarget:target];
            [menu addItem:[NSMenuItem separatorItem]];
            [[menu addItemWithTitle:@"Выключить SignalBox" action:@selector(quit:) keyEquivalent:@""]
                setTarget:target];
            g_statusItem.menu = menu;

            g_trayAdded.store(true);
            // Какой именно знак получился — видно только глазами, поэтому пишем
            // в лог: иначе «значок не тот» нечем отличить от «символ не нашёлся».
            logf("[tray] Значок добавлен в строку меню (%s).\n",
                 icon ? "символ wifi" : "запасной знак ◉");
        }

        // Тот же принцип, что на Windows: не засыпаем навсегда, раз в ~150 мс
        // просыпаемся и проверяем, не пора ли выходить.
        [app finishLaunching];
        while (keepRunning()) {
            @autoreleasepool {
                NSEvent* e = [app nextEventMatchingMask:NSEventMaskAny
                                              untilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]
                                                 inMode:NSDefaultRunLoopMode
                                                dequeue:YES];
                if (e) [app sendEvent:e];
            }
        }
    }

    removeTrayIcon();
}

// ---------------- системные горячие клавиши ----------------
// Carbon RegisterEventHotKey, а НЕ CGEventTap: тап читает весь ввод в системе и
// требует разрешения «Мониторинг ввода», а регистрация конкретной комбинации
// работает без разрешений вовсе. API старый, но поддерживается и на arm64.
namespace {

std::function<void(int)> g_hotkeyCb;
std::vector<EventHotKeyRef> g_hotkeyRefs;
EventHandlerRef g_hotkeyHandler = nullptr;

// Каноническое имя -> виртуальный код macOS. Коды другие, чем на Windows,
// но ИМЕНА общие: они хранятся в groups.json и должны означать одно и то же.
UInt32 keyCodeFromName(const std::string& n) {
    if (n.size() == 1) {
        static const std::string letters = "asdfhgzxcv bqweryt123465=97-80]ou[ip lj'k;\\,/nm.";
        // раскладка кодов у Carbon нелинейная, поэтому таблица явная
        static const std::pair<char, UInt32> kChars[] = {
            {'A',kVK_ANSI_A},{'B',kVK_ANSI_B},{'C',kVK_ANSI_C},{'D',kVK_ANSI_D},{'E',kVK_ANSI_E},
            {'F',kVK_ANSI_F},{'G',kVK_ANSI_G},{'H',kVK_ANSI_H},{'I',kVK_ANSI_I},{'J',kVK_ANSI_J},
            {'K',kVK_ANSI_K},{'L',kVK_ANSI_L},{'M',kVK_ANSI_M},{'N',kVK_ANSI_N},{'O',kVK_ANSI_O},
            {'P',kVK_ANSI_P},{'Q',kVK_ANSI_Q},{'R',kVK_ANSI_R},{'S',kVK_ANSI_S},{'T',kVK_ANSI_T},
            {'U',kVK_ANSI_U},{'V',kVK_ANSI_V},{'W',kVK_ANSI_W},{'X',kVK_ANSI_X},{'Y',kVK_ANSI_Y},
            {'Z',kVK_ANSI_Z},
            {'0',kVK_ANSI_0},{'1',kVK_ANSI_1},{'2',kVK_ANSI_2},{'3',kVK_ANSI_3},{'4',kVK_ANSI_4},
            {'5',kVK_ANSI_5},{'6',kVK_ANSI_6},{'7',kVK_ANSI_7},{'8',kVK_ANSI_8},{'9',kVK_ANSI_9},
        };
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
        for (const auto& kv : kChars) if (kv.first == c) return kv.second;
        (void)letters;
        return 0xFFFF;
    }
    if ((n[0] == 'F' || n[0] == 'f') && n.size() >= 2) {
        static const UInt32 kF[] = {kVK_F1,kVK_F2,kVK_F3,kVK_F4,kVK_F5,kVK_F6,kVK_F7,kVK_F8,
                                    kVK_F9,kVK_F10,kVK_F11,kVK_F12,kVK_F13,kVK_F14,kVK_F15,
                                    kVK_F16,kVK_F17,kVK_F18,kVK_F19,kVK_F20};
        const int num = std::atoi(n.c_str() + 1);
        if (num >= 1 && num <= 20) return kF[num - 1];
    }
    if (n.rfind("Numpad", 0) == 0 && n.size() == 7) {
        static const UInt32 kNum[] = {kVK_ANSI_Keypad0,kVK_ANSI_Keypad1,kVK_ANSI_Keypad2,
                                      kVK_ANSI_Keypad3,kVK_ANSI_Keypad4,kVK_ANSI_Keypad5,
                                      kVK_ANSI_Keypad6,kVK_ANSI_Keypad7,kVK_ANSI_Keypad8,
                                      kVK_ANSI_Keypad9};
        const unsigned char d = static_cast<unsigned char>(n[6]);
        if (d >= '0' && d <= '9') return kNum[d - '0'];
    }
    static const std::pair<const char*, UInt32> kNamed[] = {
        {"Space", kVK_Space}, {"Enter", kVK_Return}, {"Tab", kVK_Tab},
        {"Left", kVK_LeftArrow}, {"Right", kVK_RightArrow}, {"Up", kVK_UpArrow}, {"Down", kVK_DownArrow},
        {"Insert", kVK_Help}, {"Delete", kVK_ForwardDelete}, {"Home", kVK_Home}, {"End", kVK_End},
        {"PageUp", kVK_PageUp}, {"PageDown", kVK_PageDown}, {"Backspace", kVK_Delete},
    };
    for (const auto& kv : kNamed) if (n == kv.first) return kv.second;
    return 0xFFFF;
}

OSStatus hotkeyEvent(EventHandlerCallRef, EventRef ev, void*) {
    EventHotKeyID id{};
    if (GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                          sizeof(id), nullptr, &id) == noErr && g_hotkeyCb)
        g_hotkeyCb(static_cast<int>(id.id));
    return noErr;
}

} // namespace

bool hotkeysSupported() { return true; }

void onHotkey(std::function<void(int)> handler) { g_hotkeyCb = std::move(handler); }

std::vector<int> hotkeysApply(const std::vector<std::pair<int, HotKey>>& keys) {
    std::vector<int> failed;

    for (EventHotKeyRef r : g_hotkeyRefs) UnregisterEventHotKey(r);
    g_hotkeyRefs.clear();

    if (!g_hotkeyHandler && !keys.empty()) {
        EventTypeSpec spec{ kEventClassKeyboard, kEventHotKeyPressed };
        InstallApplicationEventHandler(&hotkeyEvent, 1, &spec, nullptr, &g_hotkeyHandler);
    }

    for (const auto& kv : keys) {
        const UInt32 code = keyCodeFromName(kv.second.key);
        if (code == 0xFFFF) { failed.push_back(kv.first); continue; }
        UInt32 mods = 0;
        if (kv.second.ctrl)  mods |= controlKey;
        if (kv.second.alt)   mods |= optionKey;
        if (kv.second.shift) mods |= shiftKey;
        if (kv.second.meta)  mods |= cmdKey;
        EventHotKeyID hid{};
        hid.signature = 'SgBx';
        hid.id = static_cast<UInt32>(kv.first);
        EventHotKeyRef ref = nullptr;
        if (RegisterEventHotKey(code, mods, hid, GetApplicationEventTarget(), 0, &ref) == noErr && ref)
            g_hotkeyRefs.push_back(ref);
        else
            failed.push_back(kv.first);       // комбинацию занял кто-то другой
    }
    return failed;
}

void hotkeysClear() { hotkeysApply({}); }

// ---------------- MIDI-вход ----------------
namespace {
MIDIClientRef  g_midiClient = 0;
MIDIPortRef    g_midiPort   = 0;
std::function<void(unsigned, unsigned, unsigned)> g_midiSink;

// CoreMIDI зовёт это из своего высокоприоритетного потока: только разобрать и
// отдать наверх, ничего тяжёлого.
void midiReadProc(const MIDIPacketList* pktlist, void*, void*) {
    if (!g_midiSink) return;
    const MIDIPacket* p = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
        for (UInt16 k = 0; k + 2 < p->length; k += 3)
            g_midiSink(p->data[k], p->data[k + 1], p->data[k + 2]);
        p = MIDIPacketNext(p);
    }
}
} // namespace

bool midiStart(std::function<void(unsigned, unsigned, unsigned)> onMessage) {
    g_midiSink = std::move(onMessage);

    const ItemCount sources = MIDIGetNumberOfSources();
    if (sources == 0) {
        logf("[midi] MIDI-устройств не найдено. Подключи контроллер и перезапусти.\n");
        return false;
    }
    if (MIDIClientCreate(CFSTR("SignalBox"), nullptr, nullptr, &g_midiClient) != noErr)
        return false;
    if (MIDIInputPortCreate(g_midiClient, CFSTR("SignalBox In"), midiReadProc, nullptr,
                            &g_midiPort) != noErr)
        return false;

    logf("[midi] MIDI-входов: %lu\n", static_cast<unsigned long>(sources));
    bool any = false;
    for (ItemCount i = 0; i < sources; ++i) {
        MIDIEndpointRef src = MIDIGetSource(i);
        CFStringRef nameRef = nullptr;
        if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &nameRef) == noErr && nameRef) {
            char nameBuf[160] = {0};
            CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
            CFRelease(nameRef);
            logf("   [%lu] %s\n", static_cast<unsigned long>(i), nameBuf);
        }
        if (MIDIPortConnectSource(g_midiPort, src, nullptr) == noErr) any = true;
    }
    return any;
}

void midiStop() {
    if (g_midiPort)   { MIDIPortDispose(g_midiPort);     g_midiPort = 0; }
    if (g_midiClient) { MIDIClientDispose(g_midiClient); g_midiClient = 0; }
    g_midiSink = nullptr;
}

// ---------------- брандмауэр ----------------
// Понятия «правило на приложение, выданное заранее» в macOS нет: встроенный
// фильтр по умолчанию выключен, а когда включён — система сама однократно
// спрашивает про входящие соединения при первом же из них. Спрашивать нечего,
// поэтому приложение эту ветку пропускает целиком.
bool firewallSupported()          { return false; }
bool firewallHasRuleForSelf()     { return true; }   // «разрешение уже есть» — не мешаем старту
bool firewallAddRulesForSelf()    { return true; }
bool firewallRequestElevatedAdd() { return true; }

} // namespace plat
