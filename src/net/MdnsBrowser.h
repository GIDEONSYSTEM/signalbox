#pragma once
// Автопоиск камер Blackmagic по mDNS/Bonjour.
//
// Камеры анонсируют себя как _http._tcp.local, а в TXT-записи анонса уже лежит
// всё нужное: capabilities=cameraControl, модель, прошивка и СТАБИЛЬНЫЙ unique
// id. По одному лишь типу сервиса фильтровать нельзя — на _http._tcp в студии
// висят ещё свитчи, ATEM и сетевые камеры наблюдения; отбираем по capabilities.
//
// Это мультикаст UDP на 224.0.0.251:5353 — та же природа, что SSDP, которым
// SignalBox ищет камеры Sony, поэтому чужая библиотека не нужна.
//
// В запросе выставлен бит QU (unicast response): ответ приходит на наш временный
// порт напрямую, и не нужно занимать 5353 и вступать в группу — этот порт может
// быть занят системным mDNSResponder (на macOS он занят всегда).

#include "Socket.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mdns {

struct Service {
    std::string instance;                  // "PYXIS 6K 3"
    std::string host;                      // "PYXIS-6K-3.local"
    std::string ip;                        // "192.168.1.118" (если пришла A-запись)
    int         port = 0;
    std::map<std::string, std::string> txt;
};

namespace detail {

// Имя DNS с учётом сжатия (указатели 0xC0). Возвращает позицию ПОСЛЕ имени в
// потоке; для имён, уехавших по указателю, — позицию после указателя.
inline size_t readName(const uint8_t* p, size_t len, size_t pos, std::string& out) {
    bool jumped = false;
    size_t after = pos;
    int guard = 0;
    while (pos < len && ++guard < 128) {
        uint8_t l = p[pos];
        if (l == 0) { if (!jumped) after = pos + 1; break; }
        if ((l & 0xC0) == 0xC0) {                       // указатель
            if (pos + 1 >= len) break;
            if (!jumped) after = pos + 2;
            pos = ((l & 0x3F) << 8) | p[pos + 1];
            jumped = true;
            continue;
        }
        if (pos + 1 + l > len) break;
        if (!out.empty()) out += ".";
        out.append(reinterpret_cast<const char*>(p + pos + 1), l);
        pos += 1 + l;
        if (!jumped) after = pos;
    }
    return after;
}

inline void putName(std::string& q, const std::string& name) {
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        q += static_cast<char>(dot - start);
        q.append(name, start, dot - start);
        if (dot == name.size()) break;
        start = dot + 1;
    }
    q += '\0';
}

} // namespace detail

// Опрашивает сеть waitMs миллисекунд и возвращает найденные сервисы типа
// serviceType (например "_http._tcp.local").
inline std::vector<Service> browse(const std::string& serviceType, int waitMs = 1200) {
    std::vector<Service> out;

    coll::Socket s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == coll::kInvalidSocket) return out;
    coll::setNoSigPipe(s);

    int ttl = 255;                                   // mDNS требует TTL 255
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    sockaddr_in any{}; any.sin_family = AF_INET; any.sin_addr.s_addr = htonl(INADDR_ANY); any.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    coll::setRecvTimeout(s, 250);

    // Запрос: PTR на тип сервиса, класс IN с битом QU (0x8000).
    std::string q;
    const uint8_t hdr[12] = {0,0, 0,0, 0,1, 0,0, 0,0, 0,0};
    q.append(reinterpret_cast<const char*>(hdr), 12);
    detail::putName(q, serviceType);
    q += '\0'; q += '\x0C';                          // QTYPE = PTR
    q += '\x80'; q += '\x01';                        // QCLASS = IN | QU

    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(5353);
    ::inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    ::sendto(s, q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    std::map<std::string, Service>     byInstance;   // instance -> сервис
    std::map<std::string, std::string> hostIp;       // host -> ip

    const auto deadline = waitMs;
    int waited = 0;
    uint8_t buf[4096];
    while (waited < deadline) {
        sockaddr_in from{}; socklen_t fl = sizeof(from);
        int n = static_cast<int>(::recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                            reinterpret_cast<sockaddr*>(&from), &fl));
        if (n <= 0) { waited += 250; continue; }
        const size_t len = static_cast<size_t>(n);
        if (len < 12) continue;

        const int qd = (buf[4] << 8) | buf[5];
        const int an = (buf[6] << 8) | buf[7];
        const int ns = (buf[8] << 8) | buf[9];
        const int ar = (buf[10] << 8) | buf[11];
        size_t pos = 12;
        for (int i = 0; i < qd && pos < len; ++i) {   // пропускаем вопросы
            std::string nm; pos = detail::readName(buf, len, pos, nm); pos += 4;
        }
        const int total = an + ns + ar;
        for (int i = 0; i < total && pos < len; ++i) {
            std::string name;
            pos = detail::readName(buf, len, pos, name);
            if (pos + 10 > len) break;
            const int type = (buf[pos] << 8) | buf[pos + 1];
            const int rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
            pos += 10;
            if (pos + static_cast<size_t>(rdlen) > len) break;
            const size_t rd = pos;

            if (type == 12) {                          // PTR: тип -> экземпляр
                std::string inst; detail::readName(buf, len, rd, inst);
                if (name == serviceType && !inst.empty()) byInstance[inst];
            } else if (type == 33) {                   // SRV: экземпляр -> хост:порт
                if (rdlen >= 6) {
                    const int port = (buf[rd + 4] << 8) | buf[rd + 5];
                    std::string target; detail::readName(buf, len, rd + 6, target);
                    Service& sv = byInstance[name];
                    sv.host = target; sv.port = port;
                }
            } else if (type == 16) {                   // TXT: пары ключ=значение
                Service& sv = byInstance[name];
                size_t k = rd;
                while (k < rd + static_cast<size_t>(rdlen) && k < len) {
                    const uint8_t sl = buf[k];
                    if (sl == 0 || k + 1 + sl > len) break;
                    const std::string kv(reinterpret_cast<const char*>(buf + k + 1), sl);
                    const size_t eq = kv.find('=');
                    if (eq != std::string::npos) sv.txt[kv.substr(0, eq)] = kv.substr(eq + 1);
                    else                          sv.txt[kv] = std::string();
                    k += 1 + sl;
                }
            } else if (type == 1 && rdlen == 4) {      // A: хост -> IPv4
                char ipb[16];
                std::snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u", buf[rd], buf[rd+1], buf[rd+2], buf[rd+3]);
                hostIp[name] = ipb;
            }
            pos += rdlen;
        }
    }
    coll::closeSocket(s);

    for (auto& kv : byInstance) {
        Service sv = kv.second;
        // Имя экземпляра — первая метка полного имени "PYXIS 6K 3._http._tcp.local".
        const size_t cut = kv.first.find("._");
        sv.instance = (cut == std::string::npos) ? kv.first : kv.first.substr(0, cut);
        auto it = hostIp.find(sv.host);
        if (it != hostIp.end()) sv.ip = it->second;
        if (!sv.host.empty() || !sv.txt.empty()) out.push_back(sv);
    }
    return out;
}

} // namespace mdns
