#define _GNU_SOURCE
#include "mem/talloc.h"
#include "net/tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* mem_talloc_alloc header (must be available on build host) */

/* ---------- Module-global mem_talloc_alloc context ---------- */
void *net_tcp_mem_ctx = NULL;

/* ---------- Internals ---------- */

/* Connection object */
struct net_tcp_connection {
    int fd;
    _Atomic unsigned int events; /* bitmask of net_tcp_Event */
    net_tcp_event_handler handlers[8];
    void* userdata[8];
};

struct net_tcp_addr{
    char ip[16];      /* "127.0.0.1" */
    uint16_t port;    /* host order */
};

/* Small connection table helpers are optional; for now we will not export them. */

/* Convert net_tcp_Event bitmask to epoll events */
static uint32_t events_to_epoll(unsigned int ev)
{
    uint32_t e = 0;
    if (ev & NET_TCP_EVENT_READABLE) e |= EPOLLIN | EPOLLRDHUP;
    if (ev & NET_TCP_EVENT_WRITABLE) e |= EPOLLOUT;
    if (ev & NET_TCP_EVENT_ERROR)    e |= EPOLLERR | EPOLLHUP;
    return e;
}

/* Convert epoll events to net_tcp_Event bitmask */
static unsigned int epoll_to_events(uint32_t epev)
{
    unsigned int ev = 0;
    if (epev & (EPOLLIN | EPOLLRDHUP)) ev |= NET_TCP_EVENT_READABLE;
    if (epev & EPOLLOUT)               ev |= NET_TCP_EVENT_WRITABLE;
    if (epev & (EPOLLERR | EPOLLHUP))  ev |= NET_TCP_EVENT_ERROR;
    return ev;
}

/* Make socket non-blocking */
static int make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

/* ---------- Module init / free ---------- */

void net_tcp_module_init(void)
{
    if (net_tcp_mem_ctx) return;
    net_tcp_mem_ctx = mem_talloc_new_ctx(NULL);
    if (!net_tcp_mem_ctx) {
        fprintf(stderr, "net_tcp: failed to create mem_talloc_alloc context\n");
        /* not fatal here, but allocations will fail */
    }
}

void net_tcp_module_free(void)
{
    if (!net_tcp_mem_ctx) return;
    mem_talloc_free(net_tcp_mem_ctx);
    net_tcp_mem_ctx = NULL;
}

/* ---------- Addr factory ---------- */

net_tcp_addr* net_tcp_addr_make(const char* ip, uint16_t port)
{
    if (!net_tcp_mem_ctx) net_tcp_module_init();
    net_tcp_addr* a = mem_talloc_alloc(net_tcp_mem_ctx, sizeof(net_tcp_addr));
    if (!a) return NULL;
    strncpy(a->ip, ip, sizeof(a->ip) - 1);
    a->ip[sizeof(a->ip)-1] = '\0';
    a->port = port;
    return a;
}

/* ---------- Create socket helpers ---------- */

static int create_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    return fd;
}

/* ---------- connect/listen/accept implementations ---------- */

net_tcp_connection* net_tcp_connect(const net_tcp_addr* addr)
{
    if (!net_tcp_mem_ctx) net_tcp_module_init();

    int fd = create_socket();
    if (fd < 0) {
        perror("net_tcp_connect: socket");
        return NULL;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr->port);
    if (inet_pton(AF_INET, addr->ip, &sa.sin_addr) <= 0) {
        perror("net_tcp_connect: inet_pton");
        close(fd);
        return NULL;
    }

    int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        perror("net_tcp_connect: connect");
        close(fd);
        return NULL;
    }

    net_tcp_connection* c = mem_talloc_alloc(net_tcp_mem_ctx, sizeof(net_tcp_connection));
    if (!c) {
        close(fd);
        return NULL;
    }

    c->fd = fd;
    atomic_store_explicit(&c->events, NET_TCP_EVENT_READABLE | NET_TCP_EVENT_WRITABLE | NET_TCP_EVENT_ERROR, memory_order_relaxed);
    for (size_t i = 0; i < sizeof(c->handlers)/sizeof(c->handlers[0]); ++i) {
        c->handlers[i] = NULL;
        c->userdata[i] = NULL;
    }
    return c;
}

