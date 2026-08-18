#pragma once
// Мелочи для сборки JSON, нужные и main.cpp, и реализациям камер: каждая марка
// сама сериализует свой фрагмент /status.json, поэтому экранирование строк
// должно быть одно на всех, а не скопировано в каждый файл.

#include <cstdio>
#include <string>

namespace jsonw {

inline std::string esc(const std::string& in) {
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

// Целое или null — форма, в которой панель ждёт «значение неизвестно».
inline std::string numOrNull(long long v, long long unknown = -1) {
    return v == unknown ? std::string("null") : std::to_string(v);
}

inline const char* boolStr(bool b) { return b ? "true" : "false"; }

} // namespace jsonw
