#pragma once
// Одна камера студии — независимо от марки.
// Sony реализует этот интерфейс в coll::CameraSession (поверх Camera Remote SDK),
// Blackmagic — в bmd::BmdCamera (поверх их REST API и WebSocket).
//
// Интерфейс намеренно узкий: всё, что знает про конкретный SDK, протокол и набор
// свойств, остаётся внутри реализации. main.cpp про марки не знает вовсе.

#include <string>

namespace cam {

enum class Vendor { Sony, Bmd };

inline const char* vendorTag(Vendor v) { return v == Vendor::Bmd ? "bmd" : "sony"; }

class ICamera {
public:
    virtual ~ICamera() = default;

    // ---- личность ----
    virtual Vendor      vendor()       const = 0;
    // 🔴 Стабильный ключ адресации: группы, горячие клавиши и MIDI обращаются к
    // камере ТОЛЬКО по нему. У Sony это её IP — ровно то, что лежало в
    // groups.json всегда, поэтому старые файлы групп продолжают работать без
    // миграции. У Blackmagic — "bmd:<unique id>" из анонса mDNS: там адрес
    // раздаёт DHCP и он не постоянен.
    virtual std::string key()          const = 0;
    virtual int         index()        const = 0;   // порядковый номер обнаружения
    virtual std::string idLabel()      const = 0;   // "CAM 1"
    virtual std::string modelDisplay() const = 0;   // "ZV-E10 II", "PYXIS 6K"
    virtual std::string address()      const = 0;   // IP или имя — только для показа
    // Аппаратный идентификатор, если он есть: у Sony это MAC (по нему камера
    // находится после смены адреса), у остальных может быть пусто.
    virtual std::string hwId()         const { return {}; }

    // ---- состояние ----
    // Дёшево и без сетевого I/O: это читают тумблер групповой записи, горячие
    // клавиши и печать сводки — им нельзя блокироваться на камере.
    virtual bool connected() const = 0;
    virtual bool recording() const = 0;

    // ---- то, что уходит в /status.json ----
    // 🔴 Фрагмент у каждой марки СВОЙ. Набор свойств у Sony и Blackmagic разный
    // (у одной перегрев и уровень микрофона, у другой таймкод и tally), и общая
    // структура на оба вендора означала бы натянуть чужую модель свойств на одну
    // из них. Панель разбирает фрагмент по полю "vendor" и рисует свою карточку.
    // Обязательная общая часть: key, id, vendor, model, ip, online, rec.
    virtual std::string statusJson() = 0;

    // ---- команды панели (POST /cmd) ----
    // Каждая марка знает свой набор действий. Неизвестное действие — вернуть
    // false и написать в лог, но не падать: так уже устроен runAction для групп.
    virtual bool command(const std::string& action, const std::string& value) = 0;

    // ---- жизненный цикл ----
    // Sony подключается рукопожатием SDK и требует повторных попыток; у сетевых
    // камер этих понятий нет, поэтому по умолчанию — пустышки.
    virtual bool maybeRetryConnect() { return false; }
    virtual void beginDisconnect()   {}
    virtual void finishRelease()     {}

    // ---- превью ----
    // jpegOut — последний кадр, framesJson — рамки фокуса/лиц. false, если кадра
    // нет или марка превью пока не умеет.
    virtual bool grabLiveView(std::string& jpegOut, std::string& framesJson) {
        (void)jpegOut; (void)framesJson; return false;
    }
};

} // namespace cam
