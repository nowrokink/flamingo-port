#include "server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "cache/compile.h"

#pragma comment(lib, "ws2_32.lib")

namespace fp::cache {

struct Server::State {
    Queue* queue = nullptr;
    std::string token, opt;
    int timeout_s = 0;
    Finish finish;
    SOCKET listener = INVALID_SOCKET;
    std::atomic<bool> stopping{false};
    std::atomic<int> connections{0};
    std::thread accept_thread;
};

namespace {

constexpr const char* kProtocol = "1";
constexpr size_t kMaxBody = 64u << 20;

bool in_net(uint32_t ip, const char* net, int bits) {
    in_addr a{};
    inet_pton(AF_INET, net, &a);
    const uint32_t n = ntohl(a.s_addr), mask = bits ? ~0u << (32 - bits) : 0;
    return (ip & mask) == (n & mask);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// parse_qs(query).get(key, [""])[0]: blank values count as absent
std::string query_value(const std::string& query, const std::string& key) {
    size_t at = 0;
    for (;;) {
        const size_t amp = query.find('&', at);
        const std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && url_decode(pair.substr(0, eq)) == key && eq + 1 < pair.size())
            return url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) return "";
        at = amp + 1;
    }
}

bool send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        const int put = send(s, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (put <= 0) return false;
        p += put;
        n -= static_cast<size_t>(put);
    }
    return true;
}

const char* reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 410: return "Gone";
        case 426: return "Upgrade Required";
        default: return "Not Implemented";
    }
}

void answer(SOCKET s, int code, const std::string& body = "",
            const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::string head = "HTTP/1.0 " + std::to_string(code) + " " + reason(code) + "\r\n";
    for (const auto& [k, v] : headers) head += k + ": " + v + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    if (send_all(s, head.data(), head.size()) && !body.empty()) send_all(s, body.data(), body.size());
}

struct Request {
    std::string method, path, query;
    std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
    std::string body;
    std::string header(const char* name) const {
        for (const auto& [k, v] : headers)
            if (k == name) return v;
        return "";
    }
};

bool read_request(SOCKET s, Request& r) {
    std::string buf;
    std::vector<char> tmp(65536);
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > (1u << 20)) return false;
        const int got = recv(s, tmp.data(), static_cast<int>(tmp.size()), 0);
        if (got <= 0) return false;
        buf.append(tmp.data(), static_cast<size_t>(got));
    }
    const std::string head = buf.substr(0, end);
    r.body = buf.substr(end + 4);
    const size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol);
    const size_t a = line.find(' ');
    if (a == std::string::npos) return false;
    const size_t b = line.find(' ', a + 1);
    r.method = line.substr(0, a);
    const std::string target = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = q == std::string::npos ? "" : target.substr(q + 1);
    for (size_t at = eol == std::string::npos ? head.size() : eol + 2; at < head.size();) {
        const size_t e = head.find("\r\n", at);
        const std::string h = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
        const size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string v = h.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            r.headers.emplace_back(lower(h.substr(0, colon)), v);
        }
        if (e == std::string::npos) break;
        at = e + 2;
    }
    // the body only when 0 < Content-Length <= 64 MiB, as the Python read it
    const std::string cl = r.header("content-length");
    const size_t n = cl.empty() ? 0 : static_cast<size_t>(std::strtoull(cl.c_str(), nullptr, 10));
    if (n == 0 || n > kMaxBody) {
        r.body.clear();
        return true;
    }
    while (r.body.size() < n) {
        const int got = recv(s, tmp.data(), static_cast<int>(tmp.size()), 0);
        if (got <= 0) return false;
        r.body.append(tmp.data(), static_cast<size_t>(got));
    }
    r.body.resize(n);
    return true;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

