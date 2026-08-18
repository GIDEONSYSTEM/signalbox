#pragma once
// Синхронный HTTP/1.1-клиент для камер Blackmagic (их control-API — обычный HTTP
// на LAN, без TLS). Умеет GET/PUT/POST, отвечает кодом и телом.
//
// Почему не plat::httpGet (§14): тот только GET и заточен под HTTPS к GitHub
// (WinHTTP / NSURLSession). Здесь нужен PUT/POST по чистому HTTP, и он делит
// сокетный шим с сервером (net/Socket.h), а не живёт в платформенном слое —
// сокеты BSD одинаковы везде.
//
// Осознанное упрощение: запрос уходит с "Connection: close", ответ читается до
// закрытия сокета. Поэтому не нужен разбор chunked — камера Blackmagic отдаёт
// Content-Length и закрывает соединение. Для сервера общего назначения так
// нельзя, для одного известного API — ровно то, что надо.

#include "Socket.h"

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

    std::string raw; char buf[4096];
    for (;;) {
        int n = coll::recvSome(s, buf, sizeof(buf));
        if (n > 0)      raw.append(buf, static_cast<size_t>(n));
        else            break;   // 0 = закрытие, <0 = таймаут/ошибка
    }
    coll::closeSocket(s);

    if (raw.empty()) { r.err = "empty response"; return r; }
    // Статус-строка: "HTTP/1.1 200 OK"
    size_t sp = raw.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(raw.c_str() + sp + 1);
    size_t hdrEnd = raw.find("\r\n\r\n");
    r.body = (hdrEnd == std::string::npos) ? std::string() : raw.substr(hdrEnd + 4);
    return r;
}

inline Resp get (const std::string& h,int p,const std::string& path,int t=4000){ return request(h,p,"GET",path,"","application/json",t); }
inline Resp put (const std::string& h,int p,const std::string& path,const std::string& b,int t=4000){ return request(h,p,"PUT",path,b,"application/json",t); }
inline Resp post(const std::string& h,int p,const std::string& path,const std::string& b=std::string(),int t=4000){ return request(h,p,"POST",path,b,"application/json",t); }

} // namespace net
