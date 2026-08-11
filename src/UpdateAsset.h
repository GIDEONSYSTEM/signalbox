#pragma once
// Выбор архива релиза под свою ОС.
//
// Почему отдельным заголовком, а не строчками внутри main.cpp: живой релиз даёт
// ровно ОДИН случай из нескольких (сейчас там один архив без метки), а ошибка
// здесь ломает обновление сразу у всех студий и замечается не скоро — §7 уже
// дважды об это спотыкался. Чистая функция без сети и без файлов прогоняется
// тестом на выдуманных ответах GitHub за секунду.
//
// Соглашение об именах ассетов: метка ОС в имени файла через дефис —
// «SignalBox-1.0.6-win.zip», «SignalBox-1.0.6-mac.zip». Архив БЕЗ метки считается
// своим: так выглядят релизы по v1.0.5, где сборка была одна.
//
// ⚠️ Версии по v1.0.5 включительно выбирать не умеют — они берут ПЕРВЫЙ .zip в
// релизе. Поэтому виндовый архив заливать на GitHub первым, иначе не успевшие
// обновиться студии на Windows скачают mac-сборку.

#include <cctype>
#include <cstddef>   // size_t: у clang приезжает попутно, у MSVC — не обязан
#include <string>

namespace upd {

// Все метки ОС, какие бывают в именах архивов. Нужен именно список, а не одна
// своя метка: по нему архив ЧУЖОЙ ОС отличается от архива БЕЗ метки, а это
// разные случаи — чужой пропускаем, архив без метки берём как запасной.
inline const char* const* platformTags(int& count) {
    static const char* const kTags[] = {"win", "mac"};
    count = 2;
    return kTags;
}

// Регистронезависимый поиск подстроки; npos — не нашлось.
inline size_t findNoCase(const std::string& hay, const std::string& needle, size_t from = 0) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() &&
               std::tolower(static_cast<unsigned char>(hay[i + k])) ==
               std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

inline bool containsNoCase(const std::string& hay, const std::string& needle) {
    return findNoCase(hay, needle) != std::string::npos;
}

inline bool endsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return containsNoCase(s.substr(s.size() - suffix.size()), suffix);
}

// Метка ОС в имени файла: «-mac» перед точкой, дефисом, подчёркиванием или
// концом имени.
// 🐞 Простым поиском подстроки этого делать НЕЛЬЗЯ: «-mac» сидит внутри
// «SignalBox-machine.zip», и такой архив крал выбор у настоящей mac-сборки,
// стоящей следом (поймано тестом, не рассуждением). Поэтому после метки
// обязателен разделитель.
inline bool hasPlatformTag(const std::string& name, const std::string& tag) {
    const std::string needle = "-" + tag;
    for (size_t p = findNoCase(name, needle); p != std::string::npos;
         p = findNoCase(name, needle, p + 1)) {
        const size_t after = p + needle.size();
        if (after >= name.size()) return true;                  // метка в самом конце
        const unsigned char c = static_cast<unsigned char>(name[after]);
        if (!std::isalnum(c)) return true;                      // '.', '-', '_' и прочее
    }
    return false;
}

// Имя файла из URL — всё после последнего '/'.
inline std::string urlFileName(const std::string& u) {
    const size_t s = u.find_last_of('/');
    return (s == std::string::npos) ? u : u.substr(s + 1);
}

struct AssetChoice {
    std::string url;            // пусто — подходящего архива нет
    bool        anyZip = false; // был ли в релизе хоть один .zip (иначе релиз пустой)
};

// json — ответ GitHub на releases/latest, tag — метка своей ОС ("win"/"mac").
// Ищем по порядку ассетов: первый с меткой своей ОС побеждает; если такого нет,
// берём первый архив вообще без метки ОС.
inline AssetChoice pickReleaseAsset(const std::string& json, const std::string& tag) {
    AssetChoice out;
    std::string untagged;

    const std::string key = "\"browser_download_url\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + 1)) {
        const size_t colon = json.find(':', p);
        if (colon == std::string::npos) break;
        const size_t q1 = json.find('"', colon + 1);
        const size_t q2 = (q1 == std::string::npos) ? q1 : json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;

        const std::string u = json.substr(q1 + 1, q2 - q1 - 1);
        if (!endsWithNoCase(u, ".zip")) continue;
        out.anyZip = true;

        // Метку ищем в ИМЕНИ ФАЙЛА, а не во всём URL: в пути к релизу встречается
        // и имя владельца, и тег версии, и там легко поймать ложное совпадение.
        const std::string name = urlFileName(u);
        if (hasPlatformTag(name, tag)) { out.url = u; return out; }

        int n = 0;
        const char* const* tags = platformTags(n);
        bool foreign = false;
        for (int i = 0; i < n; ++i)
            if (hasPlatformTag(name, tags[i])) { foreign = true; break; }
        if (!foreign && untagged.empty()) untagged = u;
    }

    out.url = untagged;
    return out;
}

} // namespace upd
