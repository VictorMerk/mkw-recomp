// Exercise the production LAN socket overrides with synthetic guest registers and owned loopback UDP sockets.
// Single-thread harness: runtime logging, memory-commit and scheduler callbacks are inert; no game or GPU is needed.
#include "host.h"
#undef HOST_OVERRIDE
#define HOST_OVERRIDE(sym, fn)
#include "../../runtime/sdk/ov/ov_socket.c"
#include <assert.h>
#include <stdarg.h>

static const char *test_peers = "", *test_ip = "";
static int test_lan;
const char *cfg_get(const char *name) {
    if (!strcmp(name, "MKW_LAN_PEERS")) return test_peers;
    if (!strcmp(name, "MKW_LAN_IP")) return test_ip;
    return NULL;
}
int cfg_bool(const char *name) { return !strcmp(name, "MKW_LAN") && test_lan; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void mem_commit_range(uint64_t a, uint64_t n) { (void)a; (void)n; }
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { (void)away; }
void plat_sleep_ms(uint32_t ms) { (void)ms; }

static uint32_t ip(const char *text) {
    struct in_addr a;
    assert(inet_pton(AF_INET, text, &a) == 1);
    return a.s_addr;
}
static HostSock receiver(uint32_t addr, uint16_t port, struct sockaddr_in *bound) {
    HostSock s = socket(AF_INET, SOCK_DGRAM, 0);
    assert(s != HOST_BAD);
    *bound = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = port};
    bound->sin_addr.s_addr = addr;
    assert(!bind(s, (struct sockaddr *)bound, sizeof *bound));
#ifdef _WIN32
    int len = sizeof *bound;
#else
    socklen_t len = sizeof *bound;
#endif
    assert(!getsockname(s, (struct sockaddr *)bound, &len));
    return s;
}
static void received(HostSock s, int expected) {
    struct pollfd p = {0};
    p.fd = s;
    p.events = ev_host(G_POLLIN);
    int result = host_poll(&p, 1, expected ? 1000 : 20);
    if (!expected) { assert(result == 0); return; }
    assert(result == 1);
    char data[32];
    int n = recvfrom(s, data, sizeof data, 0, NULL, NULL);
    assert(n == 5 && !memcmp(data, "hello", 5));
}
static int64_t send_guest(int fd, uint32_t addr, uint16_t port) {
    uint8_t guest[16] = {16, B_AF_INET};
    memcpy(guest + 2, &port, 2);
    memcpy(guest + 4, &addr, 4);
    Cpu c = {0};
    c.x[0] = fd; c.x[1] = (uintptr_t)"hello"; c.x[2] = 5;
    c.x[4] = (uintptr_t)guest; c.x[5] = sizeof guest;
    ov_sendto(&c);
    return (int64_t)c.x[0];
}
int main(void) {
    Cpu c = {0};
    c.x[0] = B_AF_INET; c.x[1] = SOCK_DGRAM;
    ov_socket(&c);
    assert((int64_t)c.x[0] == -1);   // opt-in remains required
    test_lan = 1;
    ov_init(&c);
    assert(!c.x[0]);
    uint32_t out[OV_LAN_PEERS_MAX] = {0};
    test_peers = "127.0.0.1,8.8.8.8;100.64.0.2 192.168.1.2";
    assert(ov_lan_peer_first(out) && out[0] == ip("127.0.0.1"));
    ov_lan_set_broadcast(ip("192.168.1.255"));
    assert(ov_lan_peers(ip("192.168.1.255"), out, OV_LAN_PEERS_MAX) == 3);
    assert(out[1] == ip("100.64.0.2") && out[2] == ip("192.168.1.2"));
    assert(!ov_lan_peers(ip("127.0.0.1"), out, OV_LAN_PEERS_MAX));
    test_peers = "127.0.0.2";
    assert(ov_lan_peer_first(out) && out[0] == ip("127.0.0.2")); // settings refresh
    test_peers = "";
    assert(!ov_lan_peer_first(out));
    test_ip = "127.0.0.2";
    assert(ov_lan_bind_address() == ip(test_ip));
    test_ip = "192.168.1.2";
    assert(!ov_lan_bind_address());

    struct sockaddr_in a, b;
    HostSock sa = receiver(ip("127.0.0.1"), 0, &a);
    HostSock sb = receiver(ip("127.0.0.2"), a.sin_port, &b);
    c = (Cpu){0}; c.x[0] = B_AF_INET; c.x[1] = SOCK_DGRAM;
    ov_socket(&c);
    int fd = (int)c.x[0];
    assert(fd >= 0);
    test_ip = "127.0.0.2";
    uint8_t any[16] = {16, B_AF_INET};
    c = (Cpu){0}; c.x[0] = fd; c.x[1] = (uintptr_t)any; c.x[2] = sizeof any;
    ov_bind(&c);
    assert(!c.x[0]);
    Slot slot; assert(!slot_get(fd, &slot));
    struct sockaddr_in own;
#ifdef _WIN32
    int len = sizeof own;
#else
    socklen_t len = sizeof own;
#endif
    assert(!getsockname(slot.s, (struct sockaddr *)&own, &len));
    assert(own.sin_addr.s_addr == ip("127.0.0.2"));
    test_peers = "127.0.0.1 127.0.0.2";
    assert(send_guest(fd, ip("192.168.1.255"), a.sin_port) == 5);
    received(sa, 1); received(sb, 1);
    assert(send_guest(fd, a.sin_addr.s_addr, a.sin_port) == 5);
    received(sa, 1); received(sb, 0); // direct sends must not fan out
    assert(send_guest(fd, ip("8.8.8.8"), a.sin_port) == -1 && last_errno == G_ENETUNREACH);
    received(sa, 0); received(sb, 0);
    c = (Cpu){0}; c.x[0] = fd; ov_close(&c);
    assert(!c.x[0]);
    host_close(sa); host_close(sb);
    puts("LAN peer filtering, refresh, loopback binding, broadcast fanout and public-address rejection passed");
    return 0;
}
