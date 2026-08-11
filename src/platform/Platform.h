#pragma once
// Платформенный слой SignalBox.
//
// Всё, что зависит от операционной системы, живёт ЗА этим интерфейсом:
// пути и файлы, консоль, процессы, сеть, HTTP-клиент, трей, MIDI, брандмауэр.
// Остальной код (main.cpp, CameraSession, HttpServer) про ОС ничего не знает —
// поэтому порт на macOS сводится к одной новой реализации этого заголовка,
// а не к распутыванию Windows-вызовов по всему проекту.
//
// Правила слоя:
//   * здесь НЕТ windows.h и любых системных заголовков — только стандартная
//     библиотека, иначе платформенные типы всё равно расползутся по проекту;
//   * все строки и пути — UTF-8 в std::string; конверсия в wchar_t происходит
//     внутри PlatformWin.cpp. Отсюда же берётся utf8FromWide: SDK Sony отдаёт
//     wchar_t, и это единственное место, где он просачивается наружу;
//   * функции никогда не бросают исключений: возвращают bool/пустую строку.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace plat {

// ---------------- инициализация ----------------
// Вызвать первым делом в main(): кодировка консоли, сокетная подсистема и
// прочая разовая подготовка. Парная shutdown() — перед выходом.
bool init();
void shutdown();

// Обработчик Ctrl+C / закрытия окна. Вызывается из системного потока —
// внутри допустимо только выставить флаг.
void onInterrupt(std::function<void()> handler);

// Куда слой пишет диагностику: своего лога у него нет, а сказать про значок в
// трее или список MIDI-входов надо. Не задан — сообщения просто теряются.
void setLogger(std::function<void(const std::string&)> sink);

// ---------------- строки ----------------
// SDK Sony отдаёт строки типом CrChar, а он зависит от платформы: под Windows
// (сборка с UNICODE) это wchar_t, под macOS — обычный char в UTF-8. Перегрузки
// дают вызывающему коду писать plat::utf8From(info->GetModel()) и не думать,
// какой сегодня CrChar. Больше wchar_t в приложении не встречается нигде.
std::string utf8From(const wchar_t* w);
inline std::string utf8From(const char* s) { return s ? std::string(s) : std::string(); }

// ---------------- пути и файлы ----------------
// Пути — UTF-8. Разделитель подставляет joinPath, руками '\\' не писать.
std::string exePath();                    // полный путь к своему exe
std::string exeDir();                     // папка, где лежит exe
std::string tempDir();                    // системная папка временных файлов
std::string joinPath(const std::string& a, const std::string& b);

bool fileExists(const std::string& path);
bool makeDir(const std::string& path);    // один уровень; уже есть — тоже true
bool removeTree(const std::string& path); // рекурсивно, молча

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const void* data, size_t bytes);
bool writeFile(const std::string& path, const std::string& data);
// Переименование с заменой приёмника (ротация лога).
bool replaceFile(const std::string& from, const std::string& to);

// Открыть файл на запись так, чтобы его МОЖНО было читать, пока мы пишем.
// Ровно для лога: без консоли он единственный способ понять, что происходит,
// и блокировать его от Notepad/PowerShell нельзя. nullptr при неудаче.
FILE* openForSharedWrite(const std::string& path);

bool setWorkingDir(const std::string& path);

// Распаковать zip в папку destDir (она уже существует).
bool extractArchive(const std::string& zipPath, const std::string& destDir);
// Скопировать содержимое srcDir поверх dstDir РЕКУРСИВНО, не удаляя в приёмнике
// то, чего нет в источнике: рядом с программой лежат данные студии
// (cameras.txt, groups.json, settings.json), и обновление не должно их стирать.
bool copyTree(const std::string& srcDir, const std::string& dstDir);

// ---------------- консоль ----------------
// Печать UTF-8 в консоль, если она есть (в GUI-подсистеме её нет — тогда
// вызов ничего не делает). Лог пишет вызывающий код, не платформа.
void writeConsole(const std::string& utf8);

// ---------------- процессы ----------------
std::string   commandLine();              // своя командная строка целиком
unsigned long currentPid();
bool          waitForProcess(unsigned long pid, int timeoutMs);

// Запустить программу и не ждать её. args — уже готовая строка аргументов.
bool launch(const std::string& exe, const std::string& args, const std::string& workDir);
// Запустить командную строку скрыто и дождаться конца. false — не запустилось
// или не уложилось в timeoutMs. exitCode может быть nullptr.
bool runAndWait(const std::string& command, int timeoutMs, int* exitCode);
// Аварийный выход, когда штатное завершение зависло в блокирующем вызове SDK.
[[noreturn]] void terminateSelf(int code);