void serve_one(Server::State& st, SOCKET s, const std::string& ip) {
    DWORD tv = 120000;  // a helper that stops talking mid-request doesn't hold a thread for good
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
    Request r;
    if (!read_request(s, r)) return;
    if (!private_address(ip) || r.header("x-token") != st.token) return answer(s, 403);
    if (r.header("x-protocol") != kProtocol) return answer(s, 426);
    if (r.method == "GET") {
        if (r.path != "/job") return answer(s, 404);
        if (st.queue->over()) return answer(s, 410);
        std::string w = query_value(r.query, "w");
        if (w.empty()) w = "?";
        const std::string helper = w.substr(0, 40) + "@" + ip;
        JobPtr job = st.queue->take(20, helper);
        if (!job) return answer(s, st.queue->over() ? 410 : 204);
        return answer(s, 200, job->hlsl,
                      {{"X-Job-Id", std::to_string(job->id)}, {"X-Profile", profile(job->stage)}, {"X-Opt", st.opt},
                       {"X-Timeout", std::to_string(st.timeout_s)}});
    }
    if (r.method == "POST") {
        const std::string id = query_value(r.query, "id");
        if (r.path != "/result" || !all_digits(id)) return answer(s, 400);
        if (JobPtr job = st.queue->give_back(std::stoull(id))) {
            if (query_value(r.query, "ok") == "1" && r.body.compare(0, 4, "DXBC") == 0)
                st.finish(job, std::move(r.body));
            else
                st.queue->put_local(job);  // a helper's failure isn't kept as such: compiled here again
        }
        return answer(s, 200);
    }
    answer(s, 501);
}

}  // namespace

bool private_address(const std::string& addr) {
    in_addr a{};
    if (inet_pton(AF_INET, addr.c_str(), &a) != 1) return false;
    const uint32_t ip = ntohl(a.s_addr);
    // ipaddress.IPv4Address: is_private (these networks) or is_loopback (127/8)
    const std::pair<const char*, int> nets[] = {
        {"0.0.0.0", 8},       {"10.0.0.0", 8},      {"127.0.0.0", 8},   {"169.254.0.0", 16},
        {"172.16.0.0", 12},   {"192.0.0.0", 29},    {"192.0.0.170", 31}, {"192.0.2.0", 24},
        {"192.168.0.0", 16},  {"198.18.0.0", 15},   {"198.51.100.0", 24}, {"203.0.113.0", 24},
        {"240.0.0.0", 4},     {"255.255.255.255", 32}};
    for (const auto& [net, bits] : nets)
        if (in_net(ip, net, bits)) return true;
    return false;
}

bool Server::start(Queue& queue, int port, const std::string& token, const std::string& opt, int timeout_s,
                   Finish finish) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE;
    setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons(static_cast<u_short>(port));
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(l, reinterpret_cast<sockaddr*>(&at), sizeof at) || listen(l, SOMAXCONN)) {
        closesocket(l);
        return false;
    }
    st_ = std::make_shared<State>();
    st_->queue = &queue;
    st_->token = token;
    st_->opt = opt;
    st_->timeout_s = timeout_s;
    st_->finish = std::move(finish);
    st_->listener = l;
    auto st = st_;
    st_->accept_thread = std::thread([st] {
        for (;;) {
            sockaddr_in from{};
            int len = sizeof from;
            SOCKET s = accept(st->listener, reinterpret_cast<sockaddr*>(&from), &len);
            if (s == INVALID_SOCKET) {
                if (st->stopping) return;
                continue;
            }
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            ++st->connections;
            std::thread([st, s, addr = std::string(ip)] {
                serve_one(*st, s, addr);
                shutdown(s, SD_SEND);
                closesocket(s);
                --st->connections;
            }).detach();
        }
    });
    return true;
}

void Server::stop() {
    if (!st_) return;
    st_->stopping = true;
    closesocket(st_->listener);
    if (st_->accept_thread.joinable()) st_->accept_thread.join();
    // requests still being answered end on their own threads (they hold the state); a helper waiting on a job hears
    // "over" within the queue's wait
    for (int i = 0; i < 250 && st_->connections > 0; ++i) Sleep(100);
    st_.reset();
}

Server::~Server() { stop(); }

}  // namespace fp::cache
