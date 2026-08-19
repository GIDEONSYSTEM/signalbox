#pragma once
// Одна камера Blackmagic за общим интерфейсом cam::ICamera.
//
// Управление идёт по их control-API (HTTP REST), без бинарного SDK: линковать
// нечего, чужих dylib нет. Состояние камера отдаёт по запросу, а в перспективе
// шлёт сама по WebSocket — поэтому опрос живёт в СВОЁМ потоке и складывает
// готовый фрагмент JSON в кэш: поток /status.json обязан отдавать его без
// единого сетевого вызова (у Sony там живой I/O к SDK, и это его беда, §16.3).

#include "ICamera.h"
#include "net/WsClient.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmd {

// Куда писать диагностику камер BM (как coll::setLogSink у Sony).
void setLogSink(std::function<void(const std::string&)> sink);

// Кого будить, когда состояние камеры изменилось. Нужен, чтобы /status.json
// пересобирался ПО СОБЫТИЮ, а не по таймеру: камера присылает изменение сама,
// и ждать следующего такта опроса означало бы добавлять задержку на ровном месте.
void setOnChanged(std::function<void()> cb);

// То, что известно о камере из анонса mDNS (или задано вручную).
struct Found {
    std::string host;              // IP или имя вида PYXIS-6K-3.local
    int         port = 80;
    std::string uniqueId;          // стабильный идентификатор из TXT
    std::string productName;       // "PYXIS 6K"
    std::string deviceName;        // "PYXIS 6K 3" — как названа в Camera Setup
};

class BmdCamera : public cam::ICamera {
public:
    BmdCamera(int index, const Found& f);
    ~BmdCamera() override;

    BmdCamera(const BmdCamera&)            = delete;
    BmdCamera& operator=(const BmdCamera&) = delete;

    // ---- cam::ICamera ----
    cam::Vendor  vendor()       const override { return cam::Vendor::Bmd; }
    // 🔴 Ключ — unique id, а НЕ адрес: у камер BM адрес раздаёт DHCP, и в разных
    // студиях он разный. Префикс отделяет их от сониевских ключей (там ключ — IP).
    std::string  key()          const override { return "bmd:" + m_uniqueId; }
    int          index()        const override { return m_index; }
    std::string  idLabel()      const override { return m_idLabel; }
    std::string  modelDisplay() const override { return m_model; }
    std::string  address()      const override;
    std::string  hwId()         const override { return m_uniqueId; }
    bool         connected()    const override { return m_online.load(); }
    bool         recording()    const override { return m_rec.load(); }
    std::string  statusJson()         override;
    bool         command(const std::string& action, const std::string& value) override;
    void         beginDisconnect()    override;
    void         finishRelease()      override;

    // Адрес мог смениться (DHCP) — камера та же, ключ тот же.
    void updateHost(const std::string& host, int port);

private:
    void pollLoop();
    void pollOnce(bool full);          // снимок состояния обычным REST
    void rebuildCache();               // собрать фрагмент /status.json из полей
    void readSupportedLists();         // допустимые значения и диапазоны — с камеры
    bool openWs();                     // подключиться и подписаться
    // Что изменилось после разбора события подписки. Таймкод выделен отдельно:
    // он приходит ~12 раз в секунду, и пересобирать из-за него /status.json так
    // же часто незачем — панель опрашивает реже и всё равно этого не увидит.
    enum class Change { None, TimecodeOnly, Other };
    Change applyEvent(const std::string& msg);   // разобрать propertyValueChanged
    void refreshTimecode();            // таймкод одним REST-запросом

    // 🔴 Сверка применения. У BM код 204 значит «команда принята», а НЕ
    // «подействовало» (§15.4): камера может молча не применить значение — так
    // было с записью без карты. Поэтому после записи запоминаем, чего ждём, и
    // сверяем на следующих проходах; не сошлось — досылаем, но ограниченное
    // число раз, иначе программа будет спорить с человеком, крутящим ручку на
    // самой камере (тот же приём, что reconcileLocked у Sony).
    struct Pending {
        std::string path;      // "/video/iso"
        std::string field;     // "iso"
        long long   want = 0;
        int         tries = 0;
    };
    static constexpr int kSetRetries = 3;

    bool writeNum(const std::string& path, const std::string& field, long long v);
    void reconcile();          // только из потока опроса

    std::mutex                      m_pendMx;
    std::map<std::string, Pending>  m_pending;   // поле -> чего ждём

    int                m_index;
    std::string        m_idLabel;
    std::string        m_uniqueId;
    std::string        m_model;
    std::string        m_deviceName;

    mutable std::mutex m_hostMx;
    std::string        m_host;
    int                m_port = 80;

    std::atomic<bool>  m_online{false};
    std::atomic<bool>  m_rec{false};
    // Камера найдена, но REST-управление на ней не включено (Web Media Manager).
    // Показываем это в панели явно: иначе оператор решит, что камера сломалась.
    std::atomic<bool>  m_apiOff{false};

    mutable std::mutex m_cacheMx;
    std::string        m_cache;          // готовый фрагмент /status.json

    // Редко меняющееся: обновляется раз в несколько проходов. Трогает только
    // поток опроса, поэтому мьютекс не нужен — но и thread_local тут не место.
    struct Slow {
        std::string codec, fps, dynRange, tally, powerSrc, shutterMeas;
        long long   w = -1, h = -1, milliVolt = -1;
        long long   iso = -1, gain = -1, wb = -1, tint = -1, shutter = -1;
        double      iris = -1.0;
        bool        irisControllable = false;
        // Допустимые значения отдаёт САМА камера — зашитых таблиц нет (§10.0.1).
        std::vector<long long> isoOpts, gainOpts;
        // Выдержку камера показывает ЛИБО скоростью (1/50), ЛИБО углом (180°) —
        // режим у каждой камеры свой, поэтому храним оба списка и отдаём тот,
        // что соответствует текущему режиму.
        std::vector<long long> shutterSpeedOpts, shutterAngleOpts;
        long long   wbMin = -1, wbMax = -1, tintMin = 0, tintMax = 0;
        std::string autoExposure;
    };
    Slow               m_slow;            // только из потока опроса
    std::string        m_tc;              // таймкод (часы камеры), только из потока опроса
    std::string        m_recTime;         // длительность на таймлайне = время записи

    // 🔴 Состояние приходит подпиской, а не опросом: камера сама шлёт
    // propertyValueChanged, и push успевает раньше, чем завершится обычный
    // REST-запрос. Опрос остаётся запасным путём — на случай, когда WebSocket
    // не поднялся или оборвался.
    ws::Client         m_ws;
    bool               m_wsReady = false;
    // Таймкод приходит подпиской и сыплет ~12.5 событиями в секунду. Само по
    // себе это дёшево (1.85 КБ/с по уже открытому соединению, новых TCP нет),
    // но пересобирать из-за него /status.json 12 раз в секунду незачем: панель
    // опрашивает раз в 100 мс и чаще всё равно ничего не увидит.
    std::chrono::steady_clock::time_point m_lastTcRebuild{};
    // ⚠️ Сверку тоже нельзя гнать на каждом событии: три попытки уложились бы в
    // четверть секунды, и мы бы сдались раньше, чем камера успела применить.
    std::chrono::steady_clock::time_point m_lastReconcile{};
    // Формат сменился — списки допустимых выдержек зависят от частоты кадров,
    // поэтому их надо перечитать, не дожидаясь переподключения.
    bool               m_listsStale = false;

    std::atomic<bool>  m_run{true};
    std::thread        m_thread;
};

} // namespace bmd
