#pragma once
// Tiny socket portability layer (Winsock on Windows, POSIX elsewhere).
#include <string>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  #define MINIDB_BAD_SOCKET INVALID_SOCKET
  using sock_len = int;
  inline int closeSocket(socket_t s) { return closesocket(s); }
  inline void netInit() { static bool done = false; if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; } }
  constexpr int SEND_FLAGS = 0;
#else
  #include <arpa/inet.h>
  #include <csignal>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  #define MINIDB_BAD_SOCKET (-1)
  using sock_len = socklen_t;
  inline int closeSocket(socket_t s) { return close(s); }
  inline void netInit() { signal(SIGPIPE, SIG_IGN); }
  constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif

inline bool sendAll(socket_t s, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        int n = static_cast<int>(::send(s, d.data() + off, static_cast<int>(d.size() - off), SEND_FLAGS));
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

inline socket_t listenOn(int port) {
    netInit();
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == MINIDB_BAD_SOCKET) return fd;
#ifndef _WIN32
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 64) != 0) {
        closeSocket(fd);
        return MINIDB_BAD_SOCKET;
    }
    return fd;
}