net_tcp_connection* net_tcp_listen(const net_tcp_addr* addr)
{
    if (!net_tcp_mem_ctx) net_tcp_module_init();

    int fd = create_socket();
    if (fd < 0) {
        perror("net_tcp_listen: socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr->port);
    if (inet_pton(AF_INET, addr->ip, &sa.sin_addr) <= 0) {
        perror("net_tcp_listen: inet_pton");
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("net_tcp_listen: bind");
        close(fd);
        return NULL;
    }
    if (listen(fd, 128) < 0) {
        perror("net_tcp_listen: listen");
        close(fd);
        return NULL;
    }

    net_tcp_connection* c = mem_talloc_alloc(net_tcp_mem_ctx, sizeof(net_tcp_connection));
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    atomic_store_explicit(&c->events, NET_TCP_EVENT_READABLE | NET_TCP_EVENT_ERROR, memory_order_relaxed);
    for (size_t i = 0; i < sizeof(c->handlers)/sizeof(c->handlers[0]); ++i) {
        c->handlers[i] = NULL;
        c->userdata[i] = NULL;
    }
    return c;
}

net_tcp_connection* net_tcp_accept(net_tcp_connection* listener)
{
    if (!listener) return NULL;
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int client_fd = accept(listener->fd, (struct sockaddr*)&sa, &len);
    if (client_fd < 0) return NULL;

    /* make non-blocking if accept didn't return O_NONBLOCK */
    if (make_nonblocking(client_fd) != 0) {
        /* not fatal */
    }

    net_tcp_connection* c = mem_talloc_alloc(net_tcp_mem_ctx, sizeof(net_tcp_connection));
    if (!c) {
        close(client_fd);
        return NULL;
    }
    c->fd = client_fd;
    atomic_store_explicit(&c->events, NET_TCP_EVENT_READABLE | NET_TCP_EVENT_WRITABLE | NET_TCP_EVENT_ERROR, memory_order_relaxed);
    for (size_t i = 0; i < sizeof(c->handlers)/sizeof(c->handlers[0]); ++i) {
        c->handlers[i] = NULL;
        c->userdata[i] = NULL;
    }
    return c;
}

/* ---------- Handler registration, ignore, resume ---------- */

void net_tcp_connection_on(net_tcp_connection* conn, net_tcp_event ev, net_tcp_event_handler handler, void* userdata)
{
    if (!conn) return;
    /* support ALL */
    if (ev == NET_TCP_EVENT_ALL) {
        for (int i = 0; i < 8; ++i) {
            conn->handlers[i] = handler;
            conn->userdata[i] = userdata;
        }
        return;
    }
    /* set specific handler if in range */
    if ((ev & (NET_TCP_EVENT_READABLE | NET_TCP_EVENT_WRITABLE |
               NET_TCP_EVENT_HUP | NET_TCP_EVENT_RDHUP |
               NET_TCP_EVENT_ERROR)) == ev) {
        /* find first bit index and set */
        for (int i = 0; i < 8; ++i) {
            if (ev & (1u << i)) {
                conn->handlers[i] = handler;
                conn->userdata[i] = userdata;
            }
        }
    }
}

void net_tcp_connection_ignore(net_tcp_connection* conn, net_tcp_event ev)
{
    if (!conn) return;
    if (ev == NET_TCP_EVENT_ALL) {
        atomic_store_explicit(&conn->events, NET_TCP_EVENT_NOTHING, memory_order_relaxed);
        for (size_t i = 0; i < 8; ++i) conn->handlers[i] = NULL;
        return;
    }
    /* clear bits from the subscription mask */
    unsigned int cur = atomic_load_explicit(&conn->events, memory_order_relaxed);
    cur &= ~ev;
    atomic_store_explicit(&conn->events, cur, memory_order_relaxed);
    /* optional: remove handlers for those bits */
    for (int i = 0; i < 8; ++i) {
        if (ev & (1u << i)) conn->handlers[i] = NULL;
    }
}

void net_tcp_connection_resume(net_tcp_connection* conn, net_tcp_event ev, net_tcp_event_handler handler)
{
    if (!conn) return;
    if (ev == NET_TCP_EVENT_ALL) {
        atomic_store_explicit(&conn->events, NET_TCP_EVENT_ALL, memory_order_relaxed);
        /* handler must be re-registered by caller if needed */
        return;
    }
    unsigned int cur = atomic_load_explicit(&conn->events, memory_order_relaxed);
    cur |= ev;
    atomic_store_explicit(&conn->events, cur, memory_order_relaxed);
    /* optionally set handler if provided */
    if (handler) {
        for (int i = 0; i < 8; ++i) {
            if (ev & (1u << i)) conn->handlers[i] = handler;
        }
    }
}

/* Close connection */
void net_tcp_connection_close(net_tcp_connection* conn)
{
    if (!conn) return;
    if (conn->fd >= 0) {
        close(conn->fd);
    }
    /* mem_talloc_alloc context owns the memory; freeing module ctx will free this as well.
       If you want to free a single connection, use mem_talloc_alloc_free(conn); */
    mem_talloc_free(conn);
}

/* ---------- Simple epoll helper implementation ---------- */

int net_tcp_create_epoll(void)
{
    int epfd = epoll_create1(0);
    return epfd;
}

/* Helper to register fd with epoll for the connection's desired events */
static int __attribute__((unused)) register_conn_epoll(int epfd, net_tcp_connection* conn)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events_to_epoll(atomic_load_explicit(&conn->events, memory_order_relaxed));
    ev.data.fd = conn->fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &ev) < 0) {
        if (errno == EEXIST) {
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) return -1;
            return 0;
        }
        return -1;
    }
    return 0;
}

/* This is a minimal event loop step: wait for events and dispatch callbacks */
void net_tcp_run_once(int epfd, int timeout_ms)
{
    const int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];
    int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
    for (int i = 0; i < n; ++i) {
        uint32_t epev = events[i].events;
        unsigned int evmask = epoll_to_events(epev);
        (void)evmask;
        int fd = events[i].data.fd;

        /* In a fuller implementation you map fd -> connection state (table). For demo, we
           attempt to use a mem_talloc_alloc pointer stored in user data via some map. Here we
           assume the caller manages fd->conn mapping and calls net_tcp_accept / etc. */
        (void)fd;

        /* Incomplete: dispatch requires finding the connection object for the fd.
           The real implementation needs a map from fd -> net_tcp_connection*. */
    }
}
