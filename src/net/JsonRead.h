#pragma once
// Достаём скаляры из ответов camera-API. Ответы Blackmagic — плоские объекты
// вроде {"whiteBalance":5500} или {"source":"AC","milliVolt":11900}, ключи на
// верхнем уровне уникальны, поэтому хватает точечного «найти "ключ": и разобрать
// значение». Это честнее, чем jsonGet в main.cpp (поиск подстроки): здесь мы
// стоим на двоеточии после ключа и парсим ровно одно значение.
//
// Не полноценный парсер: вложенные объекты не разбираем, для нужных полей их и
// нет. Если понадобится значение внутри под-объекта — сначала берём подстроку
// этого под-объекта (см. numAfter).

#include <cstdlib>
#include <string>

namespace jsonr {

// Позиция значения ключа: индекс первого не-пробельного символа после "ключ":
inline size_t valuePos(const std::string& j, const std::string& key, size_t from = 0) {
    const std::string pat = "\"" + key + "\"";
    size_t k = j.find(pat, from);
    if (k == std::string::npos) return std::string::npos;
    k = j.find(':', k + pat.size());
    if (k == std::string::npos) return std::string::npos;
    ++k;
    while (k < j.size() && (j[k] == ' ' || j[k] == '\t')) ++k;
    return k;
}

// Строковое значение: "ключ":"...". found=false, если ключа нет или он не строка.
inline std::string str(const std::string& j, const std::string& key, bool* found = nullptr) {
    size_t k = valuePos(j, key);
    if (k == std::string::npos || k >= j.size() || j[k] != '"') { if (found) *found = false; return {}; }
    ++k; std::string out;
    for (; k < j.size(); ++k) {
        if (j[k] == '\\' && k + 1 < j.size()) { out += j[k + 1]; ++k; continue; }
        if (j[k] == '"') break;
        out += j[k];
    }
    if (found) *found = true;
    return out;
}

// Целое: "ключ":<число>. unknown, если ключа нет.
inline long long num(const std::string& j, const std::string& key, long long unknown = -1, size_t from = 0) {
    size_t k = valuePos(j, key, from);
    if (k == std::string::npos) return unknown;
    return std::atoll(j.c_str() + k);
}

// Дробное: "ключ":<число>. found=false, если ключа нет.
inline double real(const std::string& j, const std::string& key, bool* found = nullptr) {
    size_t k = valuePos(j, key);
    if (k == std::string::npos) { if (found) *found = false; return 0.0; }
    if (found) *found = true;
    return std::atof(j.c_str() + k);
}

// Булево: "ключ":true|false. Возвращает def, если ключа нет.
inline bool boolean(const std::string& j, const std::string& key, bool def = false) {
    size_t k = valuePos(j, key);
    if (k == std::string::npos) return def;
    return j.compare(k, 4, "true") == 0;
}

// Число внутри под-объекта: сначала находим "obj", потом "key" после него.
inline long long numIn(const std::string& j, const std::string& obj, const std::string& key, long long unknown = -1) {
    size_t o = j.find("\"" + obj + "\"");
    if (o == std::string::npos) return unknown;
    return num(j, key, unknown, o);
}

} // namespace jsonr
