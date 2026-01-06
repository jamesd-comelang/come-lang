// Filename: src/include/net/tcp.h
#ifndef COME_NET_TCP_H
#define COME_NET_TCP_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

// Forward declarations
typedef struct net_tcp_addr net_tcp_addr;
typedef struct net_tcp_connection net_tcp_connection;

// Event types
typedef enum {
    NET_TCP_EVENT_READABLE = 1 << 0,
    NET_TCP_EVENT_WRITABLE = 1 << 1,
    NET_TCP_EVENT_HUP      = 1 << 2,
    NET_TCP_EVENT_RDHUP    = 1 << 3,
    NET_TCP_EVENT_ERROR    = 1 << 4,
    NET_TCP_EVENT_ALL      = 0xFFFFFFFF,
    NET_TCP_EVENT_NOTHING  = 0
} net_tcp_event;

// Event handler callback
typedef void (*net_tcp_event_handler)(net_tcp_connection* conn, void* userdata);

// --- Public API Functions ---

// Module init/free
void net_tcp_module_init(void);
void net_tcp_module_free(void);

// Address factory
net_tcp_addr* net_tcp_addr_make(const char* ip, uint16_t port);

// Connection setup
net_tcp_connection* net_tcp_listen(const net_tcp_addr* addr);
net_tcp_connection* net_tcp_connect(const net_tcp_addr* addr);
net_tcp_connection* net_tcp_accept(net_tcp_connection* listener);

// Connection management
void net_tcp_connection_close(net_tcp_connection* conn);

// Event registration
void net_tcp_connection_on(net_tcp_connection* conn, net_tcp_event ev, net_tcp_event_handler handler, void* userdata);
void net_tcp_connection_ignore(net_tcp_connection* conn, net_tcp_event ev);
void net_tcp_connection_resume(net_tcp_connection* conn, net_tcp_event ev, net_tcp_event_handler handler);

// Event loop helpers
int net_tcp_create_epoll(void);
void net_tcp_run_once(int epfd, int timeout_ms);

#endif // COME_NET_TCP_H
