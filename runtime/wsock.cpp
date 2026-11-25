// Virtual Winsock 1.1 / 2.0 layer for browser (WebSocket relay) and native
// runtime builds. Implements WSOCK32.dll and WS2_32.dll socket operations
// including WSAAsyncSelect window-message notifications and FIONREAD.
#include "wsock_internal.h"
#include "imports.h"
#include "memory.h"
#include "user32_internal.h"
#include "win32.h"
#include "../platform/os.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static void js_wsock_send_frame(const uint8_t *ptr, int len) {
    // clang-format off
    MAIN_THREAD_EM_ASM({
        if (window.recompNet && typeof window.recompNet.send === 'function') {
            window.recompNet.send(HEAPU8.subarray($0, $0 + $1));
        }
    }, ptr, len);
    // clang-format on
}
#else
static void js_wsock_send_frame(const uint8_t *, int) {}
#endif

namespace {

constexpr uint32_t INVALID_SOCKET_ = 0xffffffffu;
constexpr uint32_t SOCKET_ERROR_ = 0xffffffffu;

constexpr int AF_INET_ = 2;
constexpr int SOCK_STREAM_ = 1;
constexpr int SOCK_DGRAM_ = 2;

constexpr uint32_t FIONREAD_ = 0x4004667fu;
constexpr uint32_t FIONBIO_ = 0x8004667eu;

constexpr uint32_t SOL_SOCKET_ = 0xffffu;
constexpr uint32_t SO_REUSEADDR_ = 0x0004u;
constexpr uint32_t SO_BROADCAST_ = 0x0020u;
constexpr uint32_t SO_ERROR_ = 0x1007u;
constexpr uint32_t SO_TYPE_ = 0x1008u;

constexpr uint32_t FD_READ_ = 1u << 0;
constexpr uint32_t FD_WRITE_ = 1u << 1;
constexpr uint32_t FD_OOB_ = 1u << 2;
constexpr uint32_t FD_ACCEPT_ = 1u << 3;
constexpr uint32_t FD_CONNECT_ = 1u << 4;
constexpr uint32_t FD_CLOSE_ = 1u << 5;

constexpr uint32_t WSAEINTR_ = 10004;
constexpr uint32_t WSAEBADF_ = 10009;
constexpr uint32_t WSAEFAULT_ = 10014;
constexpr uint32_t WSAEINVAL_ = 10022;
constexpr uint32_t WSAEWOULDBLOCK_ = 10035;
constexpr uint32_t WSAEINPROGRESS_ = 10036;
constexpr uint32_t WSAEALREADY_ = 10037;
constexpr uint32_t WSAENOTSOCK_ = 10038;
constexpr uint32_t WSAEMSGSIZE_ = 10040;
constexpr uint32_t WSAEAFNOSUPPORT_ = 10047;
constexpr uint32_t WSAEADDRINUSE_ = 10048;
constexpr uint32_t WSAENETDOWN_ = 10050;
constexpr uint32_t WSAECONNRESET_ = 10054;
constexpr uint32_t WSAENOBUFS_ = 10055;
constexpr uint32_t WSAEISCONN_ = 10056;
constexpr uint32_t WSAENOTCONN_ = 10057;
constexpr uint32_t WSAECONNREFUSED_ = 10061;
constexpr uint32_t WSAHOST_NOT_FOUND_ = 11001;

// Wire protocol opcodes (shared with net-relay.js and web_launcher.py)
constexpr uint8_t OP_OPEN_BIND = 0x01;
constexpr uint8_t OP_LISTEN = 0x02;
constexpr uint8_t OP_CONNECT = 0x03;
constexpr uint8_t OP_SEND = 0x04;
constexpr uint8_t OP_SENDTO = 0x05;
constexpr uint8_t OP_CLOSE = 0x06;

constexpr uint8_t OP_ASSIGN_IP = 0x80;
constexpr uint8_t OP_CONNECT_RESULT = 0x81;
constexpr uint8_t OP_ACCEPT = 0x82;
constexpr uint8_t OP_RECV = 0x83;
constexpr uint8_t OP_RECVFROM = 0x84;
constexpr uint8_t OP_CLOSED = 0x85;

struct Datagram {
    uint32_t src_ip = 0;   // host byte order
    uint16_t src_port = 0; // host byte order
    std::vector<uint8_t> data;
};

struct VirtualSocket {
    uint32_t id = 0;
    int af = AF_INET_;
    int type = SOCK_STREAM_;
    int proto = 0;
    bool nonblocking = false;
    bool bound = false;
    bool listening = false;
    bool connecting = false;
    bool connected = false;
    bool closed_by_peer = false;
    bool reuseaddr = false;
    bool broadcast = false;
    uint32_t local_ip = 0;   // host byte order
    uint16_t local_port = 0; // host byte order
    uint32_t peer_ip = 0;    // host byte order
    uint16_t peer_port = 0;  // host byte order
    uint32_t last_error = 0;

    // WSAAsyncSelect state
    uint32_t hwnd = 0;
    uint32_t wmsg = 0;
    uint32_t event_mask = 0;
    uint32_t pending_events = 0;
    uint32_t connect_error = 0;

