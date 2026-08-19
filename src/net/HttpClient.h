#pragma once
// Синхронный HTTP/1.1-клиент для камер Blackmagic (их control-API — обычный HTTP
// на LAN, без TLS). Умеет GET/PUT/POST, отвечает кодом и телом.
//
// Почему не plat::httpGet (§14): тот только GET и заточен под HTTPS к GitHub
// (WinHTTP / NSURLSession). Здесь нужен PUT/POST по чистому HTTP, и он делит
// сокетный шим с сервером (net/Socket.h), а не живёт в платформенном слое —
// сокеты BSD одинаковы везде.
//
// 🔴 Ответ читается по Content-Length, а НЕ «до закрытия сокета». Замерено на
// живой PYXIS: камера присылает ответ сразу, но соединение НЕ закрывает, хотя мы
// послали "Connection: close". Чтение до EOF упиралось в таймаут на КАЖДОМ
// запросе — 2.5 с вместо миллисекунд, полный опрос камеры занимал 35 с.
// Если Content-Length нет (или Transfer-Encoding: chunked) — откатываемся на
// чтение до закрытия, чтобы не зависнуть навсегда: таймаут ограничит сверху.

#include "Socket.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

namespace net {

struct Resp {
    int         status = 0;      // 0 = не дошли до ответа (сетевая ошибка)
    std::string body;
    std::string err;             // текст ошибки, если status == 0
    bool ok()      const { return status >= 200 && status < 300; }
    bool okEmpty() const { return status == 204; }
};

namespace detail {

// connect() с таймаутом: без него недоступный хост вешает поток надолго.
inline bool connectTimeout(coll::Socket s, const sockaddr* addr, socklen_t len, int ms) {
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    int rc = ::connect(s, addr, len);
    bool ok = (rc == 0);
    if (!ok) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        if (::select(static_cast<int>(s) + 1, nullptr, &wf, nullptr, &tv) > 0) {
            int soerr = 0; socklen_t sl = sizeof(soerr);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl);
            ok = (soerr == 0);
        }
    }
#ifdef _WIN32
    u_long bl = 0; ioctlsocket(s, FIONBIO, &bl);
#else
    fcntl(s, F_SETFL, fl);
#endif
    return ok;
}

} // namespace detail

// host — IP или имя (в т.ч. "*.local", резолвится системным getaddrinfo/Bonjour).
inline Resp request(const std::string& host, int port,
                    const std::string& method, const std::string& path,
                    const std::string& body = std::string(),
                    const std::string& contentType = "application/json",
                    int timeoutMs = 4000) {
    Resp r;
    char portStr[8]; std::snprintf(portStr, sizeof(portStr), "%d", port);

    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        r.err = "getaddrinfo failed for " + host; return r;
    }
    coll::Socket s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == coll::kInvalidSocket) { ::freeaddrinfo(res); r.err = "socket() failed"; return r; }
    coll::setNoSigPipe(s);

    if (!detail::connectTimeout(s, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen), timeoutMs)) {
        ::freeaddrinfo(res); coll::closeSocket(s);
        r.err = "connect timeout/refused " + host + ":" + portStr; return r;
    }
    ::freeaddrinfo(res);
    coll::setRecvTimeout(s, timeoutMs);

    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Connection: close\r\n";
    req += "Accept: application/json\r\n";
    if (!body.empty() || method == "PUT" || method == "POST") {
        req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    for (size_t sent = 0; sent < req.size(); ) {
        int n = coll::sendAll(s, req.data() + sent, req.size() - sent);
        if (n <= 0) { coll::closeSocket(s); r.err = "send failed"; return r; }
        sent += static_cast<size_t>(n);
    }

    // Регистронезависимый поиск заголовка: имена заголовков регистра не имеют.
    auto findHeader = [](const std::string& h, const char* name) -> std::string {
        std::string low; low.reserve(h.size());
        for (char c : h) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string key = name;                       // уже в нижнем регистре
        size_t k = low.find("\r\n" + key + ":");
        if (k == std::string::npos) return {};
        k += 2 + key.size() + 1;
        size_t e = low.find("\r\n", k);
        std::string v = h.substr(k, e == std::string::npos ? std::string::npos : e - k);
        size_t b = v.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : v.substr(b);
    };

    std::string raw; char buf[4096];
    size_t hdrEnd = std::string::npos;
    long long want = -1;            // сколько байт тела ждём; -1 = неизвестно

    for (;;) {
        if (hdrEnd == std::string::npos) {
            hdrEnd = raw.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                const std::string head = raw.substr(0, hdrEnd + 2);
                size_t sp = head.find(' ');
                if (sp != std::string::npos) r.status = std::atoi(head.c_str() + sp + 1);
                const std::string cl = findHeader(head, "content-length");
                if (!cl.empty())                       want = std::atoll(cl.c_str());
                else if (r.status == 204 || r.status == 304) want = 0;
            }
        }
        if (hdrEnd != std::string::npos && want >= 0 &&
            raw.size() >= hdrEnd + 4 + static_cast<size_t>(want))
            break;                                     // тело получено целиком

        int n = coll::recvSome(s, buf, sizeof(buf));
        if (n > 0) { raw.append(buf, static_cast<size_t>(n)); continue; }
        break;                                         // 0 = закрытие, <0 = таймаут
    }
    coll::closeSocket(s);

    if (raw.empty()) { r.err = "empty response"; return r; }
    if (r.status == 0) {
        size_t sp = raw.find(' ');
        if (sp != std::string::npos) r.status = std::atoi(raw.c_str() + sp + 1);
    }
    if (hdrEnd == std::string::npos) hdrEnd = raw.find("\r\n\r\n");
    r.body = (hdrEnd == std::string::npos) ? std::string() : raw.substr(hdrEnd + 4);
    if (want >= 0 && r.body.size() > static_cast<size_t>(want)) r.body.resize(static_cast<size_t>(want));
    return r;
}

inline Resp get (const std::string& h,int p,const std::string& path,int t=4000){ return request(h,p,"GET",path,"","application/json",t); }
inline Resp put (const std::string& h,int p,const std::string& path,const std::string& b,int t=4000){ return request(h,p,"PUT",path,b,"application/json",t); }
inline Resp post(const std::string& h,int p,const std::string& path,const std::string& b=std::string(),int t=4000){ return request(h,p,"POST",path,b,"application/json",t); }

} // namespace net