// ---------------- единственный экземпляр ----------------
// Несколько копий дерутся за порт 8787, и панель может прийти из устаревшей
// папки. waitForPrevious=true — подождать, пока уходящая копия отпустит слот
// (самоперезапуск). Не удалось определить — считаем, что можно (не блокируем старт).
bool acquireSingleInstance(const std::string& name, bool waitForPrevious);
void releaseSingleInstance();

// ---------------- сеть ----------------
// Один IPv4-адрес одного интерфейса. Взвешивает кандидатов вызывающий код
// (какой адрес показать в QR), платформа только сообщает факты о железе.
struct NetInterface {
    std::string  name;              // человекочитаемое имя — для лога
    std::string  ip;                // IPv4
    bool         virtualIf = false; // туннель / PPP / виртуальный адаптер: VPN, Hyper-V, Docker
    bool         hasGateway = false;// есть НАСТОЯЩИЙ шлюз (не 0.0.0.0)
    unsigned int type = 0;          // системный код типа интерфейса — только для лога
};
std::vector<NetInterface> listIPv4Interfaces();
// Запасной путь, если таблицу интерфейсов прочитать не удалось: все адреса,
// которыми отзывается сам хост.
std::vector<std::string> hostIPv4Addresses();

// IPv4 -> число в том порядке байт, который ждёт SDK Sony: 1-й октет в младшем
// байте (192.168.0.5 = 0x0500A8C0). Ровно это даёт inet_pton на little-endian —
// и Windows x64, и Apple Silicon такие. htonl здесь НЕ нужен.
bool ipv4ToNumber(const std::string& ip, uint32_t& out);

// MAC устройства по адресу (ARP). false — адрес не ответил, т.е. по нему никого
// нет: так мы не создаём сессии для выключенных камер.
bool arpMac(uint32_t ipLe, unsigned char out[6]);
// Обратный поиск по таблице соседей: камеру мог переселить DHCP.
bool ipForMac(const std::string& macText, std::string& ipOut);

// ---------------- HTTP-клиент ----------------
// GET с обработкой редиректов (ссылки GitHub на архивы уводят на CDN).
// Тело кладём в body и/или в файл destFile — любой из них может быть пуст.
// statusOut получает код ответа (0, если до ответа не дошло).
bool httpGet(const std::string& url, std::string* body, const std::string& destFile,
             int* statusOut);

// ---------------- оболочка ОС ----------------
bool openUrl(const std::string& url);
// Модальный вопрос пользователю. false — «нет» ИЛИ спросить негде.
bool askYesNo(const std::string& title, const std::string& text);
std::string computerName();

// ---------------- значок в трее и цикл событий ----------------
// Что делают пункты меню значка. Вызываются из потока цикла событий.
struct TrayActions {
    std::function<void()> onOpenPanel;
    std::function<void()> onRestart;
    std::function<void()> onQuit;
};

// Поднимает значок и крутит цикл событий ОС, пока keepRunning() возвращает true.
//
// ВЫЗЫВАТЬ ТОЛЬКО ИЗ ГЛАВНОГО ПОТОКА. На Windows это лишь удобно, а на macOS
// обязательно: AppKit поднимает status bar item исключительно с главного потока.
// Поэтому HTTP-сервер живёт в рабочем потоке, а не наоборот.
//
// Если на платформе значка нет, функция обязана просто ждать, пока keepRunning()
// не станет false, — иначе main завершится, не начав работу.
void runEventLoop(const TrayActions& actions, const std::function<bool()>& keepRunning);

// Снять значок, не дожидаясь конца цикла событий (аварийный выход по сторожу).
void removeTrayIcon();

// ---------------- MIDI-вход ----------------
// Пульт-контроллер переключает запись на всех камерах. Возможность
// необязательная: нет реализации — просто false, приложение работает дальше.
// onMessage зовётся из системного потока: делать в нём тяжёлое нельзя, только
// положить сообщение в очередь.
bool midiStart(std::function<void(unsigned status, unsigned data1, unsigned data2)> onMessage);
void midiStop();

// ---------------- брандмауэр ----------------
// Камеры отвечают на обнаружение ВХОДЯЩИМ UDP, панель отдаётся по входящему TCP.
// Windows выдаёт разрешение только для профиля сети, отмеченного в разовом окне,
// — поэтому ПК, работающий в одной студии, замолкает в другой. Здесь разрешение
// выдаётся сразу для всех типов сетей.
//
// Где такого понятия нет — firewallSupported() = false, и приложение не спрашивает.
bool firewallSupported();
bool firewallHasRuleForSelf();
// Собственно добавление правил: требует прав администратора, поэтому вызывается
// в элевированной копии процесса (ветка --firewall).
bool firewallAddRulesForSelf();
// Запросить элевацию у ОС и дождаться результата (показывает системный запрос).
bool firewallRequestElevatedAdd();

} // namespace plat