    std::deque<uint8_t> tcp_rx;
    std::deque<Datagram> udp_rx;
    std::deque<uint32_t> accept_queue;
};

std::mutex g_mu;
std::map<uint32_t, VirtualSocket> g_sockets;
uint32_t g_next_sock_id = 0x1000u;
uint16_t g_next_ephemeral_port = 49152u;
uint32_t g_virtual_ip = 0x0a000002u; // 10.0.0.2 fallback until relay assigns one
thread_local uint32_t t_wsa_error = 0;

uint16_t bswap16(uint16_t v) {
    return uint16_t((v >> 8) | (v << 8));
}
uint32_t bswap32(uint32_t v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

void put_be16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v & 0xffu);
}
void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v & 0xffu);
}
uint16_t get_be16(const uint8_t *p) {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}
uint32_t get_be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void send_wire(const std::vector<uint8_t> &frame) {
    if (!frame.empty())
        js_wsock_send_frame(frame.data(), (int)frame.size());
}

void wire_open_bind(const VirtualSocket &s) {
    std::vector<uint8_t> f(10);
    f[0] = OP_OPEN_BIND;
    put_be32(&f[1], s.id);
    f[5] = uint8_t(s.type == SOCK_DGRAM_ ? 17 : 6);
    put_be16(&f[6], s.local_port);
    f[8] = s.reuseaddr ? 1 : 0;
    f[9] = s.broadcast ? 1 : 0;
    send_wire(f);
}

void wire_listen(uint32_t sock_id, uint16_t backlog) {
    std::vector<uint8_t> f(7);
    f[0] = OP_LISTEN;
    put_be32(&f[1], sock_id);
    put_be16(&f[5], backlog);
    send_wire(f);
}

void wire_connect(uint32_t sock_id, uint32_t ip_host, uint16_t port_host) {
    std::vector<uint8_t> f(11);
    f[0] = OP_CONNECT;
    put_be32(&f[1], sock_id);
    put_be32(&f[5], ip_host);
    put_be16(&f[9], port_host);
    send_wire(f);
}

void wire_send(uint32_t sock_id, const uint8_t *data, size_t len) {
    std::vector<uint8_t> f(5 + len);
    f[0] = OP_SEND;
    put_be32(&f[1], sock_id);
    if (len)
        std::memcpy(&f[5], data, len);
    send_wire(f);
}

void wire_sendto(uint32_t sock_id, uint32_t ip_host, uint16_t port_host, const uint8_t *data,
                 size_t len) {
    std::vector<uint8_t> f(11 + len);
    f[0] = OP_SENDTO;
    put_be32(&f[1], sock_id);
    put_be32(&f[5], ip_host);
    put_be16(&f[9], port_host);
    if (len)
        std::memcpy(&f[11], data, len);
    send_wire(f);
}

void wire_close(uint32_t sock_id) {
    std::vector<uint8_t> f(5);
    f[0] = OP_CLOSE;
    put_be32(&f[1], sock_id);
    send_wire(f);
}

void write_sockaddr_in(uint32_t addr_ptr, uint32_t ip_host, uint16_t port_host) {
    if (!addr_ptr || !gm_valid(addr_ptr, 16))
        return;
    std::memset(g_mem + addr_ptr, 0, 16);
    wr16(addr_ptr + 0, AF_INET_);
    wr16(addr_ptr + 2, bswap16(port_host));
    wr32(addr_ptr + 4, bswap32(ip_host));
}

bool read_sockaddr_in(uint32_t addr_ptr, uint32_t namelen, uint16_t &family, uint32_t &ip_host,
                      uint16_t &port_host) {
    if (!addr_ptr || namelen < 16 || !gm_valid(addr_ptr, 16))
        return false;
    family = rd16(addr_ptr + 0);
    port_host = bswap16(rd16(addr_ptr + 2));
    ip_host = bswap32(rd32(addr_ptr + 4));
    return true;
}

bool parse_ipv4(const char *s, uint32_t &out_host) {
    if (!s || !*s)
        return false;
    uint32_t parts[4] = {};
    int idx = 0;
    const char *p = s;
    while (*p && idx < 4) {
        if (*p < '0' || *p > '9')
            return false;
        uint32_t val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + uint32_t(*p - '0');
            if (val > 255)
                return false;
            ++p;
        }
        parts[idx++] = val;
        if (*p == '.') {
            if (idx >= 4)
                return false;
            ++p;
        } else if (*p != '\0') {
            return false;
        }
    }
    if (idx != 4)
        return false;
    out_host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

uint32_t fail_wsa(X86 *c, uint32_t ret, uint32_t err) {
    t_wsa_error = err;
    set_eax(c, ret);
    return ret;
}

