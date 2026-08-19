#pragma once
// Минимальный клиент WebSocket (RFC 6455) для подписки на события камеры.
//
// Зачем: камера сама шлёт propertyValueChanged, и push приходит быстрее, чем
// успевает завершиться обычный REST-запрос. Это снимает опрос раз в секунду —
// у Sony он неизбежен (так устроен SDK), а здесь не нужен.
//
// Тонкости, без которых не работает:
// * кадры клиент->сервер ОБЯЗАНЫ быть маскированы (сервер рвёт соединение);
// * ответ на рукопожатие — 101, и вместе с ним в том же чтении может прийти
//   начало первого кадра, поэтому остаток буфера нельзя выбрасывать;
// * ping от сервера надо отражать pong'ом, иначе он нас отключит.
//
// TLS не нужен: control-API камер Blackmagic в локальной сети — чистый HTTP.

#include "Socket.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ws {

class Client {
public:
    ~Client() { close(); }

    bool connected() const { return m_sock != coll::kInvalidSocket; }

    // Рукопожатие. path — например "/control/api/v1/event/websocket".
    bool open(const std::string& host, int port, const std::string& path, int timeoutMs = 4000) {
        close();
        char portStr[8]; std::snprintf(portStr, sizeof(portStr), "%d", port);
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) return false;
        coll::Socket s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == coll::kInvalidSocket) { ::freeaddrinfo(res); return false; }
        coll::setNoSigPipe(s);
        const bool ok = (::connect(s, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) == 0);
        ::freeaddrinfo(res);
        if (!ok) { coll::closeSocket(s); return false; }
        coll::setRecvTimeout(s, timeoutMs);

        std::string req = "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: c2lnbmFsYm94LTEyMzQ1Ng==\r\n";   // значение произвольно
        req += "Sec-WebSocket-Version: 13\r\n\r\n";
        for (size_t sent = 0; sent < req.size(); ) {
            int n = coll::sendAll(s, req.data() + sent, req.size() - sent);
            if (n <= 0) { coll::closeSocket(s); return false; }
            sent += static_cast<size_t>(n);
        }

        std::string raw; char buf[2048];
        while (raw.find("\r\n\r\n") == std::string::npos) {
            int n = coll::recvSome(s, buf, sizeof(buf));
            if (n <= 0) { coll::closeSocket(s); return false; }
            raw.append(buf, static_cast<size_t>(n));
        }
        if (raw.compare(0, 12, "HTTP/1.1 101") != 0) { coll::closeSocket(s); return false; }
        // Хвост после заголовков — уже данные кадров, сохраняем.
        m_buf.assign(raw.begin() + static_cast<long>(raw.find("\r\n\r\n")) + 4, raw.end());
        m_sock = s;
        return true;
    }

    void close() {
        if (m_sock != coll::kInvalidSocket) { coll::closeSocket(m_sock); m_sock = coll::kInvalidSocket; }
        m_buf.clear();
    }

    bool sendText(const std::string& payload) { return sendFrame(0x1, payload); }

    // Ждёт текстовый кадр до timeoutMs. true — кадр получен в out.
    // false — таймаут (соединение живо) либо обрыв: смотреть connected().
    bool recvText(std::string& out, int timeoutMs) {
        if (m_sock == coll::kInvalidSocket) return false;
        coll::setRecvTimeout(m_sock, timeoutMs);
        for (;;) {
            std::string payload; int opcode = 0;
            if (takeFrame(payload, opcode)) {
                if (opcode == 0x1) { out.swap(payload); return true; }
                if (opcode == 0x9) { sendFrame(0xA, payload); continue; }   // ping -> pong
                if (opcode == 0x8) { close(); return false; }               // close
                continue;                                                   // pong и прочее
            }
            char buf[4096];
            int n = coll::recvSome(m_sock, buf, sizeof(buf));
            if (n > 0) { m_buf.append(buf, static_cast<size_t>(n)); continue; }
            if (n == 0) { close(); return false; }                          // сервер закрыл
            return false;                                                   // таймаут
        }
    }

private:
    bool sendFrame(uint8_t opcode, const std::string& payload) {
        if (m_sock == coll::kInvalidSocket) return false;
        std::string f;
        f += static_cast<char>(0x80 | opcode);
        const size_t n = payload.size();
        uint8_t mask[4] = {0x21, 0x5A, 0x7E, 0x0C};      // маска обязательна
        if (n < 126)        f += static_cast<char>(0x80 | n);
        else if (n < 65536) { f += static_cast<char>(0x80 | 126);
                              f += static_cast<char>((n >> 8) & 0xFF); f += static_cast<char>(n & 0xFF); }
        else                { f += static_cast<char>(0x80 | 127);
                              for (int i = 7; i >= 0; --i) f += static_cast<char>((n >> (i * 8)) & 0xFF); }
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) f += static_cast<char>(payload[i] ^ mask[i % 4]);
        for (size_t sent = 0; sent < f.size(); ) {
            int r = coll::sendAll(m_sock, f.data() + sent, f.size() - sent);
            if (r <= 0) { close(); return false; }
            sent += static_cast<size_t>(r);
        }
        return true;
    }

    // Вынуть один готовый кадр из буфера. Фрагментацию не собираем: камера шлёт
    // события целыми кадрами.
    bool takeFrame(std::string& payload, int& opcode) {
        if (m_buf.size() < 2) return false;
        const uint8_t b0 = static_cast<uint8_t>(m_buf[0]);
        const uint8_t b1 = static_cast<uint8_t>(m_buf[1]);
        opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;            // от сервера маски нет
        uint64_t len = b1 & 0x7F;
        size_t off = 2;
        if (len == 126) {
            if (m_buf.size() < 4) return false;
            len = (static_cast<uint8_t>(m_buf[2]) << 8) | static_cast<uint8_t>(m_buf[3]);
            off = 4;
        } else if (len == 127) {
            if (m_buf.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(m_buf[2 + i]);
            off = 10;
        }
        if (masked) off += 4;
        if (m_buf.size() < off + len) return false;
        payload.assign(m_buf, off, static_cast<size_t>(len));
        if (masked) {
            const char* mk = m_buf.data() + off - 4;
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mk[i % 4]);
        }
        m_buf.erase(0, off + static_cast<size_t>(len));
        return true;
    }

    coll::Socket m_sock = coll::kInvalidSocket;
    std::string  m_buf;
};

} // namespace ws
