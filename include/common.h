#pragma once
// Shared helpers: time, token encoding (for WAL/snapshot files) and tiny JSON builders.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

struct Entry {
    std::string key;
    std::string value;
    long long expireAtMs = 0;   // absolute unix time in ms, 0 = never expires
};

inline long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Percent-encode whitespace/control chars so a key or value is always ONE token in a line.
// The empty string is encoded as "%%" (unambiguous because a real '%' becomes "%25").
inline std::string encodeToken(const std::string& s) {
    if (s.empty()) return "%%";
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c <= 0x20 || c == '%' || c == 0x7f) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline bool decodeToken(const std::string& t, std::string& out) {
    out.clear();
    if (t == "%%") return true;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '%') { out += t[i]; continue; }
        if (i + 2 >= t.size()) return false;
        int a = hexVal(t[i + 1]), b = hexVal(t[i + 2]);
        if (a < 0 || b < 0) return false;
        out += static_cast<char>(a * 16 + b);
        i += 2;
    }
    return true;
}

inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

inline std::string jstr(const char* k, const std::string& v) {
    return std::string("\"") + k + "\":\"" + jsonEscape(v) + "\"";
}
inline std::string jnum(const char* k, long long v) {
    return std::string("\"") + k + "\":" + std::to_string(v);
}