// ---------------------------------------------------------------------------
// Winsock API implementations
// ---------------------------------------------------------------------------
void w_WSAStartup(X86 *c) {
    uint32_t version = arg(c, 0), pdata = arg(c, 1);
    if (pdata && gm_valid(pdata, 400)) {
        std::memset(g_mem + pdata, 0, 400);
        wr16(pdata + 0, (uint16_t)version);
        wr16(pdata + 2, 0x0202);
        gm_put_str(pdata + 4, "pop-metal recomp sockets", 257);
        gm_put_str(pdata + 4 + 257, "Running", 129);
        wr16(pdata + 4 + 257 + 129, 64);       // iMaxSockets
        wr16(pdata + 4 + 257 + 129 + 2, 65507); // iMaxUdpDg
    }
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_WSACleanup(X86 *c) {
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_WSAGetLastError(X86 *c) {
    set_eax(c, t_wsa_error);
}

void w_WSASetLastError(X86 *c) {
    t_wsa_error = arg(c, 0);
    set_eax(c, 0);
}

void w_htonl(X86 *c) {
    set_eax(c, bswap32(arg(c, 0)));
}
void w_htons(X86 *c) {
    set_eax(c, bswap16((uint16_t)arg(c, 0)));
}

void w_inet_addr(X86 *c) {
    std::string s = gm_str(arg(c, 0), 64);
    uint32_t ip_host = 0;
    if (!parse_ipv4(s.c_str(), ip_host)) {
        set_eax(c, 0xffffffffu); // INADDR_NONE
        return;
    }
    set_eax(c, bswap32(ip_host));
}

void w_inet_ntoa(X86 *c) {
    static uint32_t buf = 0;
    if (!buf || !heap_owns(buf))
        buf = heap_alloc(32, true);
    uint32_t addr = arg(c, 0); // network byte order
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", addr & 0xffu, (addr >> 8) & 0xffu,
             (addr >> 16) & 0xffu, (addr >> 24) & 0xffu);
    gm_put_str(buf, tmp, 32);
    set_eax(c, buf);
}

void w_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (buf && len && gm_valid(buf, len))
        gm_put_str(buf, RECOMP_GAME_ID, len);
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_gethostbyname(X86 *c) {
    std::string name = gm_str(arg(c, 0), 256);
    uint32_t ip_host = 0;
    if (name.empty() || name == RECOMP_GAME_ID || name == "localhost") {
        std::lock_guard<std::mutex> lk(g_mu);
        ip_host = g_virtual_ip;
    } else if (!parse_ipv4(name.c_str(), ip_host)) {
        fail_wsa(c, 0, WSAHOST_NOT_FOUND_);
        return;
    }
    // Static guest buffer for hostent + addr list
    static uint32_t buf = 0;
    if (!buf || !heap_owns(buf))
        buf = heap_alloc(256, true);
    uint32_t p_hostent = buf;
    uint32_t p_aliases = buf + 16;
    uint32_t p_addr_list = buf + 24;
    uint32_t p_addr = buf + 32;
    uint32_t p_name = buf + 40;
    std::memset(g_mem + buf, 0, 256);
    gm_put_str(p_name, name.empty() ? RECOMP_GAME_ID : name.c_str(), 200);
    wr32(p_addr, bswap32(ip_host));
    wr32(p_addr_list, p_addr);
    wr32(p_addr_list + 4, 0);
    wr32(p_aliases, 0);
    wr32(p_hostent + 0, p_name);      // h_name
    wr32(p_hostent + 4, p_aliases);   // h_aliases
    wr16(p_hostent + 8, AF_INET_);    // h_addrtype
    wr16(p_hostent + 10, 4);          // h_length
    wr32(p_hostent + 12, p_addr_list);// h_addr_list
    t_wsa_error = 0;
    set_eax(c, p_hostent);
}

void w_socket(X86 *c) {
    int af = (int)arg(c, 0);
    int type = (int)arg(c, 1);
    int proto = (int)arg(c, 2);
    if (af != AF_INET_) {
        fail_wsa(c, INVALID_SOCKET_, WSAEAFNOSUPPORT_);
        return;
    }
    if (type != SOCK_STREAM_ && type != SOCK_DGRAM_) {
        fail_wsa(c, INVALID_SOCKET_, WSAEINVAL_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t id = g_next_sock_id++;
    if (g_next_sock_id >= 0x40000000u)
        g_next_sock_id = 0x1000u;
    VirtualSocket s;
    s.id = id;
    s.af = af;
    s.type = type;
    s.proto = proto ? proto : (type == SOCK_DGRAM_ ? 17 : 6);
    s.local_ip = g_virtual_ip;
    g_sockets[id] = std::move(s);
    t_wsa_error = 0;
    set_eax(c, id);
}

void w_closesocket(X86 *c) {
    uint32_t sid = arg(c, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    wire_close(sid);
    g_sockets.erase(it);
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_shutdown(X86 *c) {
    uint32_t sid = arg(c, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sockets.find(sid) == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_bind(X86 *c) {
    uint32_t sid = arg(c, 0), addr_ptr = arg(c, 1), namelen = arg(c, 2);
    uint16_t family = 0, port = 0;
    uint32_t ip = 0;
    if (!read_sockaddr_in(addr_ptr, namelen, family, ip, port)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    if (family != AF_INET_) {
        fail_wsa(c, SOCKET_ERROR_, WSAEAFNOSUPPORT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    if (port == 0)
        port = g_next_ephemeral_port++;
    it->second.bound = true;
    it->second.local_ip = ip ? ip : g_virtual_ip;
    it->second.local_port = port;
    wire_open_bind(it->second);
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_listen(X86 *c) {
    uint32_t sid = arg(c, 0), backlog = arg(c, 1);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    if (!it->second.bound) {
        it->second.bound = true;
        it->second.local_ip = g_virtual_ip;
        it->second.local_port = g_next_ephemeral_port++;
        wire_open_bind(it->second);
    }
    it->second.listening = true;
    wire_listen(sid, uint16_t(backlog ? backlog : 5));
    t_wsa_error = 0;
    set_eax(c, 0);
}

void do_accept(X86 *c, uint32_t sid, uint32_t addr_ptr, uint32_t addrlen_ptr) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, INVALID_SOCKET_, WSAENOTSOCK_);
        return;
    }
    if (it->second.accept_queue.empty()) {
        fail_wsa(c, INVALID_SOCKET_, WSAEWOULDBLOCK_);
        return;
    }
    uint32_t child_id = it->second.accept_queue.front();
    it->second.accept_queue.pop_front();
    auto cit = g_sockets.find(child_id);
    if (cit != g_sockets.end() && addr_ptr && addrlen_ptr && gm_valid(addrlen_ptr, 4)) {
        uint32_t cap = rd32(addrlen_ptr);
        if (cap >= 16)
            write_sockaddr_in(addr_ptr, cit->second.peer_ip, cit->second.peer_port);
        wr32(addrlen_ptr, 16);
    }
    t_wsa_error = 0;
    set_eax(c, child_id);
}

void w_accept(X86 *c) {
    do_accept(c, arg(c, 0), arg(c, 1), arg(c, 2));
}

void w_WSAAccept(X86 *c) {
    do_accept(c, arg(c, 0), arg(c, 1), arg(c, 2));
}

void w_connect(X86 *c) {
    uint32_t sid = arg(c, 0), addr_ptr = arg(c, 1), namelen = arg(c, 2);
    uint16_t family = 0, port = 0;
    uint32_t ip = 0;
    if (!read_sockaddr_in(addr_ptr, namelen, family, ip, port)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    if (family != AF_INET_) {
        fail_wsa(c, SOCKET_ERROR_, WSAEAFNOSUPPORT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (s.type == SOCK_DGRAM_) {
        // UDP connect just sets default peer
        s.peer_ip = ip;
        s.peer_port = port;
        s.connected = true;
        if (!s.bound) {
            s.bound = true;
            s.local_ip = g_virtual_ip;
            s.local_port = g_next_ephemeral_port++;
            wire_open_bind(s);
        }
        t_wsa_error = 0;
        set_eax(c, 0);
        return;
    }
    if (!s.bound) {
        s.bound = true;
        s.local_ip = g_virtual_ip;
        s.local_port = g_next_ephemeral_port++;
        wire_open_bind(s);
    }
    s.peer_ip = ip;
    s.peer_port = port;
    s.connecting = true;
    s.connected = false;
    wire_connect(sid, ip, port);
    fail_wsa(c, SOCKET_ERROR_, WSAEWOULDBLOCK_);
}

void w_send(X86 *c) {
    uint32_t sid = arg(c, 0), buf = arg(c, 1), len = arg(c, 2);
    if (!buf && len) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    if (len && !gm_valid(buf, len)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (!s.connected && s.type == SOCK_STREAM_) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTCONN_);
        return;
    }
    if (s.type == SOCK_DGRAM_)
        wire_sendto(sid, s.peer_ip, s.peer_port, len ? g_mem + buf : nullptr, len);
    else
        wire_send(sid, len ? g_mem + buf : nullptr, len);
    t_wsa_error = 0;
    set_eax(c, len);
}

void w_sendto(X86 *c) {
    uint32_t sid = arg(c, 0), buf = arg(c, 1), len = arg(c, 2);
    uint32_t to_ptr = arg(c, 4), tolen = arg(c, 5);
    if ((len && (!buf || !gm_valid(buf, len)))) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    uint16_t family = AF_INET_, port = 0;
    uint32_t ip = 0;
    if (to_ptr && !read_sockaddr_in(to_ptr, tolen, family, ip, port)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (!s.bound) {
        s.bound = true;
        s.local_ip = g_virtual_ip;
        s.local_port = g_next_ephemeral_port++;
        wire_open_bind(s);
    }
    if (!to_ptr) {
        ip = s.peer_ip;
        port = s.peer_port;
    }
    wire_sendto(sid, ip, port, len ? g_mem + buf : nullptr, len);
    t_wsa_error = 0;
    set_eax(c, len);
}

void w_recv(X86 *c) {
    uint32_t sid = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    if (len && (!buf || !gm_valid(buf, len))) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (s.type == SOCK_DGRAM_) {
        if (s.udp_rx.empty()) {
            fail_wsa(c, SOCKET_ERROR_, WSAEWOULDBLOCK_);
            return;
        }
        const Datagram &dg = s.udp_rx.front();
        uint32_t n = std::min<uint32_t>(len, uint32_t(dg.data.size()));
        if (n)
            std::memcpy(g_mem + buf, dg.data.data(), n);
        if (!(flags & 2u)) // MSG_PEEK = 2
            s.udp_rx.pop_front();
        t_wsa_error = 0;
        set_eax(c, n);
        return;
    }
    if (s.tcp_rx.empty()) {
        if (s.closed_by_peer) {
            t_wsa_error = 0;
            set_eax(c, 0);
            return;
        }
        fail_wsa(c, SOCKET_ERROR_, WSAEWOULDBLOCK_);
        return;
    }
    uint32_t n = std::min<uint32_t>(len, uint32_t(s.tcp_rx.size()));
    for (uint32_t i = 0; i < n; ++i)
        g_mem[buf + i] = s.tcp_rx[i];
    if (!(flags & 2u)) // MSG_PEEK = 2
        s.tcp_rx.erase(s.tcp_rx.begin(), s.tcp_rx.begin() + n);
    t_wsa_error = 0;
    set_eax(c, n);
}

void w_recvfrom(X86 *c) {
    uint32_t sid = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    uint32_t from_ptr = arg(c, 4), fromlen_ptr = arg(c, 5);
    if (len && (!buf || !gm_valid(buf, len))) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (s.type == SOCK_DGRAM_) {
        if (s.udp_rx.empty()) {
            fail_wsa(c, SOCKET_ERROR_, WSAEWOULDBLOCK_);
            return;
        }
        const Datagram &dg = s.udp_rx.front();
        uint32_t n = std::min<uint32_t>(len, uint32_t(dg.data.size()));
        if (n)
            std::memcpy(g_mem + buf, dg.data.data(), n);
        if (from_ptr && fromlen_ptr && gm_valid(fromlen_ptr, 4)) {
            if (rd32(fromlen_ptr) >= 16)
                write_sockaddr_in(from_ptr, dg.src_ip, dg.src_port);
            wr32(fromlen_ptr, 16);
        }
        if (!(flags & 2u))
            s.udp_rx.pop_front();
        t_wsa_error = 0;
        set_eax(c, n);
        return;
    }
    // Stream fallback
    if (s.tcp_rx.empty()) {
        if (s.closed_by_peer) {
            t_wsa_error = 0;
            set_eax(c, 0);
            return;
        }
        fail_wsa(c, SOCKET_ERROR_, WSAEWOULDBLOCK_);
        return;
    }
    uint32_t n = std::min<uint32_t>(len, uint32_t(s.tcp_rx.size()));
    for (uint32_t i = 0; i < n; ++i)
        g_mem[buf + i] = s.tcp_rx[i];
    if (from_ptr && fromlen_ptr && gm_valid(fromlen_ptr, 4)) {
        if (rd32(fromlen_ptr) >= 16)
            write_sockaddr_in(from_ptr, s.peer_ip, s.peer_port);
        wr32(fromlen_ptr, 16);
    }
    if (!(flags & 2u))
        s.tcp_rx.erase(s.tcp_rx.begin(), s.tcp_rx.begin() + n);
    t_wsa_error = 0;
    set_eax(c, n);
}

void w_ioctlsocket(X86 *c) {
    uint32_t sid = arg(c, 0), cmd = arg(c, 1), argp = arg(c, 2);
    if (!argp || !gm_valid(argp, 4)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    if (cmd == FIONREAD_) {
        uint32_t avail = 0;
        if (s.type == SOCK_DGRAM_)
            avail = s.udp_rx.empty() ? 0u : uint32_t(s.udp_rx.front().data.size());
        else
            avail = uint32_t(s.tcp_rx.size());
        wr32(argp, avail);
        t_wsa_error = 0;
        set_eax(c, 0);
        return;
    }
    if (cmd == FIONBIO_) {
        s.nonblocking = rd32(argp) != 0;
        t_wsa_error = 0;
        set_eax(c, 0);
        return;
    }
    fail_wsa(c, SOCKET_ERROR_, WSAEINVAL_);
}

void w_setsockopt(X86 *c) {
    uint32_t sid = arg(c, 0), level = arg(c, 1), optname = arg(c, 2), optval = arg(c, 3),
             optlen = arg(c, 4);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    uint32_t val = (optval && optlen >= 4 && gm_valid(optval, 4)) ? rd32(optval) : 0;
    if (level == SOL_SOCKET_) {
        if (optname == SO_REUSEADDR_)
            it->second.reuseaddr = val != 0;
        else if (optname == SO_BROADCAST_)
            it->second.broadcast = val != 0;
    }
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_getsockopt(X86 *c) {
    uint32_t sid = arg(c, 0), level = arg(c, 1), optname = arg(c, 2), optval = arg(c, 3),
             optlen_ptr = arg(c, 4);
    if (!optval || !optlen_ptr || !gm_valid(optlen_ptr, 4)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    uint32_t len = rd32(optlen_ptr);
    if (len >= 4 && gm_valid(optval, 4)) {
        uint32_t out = 0;
        if (level == SOL_SOCKET_ && optname == SO_ERROR_) {
            out = it->second.last_error;
            it->second.last_error = 0;
        } else if (level == SOL_SOCKET_ && optname == SO_TYPE_) {
            out = uint32_t(it->second.type);
        }
        wr32(optval, out);
        wr32(optlen_ptr, 4);
    }
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_getsockname(X86 *c) {
    uint32_t sid = arg(c, 0), addr_ptr = arg(c, 1), namelen_ptr = arg(c, 2);
    if (!addr_ptr || !namelen_ptr || !gm_valid(namelen_ptr, 4)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    if (rd32(namelen_ptr) >= 16)
        write_sockaddr_in(addr_ptr, it->second.local_ip, it->second.local_port);
    wr32(namelen_ptr, 16);
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_getpeername(X86 *c) {
    uint32_t sid = arg(c, 0), addr_ptr = arg(c, 1), namelen_ptr = arg(c, 2);
    if (!addr_ptr || !namelen_ptr || !gm_valid(namelen_ptr, 4)) {
        fail_wsa(c, SOCKET_ERROR_, WSAEFAULT_);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    if (!it->second.connected) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTCONN_);
        return;
    }
    if (rd32(namelen_ptr) >= 16)
        write_sockaddr_in(addr_ptr, it->second.peer_ip, it->second.peer_port);
    wr32(namelen_ptr, 16);
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_WSAAsyncSelect(X86 *c) {
    uint32_t sid = arg(c, 0), hwnd = arg(c, 1), wmsg = arg(c, 2), levent = arg(c, 3);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sockets.find(sid);
    if (it == g_sockets.end()) {
        fail_wsa(c, SOCKET_ERROR_, WSAENOTSOCK_);
        return;
    }
    VirtualSocket &s = it->second;
    s.nonblocking = (levent != 0);
    s.hwnd = hwnd;
    s.wmsg = wmsg;
    s.event_mask = levent;
    if (s.connected && (levent & FD_WRITE_))
        s.pending_events |= FD_WRITE_;
    if (s.listening && !s.accept_queue.empty() && (levent & FD_ACCEPT_))
        s.pending_events |= FD_ACCEPT_;
    if ((!s.tcp_rx.empty() || !s.udp_rx.empty()) && (levent & FD_READ_))
        s.pending_events |= FD_READ_;
    t_wsa_error = 0;
    set_eax(c, 0);
}

void w_select(X86 *c) {
    // Minimal non-blocking fd_set poll
    uint32_t readfds = arg(c, 1), writefds = arg(c, 2), exceptfds = arg(c, 3);
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t ready = 0;
    auto check_set = [&](uint32_t set_ptr, int mode) {
        if (!set_ptr || !gm_valid(set_ptr, 4))
            return;
        uint32_t count = rd32(set_ptr);
        if (count > 64)
            count = 64;
        uint32_t out_cnt = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!gm_valid(set_ptr + 4 + i * 4, 4))
                break;
            uint32_t sid = rd32(set_ptr + 4 + i * 4);
            auto it = g_sockets.find(sid);
            if (it == g_sockets.end())
                continue;
            const VirtualSocket &s = it->second;
            bool ok = false;
            if (mode == 0)
                ok = !s.tcp_rx.empty() || !s.udp_rx.empty() || !s.accept_queue.empty() ||
                     s.closed_by_peer;
            else if (mode == 1)
                ok = s.connected || s.type == SOCK_DGRAM_;
            if (ok) {
                wr32(set_ptr + 4 + out_cnt * 4, sid);
                ++out_cnt;
                ++ready;
            }
        }
        wr32(set_ptr, out_cnt);
    };
    check_set(readfds, 0);
    check_set(writefds, 1);
    if (exceptfds && gm_valid(exceptfds, 4))
        wr32(exceptfds, 0);
    t_wsa_error = 0;
    set_eax(c, ready);
}

void w_unsupported_err(X86 *c) {
    fail_wsa(c, SOCKET_ERROR_, WSAENETDOWN_);
}
void w_unsupported_false(X86 *c) {
    fail_wsa(c, 0, WSAENETDOWN_);
}

const ImportShim g_wsock_shims[] = {
    // WSOCK32.dll (ordinals + names)
    {"WSOCK32.dll", "WSAStartup", 2, w_WSAStartup},
    {"WSOCK32.dll", "ord115", 2, w_WSAStartup},
    {"WSOCK32.dll", "WSACleanup", 0, w_WSACleanup},
    {"WSOCK32.dll", "ord116", 0, w_WSACleanup},
    {"WSOCK32.dll", "WSAGetLastError", 0, w_WSAGetLastError},
    {"WSOCK32.dll", "ord111", 0, w_WSAGetLastError},
    {"WSOCK32.dll", "WSASetLastError", 1, w_WSASetLastError},
    {"WSOCK32.dll", "ord112", 1, w_WSASetLastError},
    {"WSOCK32.dll", "WSAAsyncSelect", 4, w_WSAAsyncSelect},
    {"WSOCK32.dll", "ord101", 4, w_WSAAsyncSelect},
    {"WSOCK32.dll", "socket", 3, w_socket},
    {"WSOCK32.dll", "ord23", 3, w_socket},
    {"WSOCK32.dll", "closesocket", 1, w_closesocket},
    {"WSOCK32.dll", "ord3", 1, w_closesocket},
    {"WSOCK32.dll", "shutdown", 2, w_shutdown},
    {"WSOCK32.dll", "ord22", 2, w_shutdown},
    {"WSOCK32.dll", "bind", 3, w_bind},
    {"WSOCK32.dll", "ord2", 3, w_bind},
    {"WSOCK32.dll", "listen", 2, w_listen},
    {"WSOCK32.dll", "ord13", 2, w_listen},
    {"WSOCK32.dll", "accept", 3, w_accept},
    {"WSOCK32.dll", "ord1", 3, w_accept},
    {"WSOCK32.dll", "connect", 3, w_connect},
    {"WSOCK32.dll", "ord4", 3, w_connect},
    {"WSOCK32.dll", "send", 4, w_send},
    {"WSOCK32.dll", "ord19", 4, w_send},
    {"WSOCK32.dll", "sendto", 6, w_sendto},
    {"WSOCK32.dll", "ord20", 6, w_sendto},
    {"WSOCK32.dll", "recv", 4, w_recv},
    {"WSOCK32.dll", "ord16", 4, w_recv},
    {"WSOCK32.dll", "recvfrom", 6, w_recvfrom},
    {"WSOCK32.dll", "ord17", 6, w_recvfrom},
    {"WSOCK32.dll", "select", 5, w_select},
    {"WSOCK32.dll", "ord18", 5, w_select},
    {"WSOCK32.dll", "ioctlsocket", 3, w_ioctlsocket},
    {"WSOCK32.dll", "ord12", 3, w_ioctlsocket},
    {"WSOCK32.dll", "setsockopt", 5, w_setsockopt},
    {"WSOCK32.dll", "ord21", 5, w_setsockopt},
    {"WSOCK32.dll", "getsockopt", 5, w_getsockopt},
    {"WSOCK32.dll", "ord7", 5, w_getsockopt},
    {"WSOCK32.dll", "getsockname", 3, w_getsockname},
    {"WSOCK32.dll", "ord6", 3, w_getsockname},
    {"WSOCK32.dll", "getpeername", 3, w_getpeername},
    {"WSOCK32.dll", "ord5", 3, w_getpeername},
    {"WSOCK32.dll", "htonl", 1, w_htonl},
    {"WSOCK32.dll", "ord8", 1, w_htonl},
    {"WSOCK32.dll", "ntohl", 1, w_htonl},
    {"WSOCK32.dll", "ord14", 1, w_htonl},
    {"WSOCK32.dll", "htons", 1, w_htons},
    {"WSOCK32.dll", "ord9", 1, w_htons},
    {"WSOCK32.dll", "ntohs", 1, w_htons},
    {"WSOCK32.dll", "ord15", 1, w_htons},
    {"WSOCK32.dll", "inet_addr", 1, w_inet_addr},
    {"WSOCK32.dll", "ord10", 1, w_inet_addr},
    {"WSOCK32.dll", "inet_ntoa", 1, w_inet_ntoa},
    {"WSOCK32.dll", "ord11", 1, w_inet_ntoa},
    {"WSOCK32.dll", "gethostname", 2, w_gethostname},
    {"WSOCK32.dll", "ord57", 2, w_gethostname},
    {"WSOCK32.dll", "gethostbyname", 1, w_gethostbyname},
    {"WSOCK32.dll", "ord52", 1, w_gethostbyname},

    // WS2_32.dll
    {"WS2_32.dll", "WSAStartup", 2, w_WSAStartup},
    {"WS2_32.dll", "ord115", 2, w_WSAStartup},
    {"WS2_32.dll", "WSACleanup", 0, w_WSACleanup},
    {"WS2_32.dll", "ord116", 0, w_WSACleanup},
    {"WS2_32.dll", "WSAGetLastError", 0, w_WSAGetLastError},
    {"WS2_32.dll", "ord111", 0, w_WSAGetLastError},
    {"WS2_32.dll", "WSASetLastError", 1, w_WSASetLastError},
    {"WS2_32.dll", "ord112", 1, w_WSASetLastError},
    {"WS2_32.dll", "WSAAsyncSelect", 4, w_WSAAsyncSelect},
    {"WS2_32.dll", "ord101", 4, w_WSAAsyncSelect},
    {"WS2_32.dll", "socket", 3, w_socket},
    {"WS2_32.dll", "ord23", 3, w_socket},
    {"WS2_32.dll", "closesocket", 1, w_closesocket},
    {"WS2_32.dll", "ord3", 1, w_closesocket},
    {"WS2_32.dll", "shutdown", 2, w_shutdown},
    {"WS2_32.dll", "ord22", 2, w_shutdown},
    {"WS2_32.dll", "bind", 3, w_bind},
    {"WS2_32.dll", "ord2", 3, w_bind},
    {"WS2_32.dll", "listen", 2, w_listen},
    {"WS2_32.dll", "ord13", 2, w_listen},
    {"WS2_32.dll", "accept", 3, w_accept},
    {"WS2_32.dll", "ord1", 3, w_accept},
    {"WS2_32.dll", "WSAAccept", 5, w_WSAAccept},
    {"WS2_32.dll", "connect", 3, w_connect},
    {"WS2_32.dll", "ord4", 3, w_connect},
    {"WS2_32.dll", "send", 4, w_send},
    {"WS2_32.dll", "ord19", 4, w_send},
    {"WS2_32.dll", "sendto", 6, w_sendto},
    {"WS2_32.dll", "ord20", 6, w_sendto},
    {"WS2_32.dll", "recv", 4, w_recv},
    {"WS2_32.dll", "ord16", 4, w_recv},
    {"WS2_32.dll", "recvfrom", 6, w_recvfrom},
    {"WS2_32.dll", "ord17", 6, w_recvfrom},
    {"WS2_32.dll", "select", 5, w_select},
    {"WS2_32.dll", "ord18", 5, w_select},
    {"WS2_32.dll", "ioctlsocket", 3, w_ioctlsocket},
    {"WS2_32.dll", "ord10", 3, w_ioctlsocket},
    {"WS2_32.dll", "setsockopt", 5, w_setsockopt},
    {"WS2_32.dll", "ord21", 5, w_setsockopt},
    {"WS2_32.dll", "getsockopt", 5, w_getsockopt},
    {"WS2_32.dll", "ord7", 5, w_getsockopt},
    {"WS2_32.dll", "getsockname", 3, w_getsockname},
    {"WS2_32.dll", "ord6", 3, w_getsockname},
    {"WS2_32.dll", "getpeername", 3, w_getpeername},
    {"WS2_32.dll", "ord5", 3, w_getpeername},
    {"WS2_32.dll", "gethostname", 2, w_gethostname},
    {"WS2_32.dll", "ord57", 2, w_gethostname},
    {"WS2_32.dll", "gethostbyname", 1, w_gethostbyname},
    {"WS2_32.dll", "ord52", 1, w_gethostbyname},
    {"WS2_32.dll", "WSAIoctl", 9, w_unsupported_err},
    {"WS2_32.dll", "WSACreateEvent", 0, w_unsupported_false},
    {"WS2_32.dll", "WSACloseEvent", 1, w_unsupported_false},
    {"WS2_32.dll", "WSASetEvent", 1, w_unsupported_false},
    {"WS2_32.dll", "WSAResetEvent", 1, w_unsupported_false},
    {"WS2_32.dll", "WSAWaitForMultipleEvents", 5, w_unsupported_err},
    {"WS2_32.dll", "WSARecv", 7, w_unsupported_err},
    {"WS2_32.dll", "WSARecvFrom", 9, w_unsupported_err},
    {"WS2_32.dll", "WSAGetOverlappedResult", 5, w_unsupported_false},
};

} // namespace

void wsock_register() {
    imports_register(g_wsock_shims, sizeof g_wsock_shims / sizeof g_wsock_shims[0]);
}

namespace wsock {

void pump_async_select() {
    struct PendingMsg {
        uint32_t hwnd;
        uint32_t msg;
        uint32_t wparam;
        uint32_t lparam;
    };
    std::vector<PendingMsg> to_post;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &kv : g_sockets) {
            VirtualSocket &s = kv.second;
            if (!s.hwnd || !s.wmsg || !s.pending_events)
                continue;
            uint32_t ev = s.pending_events & s.event_mask;
            s.pending_events = 0;
            if (!ev)
                continue;
            // Emit one message per active event bit, matching Win32 WSAAsyncSelect
            for (uint32_t bit = 1u; bit <= FD_CLOSE_; bit <<= 1) {
                if (!(ev & bit))
                    continue;
                uint32_t err = (bit == FD_CONNECT_) ? s.connect_error : 0u;
                uint32_t lp = (err << 16) | (bit & 0xffffu);
                to_post.push_back({s.hwnd, s.wmsg, s.id, lp});
            }
        }
    }
    if (to_post.empty())
        return;
    const uint32_t now_ms = uint32_t(os_monotonic_ns() / 1000000ull);
    for (const auto &pm : to_post) {
        user32::Msg m{pm.hwnd, pm.msg, pm.wparam, pm.lparam, now_ms,
                      uint32_t(user32::g_cursor_x), uint32_t(user32::g_cursor_y)};
        user32::queue().push_back(m);
    }
}

} // namespace wsock

#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE uint8_t *recomp_wsock_rx_buf(int len) {
    static std::vector<uint8_t> rx_staging;
    if (len > 0 && (size_t)len > rx_staging.size())
        rx_staging.resize((size_t)len);
    return rx_staging.data();
}

EMSCRIPTEN_KEEPALIVE void recomp_wsock_on_packet(const uint8_t *data, int len) {
    if (!data || len < 1)
        return;
    const uint8_t op = data[0];
    std::lock_guard<std::mutex> lk(g_mu);
    if (op == OP_ASSIGN_IP && len >= 5) {
        g_virtual_ip = get_be32(&data[1]);
        log_msg(1, "wsock: assigned virtual IP %u.%u.%u.%u", (g_virtual_ip >> 24) & 0xffu,
                (g_virtual_ip >> 16) & 0xffu, (g_virtual_ip >> 8) & 0xffu, g_virtual_ip & 0xffu);
        return;
    }
    if (op == OP_CONNECT_RESULT && len >= 15) {
        uint32_t sid = get_be32(&data[1]);
        uint32_t err = get_be32(&data[5]);
        uint32_t lip = get_be32(&data[9]);
        uint16_t lport = get_be16(&data[13]);
        auto it = g_sockets.find(sid);
        if (it == g_sockets.end())
            return;
        VirtualSocket &s = it->second;
        s.connecting = false;
        s.connect_error = err;
        if (err == 0) {
            s.connected = true;
            if (lip)
                s.local_ip = lip;
            if (lport)
                s.local_port = lport;
            s.pending_events |= FD_CONNECT_ | FD_WRITE_;
        } else {
            s.last_error = err;
            s.pending_events |= FD_CONNECT_;
        }
        return;
    }
    if (op == OP_ACCEPT && len >= 15) {
        uint32_t listen_sid = get_be32(&data[1]);
        uint32_t child_sid = get_be32(&data[5]);
        uint32_t peer_ip = get_be32(&data[9]);
        uint16_t peer_port = get_be16(&data[13]);
        auto it = g_sockets.find(listen_sid);
        if (it == g_sockets.end())
            return;
        VirtualSocket child;
        child.id = child_sid;
        child.af = AF_INET_;
        child.type = SOCK_STREAM_;
        child.proto = 6;
        child.connected = true;
        child.bound = true;
        child.local_ip = it->second.local_ip;
        child.local_port = it->second.local_port;
        child.peer_ip = peer_ip;
        child.peer_port = peer_port;
        g_sockets[child_sid] = std::move(child);
        it->second.accept_queue.push_back(child_sid);
        it->second.pending_events |= FD_ACCEPT_;
        return;
    }
    if (op == OP_RECV && len >= 5) {
        uint32_t sid = get_be32(&data[1]);
        auto it = g_sockets.find(sid);
        if (it == g_sockets.end())
            return;
        it->second.tcp_rx.insert(it->second.tcp_rx.end(), data + 5, data + len);
        it->second.pending_events |= FD_READ_;
        return;
    }
    if (op == OP_RECVFROM && len >= 11) {
        uint32_t sid = get_be32(&data[1]);
        auto it = g_sockets.find(sid);
        if (it == g_sockets.end())
            return;
        Datagram dg;
        dg.src_ip = get_be32(&data[5]);
        dg.src_port = get_be16(&data[9]);
        dg.data.assign(data + 11, data + len);
        it->second.udp_rx.push_back(std::move(dg));
        it->second.pending_events |= FD_READ_;
        return;
    }
    if (op == OP_CLOSED && len >= 9) {
        uint32_t sid = get_be32(&data[1]);
        uint32_t err = get_be32(&data[5]);
        auto it = g_sockets.find(sid);
        if (it == g_sockets.end())
            return;
        it->second.closed_by_peer = true;
        it->second.connected = false;
        it->second.last_error = err;
        it->second.pending_events |= FD_CLOSE_;
    }
}
}
#endif
