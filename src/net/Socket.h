#pragma once
// Сокетный шим BSD/WinSock — одно место на всю программу.
// Раньше жил в начале HttpServer.h; вынесен, чтобы им пользовался и HTTP-клиент
// к камерам Blackmagic (src/net/HttpClient.h), а не только сервер. Различий
// между платформами ровно на десяток строк, поэтому в платформенный слой это не
// тащим — граница слоя проходит выше (§14).

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
  #include <netdb.h>
  #include <fcntl.h>
#endif

#include <cstddef>

namespace coll {

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

} // namespace coll
