#include "tcp_server.h"
#include "platform.h"
#include <iostream>
#include <thread>

void TCPServer::start() {
    socket_t fd = listenOn(port_);
    if (fd == MINIDB_BAD_SOCKET) { std::cerr << "[tcp] cannot listen on port " << port_ << "\n"; return; }
    std::cout << "[tcp] listening on port " << port_ << " (try: nc localhost " << port_ << ")\n";
    while (true) {
        sockaddr_in ca{};
        sock_len len = sizeof ca;
        socket_t c = ::accept(fd, reinterpret_cast<sockaddr*>(&ca), &len);
        if (c == MINIDB_BAD_SOCKET) continue;
        if (clients_ >= 64) { sendAll(c, "ERR too many connections\n"); closeSocket(c); continue; }
        ++clients_;
        std::thread([this, c] { handleClient(static_cast<int>(c)); }).detach();
    }
}

void TCPServer::handleClient(int fdInt) {
    socket_t c = static_cast<socket_t>(fdInt);
    sendAll(c, "MiniDB ready. Type HELP.\n");
    std::string buf;
    char tmp[1024];
    bool open = true;
    while (open) {
        int n = static_cast<int>(::recv(c, tmp, sizeof tmp, 0));
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > 8192) break;                       // refuse absurdly long lines
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line == "QUIT" || line == "quit") { sendAll(c, "Bye\n"); open = false; break; }
            if (!sendAll(c, db_.execute(line).reply + "\n")) { open = false; break; }
        }
    }
    closeSocket(c);
    --clients_;
}
