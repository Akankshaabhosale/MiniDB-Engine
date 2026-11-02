#include "http_server.h"
#include "event_bus.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; }
std::string headerValue(const std::string& head, const std::string& headLower, const std::string& name) {
    size_t p = headLower.find("\n" + name + ":");
    if (p == std::string::npos) return "";
    size_t s = p + name.size() + 2, e = head.find('\r', s);
    std::string v = head.substr(s, e == std::string::npos ? std::string::npos : e - s);
    size_t a = v.find_first_not_of(' ');
    return a == std::string::npos ? "" : v.substr(a);
}
const char* statusText(int s) {
    switch (s) { case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request"; case 404: return "Not Found";
                 case 413: return "Payload Too Large"; case 429: return "Too Many Requests"; case 503: return "Service Unavailable"; default: return "OK"; }
}
}  // namespace

void HttpServer::reply(socket_t c, int status, const std::string& type, const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    sendAll(c, r);
}

bool HttpServer::allow(const std::string& ip) {
    std::lock_guard<std::mutex> lk(rlMtx_);
    long long now = nowMs();
    if (hits_.size() > 2000) hits_.clear();
    auto& q = hits_[ip];
    while (!q.empty() && now - q.front() > 10000) q.pop_front();
    if (q.size() >= 200) return false;          // 200 requests / 10 s per client
    q.push_back(now);
    return true;
}

void HttpServer::start() {
    socket_t fd = listenOn(port_);
    if (fd == MINIDB_BAD_SOCKET) { std::cerr << "[http] cannot listen on port " << port_ << "\n"; return; }
    std::cout << "[http] open http://localhost:" << port_ << " in your browser\n";
    while (true) {
        sockaddr_in ca{};
        sock_len len = sizeof ca;
        socket_t c = ::accept(fd, reinterpret_cast<sockaddr*>(&ca), &len);
        if (c == MINIDB_BAD_SOCKET) continue;
        std::string ip = inet_ntoa(ca.sin_addr);
        std::thread([this, c, ip] { handle(c, ip); }).detach();
    }
}

void HttpServer::serveEvents(socket_t c) {
    if (sse_ >= 40) { reply(c, 503, "text/plain", "too many viewers"); closeSocket(c); return; }
    ++sse_;
    sendAll(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n"
               "X-Accel-Buffering: no\r\nAccess-Control-Allow-Origin: *\r\n\r\n: connected\n\n");
    EventBus& bus = EventBus::instance();
    uint64_t last = bus.lastId();
    while (true) {
        auto evs = bus.wait(last, 15000);
        std::string out;
        if (evs.empty()) out = ": ping\n\n";
        else for (auto& e : evs) out += "data: " + e + "\n\n";
        if (!sendAll(c, out)) break;
    }
    --sse_;
    closeSocket(c);
}

void HttpServer::handle(socket_t c, std::string ip) {
    std::string req;
    char buf[4096];
    size_t he;
    while ((he = req.find("\r\n\r\n")) == std::string::npos) {
        int n = static_cast<int>(::recv(c, buf, sizeof buf, 0));
        if (n <= 0 || req.size() > 16384) { closeSocket(c); return; }
        req.append(buf, static_cast<size_t>(n));
    }
    std::string head = req.substr(0, he), headLower = lower(head), body = req.substr(he + 4);
    std::istringstream first(head);
    std::string method, path, ver;
    first >> method >> path >> ver;
    path = path.substr(0, path.find('?'));

    std::string fwd = headerValue(head, headLower, "x-forwarded-for");   // real client IP behind a proxy
    if (!fwd.empty()) ip = fwd.substr(0, fwd.find(','));

    size_t clen = 0;
    try { std::string cl = headerValue(head, headLower, "content-length"); if (!cl.empty()) clen = std::stoul(cl); } catch (...) {}
    if (clen > 4096) { reply(c, 413, "text/plain", "body too large"); closeSocket(c); return; }
    while (body.size() < clen) {
        int n = static_cast<int>(::recv(c, buf, sizeof buf, 0));
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }

    if (method == "OPTIONS") { reply(c, 204, "text/plain", ""); closeSocket(c); return; }
    if (path == "/api/events") { serveEvents(c); return; }

    if (path == "/" || path == "/index.html") {
        std::ifstream in(webDir_ + "/index.html", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        if (!in) reply(c, 404, "text/plain", "web/index.html not found (set MINIDB_WEB_DIR)");
        else reply(c, 200, "text/html; charset=utf-8", ss.str());
    } else if (path.rfind("/api/", 0) == 0 && !allow(ip)) {
        reply(c, 429, "application/json", "{\"ok\":false,\"reply\":\"ERR rate limit: slow down a little\"}");
    } else if (path == "/api/state") {
        reply(c, 200, "application/json", db_.stateJson());
    } else if (path == "/api/wal") {
        reply(c, 200, "application/json", db_.walJson());
    } else if (path == "/api/cmd" && method == "POST") {
        auto r = db_.execute(body);
        reply(c, 200, "application/json", std::string("{\"ok\":") + (r.ok ? "true" : "false") + "," + jstr("reply", r.reply) + "}");
    } else if (path == "/api/crash" && method == "POST") {
        long long now = nowMs();
        if (now - lastCrash_ < 2000) {
            reply(c, 429, "application/json", "{\"ok\":false,\"reply\":\"ERR give recovery a moment\"}");
        } else {
            lastCrash_ = now;
            db_.crashAndRecover();
            reply(c, 200, "application/json", "{\"ok\":true,\"reply\":\"Engine crashed and recovered from disk\"}");
        }
    } else {
        reply(c, 404, "text/plain", "not found");
    }
    closeSocket(c);
}
