#pragma once
// Tiny blocking HTTP/1.1 server for localhost. Enough for serving a couple of
// static files, GET /status.json and POST /cmd. Not a general server.
//
// Сокеты BSD одинаковы везде; различий ровно на десяток строк, и они собраны
// в шим ниже — отдельная реализация в платформенном слое того не стоит.
// ⚠ Ветка POSIX на macOS ещё НЕ компилировалась.

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace coll {

// ---- сокетный шим ----
#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(Socket s)                 { ::closesocket(s); }
inline int  sendAll(Socket s, const char* p, size_t n) {
    return ::send(s, p, static_cast<int>(n), 0);
}
inline int  recvSome(Socket s, char* p, size_t n) { return ::recv(s, p, static_cast<int>(n), 0); }
inline void setRecvTimeout(Socket s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}
inline void setNoSigPipe(Socket)                  {}   // на Windows SIGPIPE нет
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
inline void closeSocket(Socket s)                 { ::close(s); }
inline int  sendAll(Socket s, const char* p, size_t n) {
    return static_cast<int>(::send(s, p, n, 0));
}
inline int  recvSome(Socket s, char* p, size_t n) { return static_cast<int>(::recv(s, p, n, 0)); }
inline void setRecvTimeout(Socket s, int ms) {
    timeval t{};
    t.tv_sec  = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
}
// Без этого запись в закрытое клиентом соединение убивает весь процесс сигналом.
inline void setNoSigPipe(Socket s) {
  #ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  #endif
}
#endif

struct HttpRequest {
    std::string method;
    std::string path;       // without query string
    std::string query;
    std::string body;
};

struct HttpResponse {
    int         status      = 200;
    std::string statusText  = "OK";
    std::string contentType = "text/plain; charset=utf-8";
    std::string extraHeaders;   // optional extra "Name: value\r\n" lines
    std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    ~HttpServer() { stop(); }

    // Bind/listen on 127.0.0.1:port. Returns false on failure.
    bool start(unsigned short port);
    // Serve until *running becomes false. Blocking.
    void run(const HttpHandler& handler, std::atomic<bool>& running);
    void stop();

private:
    void handleClient(Socket client, const HttpHandler& handler);

    Socket m_listen = kInvalidSocket;
};

// ---- implementation (header-only) ----

inline std::string httpReasonHeaders(const HttpResponse& r) {
    std::string h;
    h += "HTTP/1.1 " + std::to_string(r.status) + " " + r.statusText + "\r\n";
    h += "Content-Type: " + r.contentType + "\r\n";
    h += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    h += "Access-Control-Allow-Origin: *\r\n";
    h += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    h += "Access-Control-Allow-Headers: Content-Type\r\n";
    h += "Access-Control-Expose-Headers: X-Cam-Frames\r\n";
    h += "Cache-Control: no-store\r\n";
    h += r.extraHeaders;
    h += "Connection: close\r\n\r\n";
    return h;
}

inline bool HttpServer::start(unsigned short port) {
    m_listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen == kInvalidSocket) return false;

    int yes = 1;
    ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // all interfaces -> reachable from LAN (phone)

    if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(m_listen, SOMAXCONN) < 0) {
        closeSocket(m_listen);
        m_listen = kInvalidSocket;
        return false;
    }
    return true;
}

inline void HttpServer::stop() {
    if (m_listen != kInvalidSocket) {
        closeSocket(m_listen);
        m_listen = kInvalidSocket;
    }
}

inline void HttpServer::run(const HttpHandler& handler, std::atomic<bool>& running) {
    while (running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_listen, &rfds);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200000; // 200ms so we can re-check `running`

        // Первый аргумент важен только для POSIX; Windows его игнорирует.
        int sel = ::select(static_cast<int>(m_listen) + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        if (!FD_ISSET(m_listen, &rfds)) continue;

        Socket client = ::accept(m_listen, nullptr, nullptr);
        if (client == kInvalidSocket) continue;

        setRecvTimeout(client, 4000);
        setNoSigPipe(client);
        handleClient(client, handler);
        closeSocket(client);
    }
}

inline void HttpServer::handleClient(Socket client, const HttpHandler& handler) {
    std::string buf;
    char tmp[4096];

    // Read until we have the full header block.
    size_t headerEnd = std::string::npos;
    while (true) {
        int got = recvSome(client, tmp, sizeof(tmp));
        if (got <= 0) { if (buf.empty()) return; break; }
        buf.append(tmp, static_cast<size_t>(got));
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (buf.size() > 64 * 1024) break; // guard
    }
    if (headerEnd == std::string::npos) return;

    // Parse request line.
    HttpRequest req;
    size_t lineEnd = buf.find("\r\n");
    std::string requestLine = buf.substr(0, lineEnd);
    {
        size_t s1 = requestLine.find(' ');
        size_t s2 = (s1 == std::string::npos) ? std::string::npos
                                              : requestLine.find(' ', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos) return;
        req.method = requestLine.substr(0, s1);
        std::string target = requestLine.substr(s1 + 1, s2 - s1 - 1);
        size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = target;
        } else {
            req.path  = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
    }

    // Content-Length (case-insensitive search).
    size_t contentLength = 0;
    {
        std::string headers = buf.substr(0, headerEnd);
        std::string lower = headers;
        for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        size_t pos = lower.find("content-length:");
        if (pos != std::string::npos) {
            pos += std::string("content-length:").size();
            contentLength = static_cast<size_t>(std::strtoul(headers.c_str() + pos, nullptr, 10));
        }
    }

    // Body.
    size_t bodyStart = headerEnd + 4;
    std::string body = buf.substr(bodyStart);
    while (body.size() < contentLength) {
        int got = recvSome(client, tmp, sizeof(tmp));
        if (got <= 0) break;
        body.append(tmp, static_cast<size_t>(got));
    }
    if (body.size() > contentLength) body.resize(contentLength);
    req.body = body;

    HttpResponse res;
    if (req.method == "OPTIONS") {
        res.status = 204;
        res.statusText = "No Content";
        res.body.clear();
    } else {
        res = handler(req);
    }

    std::string out = httpReasonHeaders(res) + res.body;
    size_t sent = 0;
    while (sent < out.size()) {
        int n = sendAll(client, out.data() + sent, out.size() - sent);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

} // namespace coll
