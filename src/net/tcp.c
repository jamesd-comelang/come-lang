// Filename: src/net/tcp.c - Refactored with Linux epoll

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h> // NEW: for epoll

// --- COME/talloc Dependencies ---
#include "mem/talloc.h"

// --- Global State ---
#define MAX_EVENTS 64
static int epoll_fd = -1;

// --- Struct Definitions for COME API ---

// Forward declaration for self-referencing
typedef struct net_tcp_connection net_tcp_connection;

// Struct for a TCP address (net.tcp.addr in COME)
typedef struct net_tcp_addr {
    void* mem_ctx;
    int family;
    int port;
    char ip[INET6_ADDRSTRLEN];
} net_tcp_addr;

// Struct for a TCP connection (net.tcp.connection in COME)
typedef struct net_tcp_connection {
    void* mem_ctx;
    int fd;        // Socket file descriptor
    net_tcp_addr* local_addr;
    net_tcp_addr* remote_addr;
    int is_listening;

    // Event Handlers (C implementation of COME's .on() methods)
    void (*handler_accept)(net_tcp_connection* listener, net_tcp_connection* new_conn);
    void (*handler_connect_done)(net_tcp_connection* conn); // For async connect completion
    void (*handler_data_ready)(net_tcp_connection* conn); // Corresponds to DATA_READY
    void (*handler_write_done)(net_tcp_connection* conn); // Corresponds to DATA_DONE/DONE
    void (*handler_close)(net_tcp_connection* conn); // For connection close/error

} net_tcp_connection;

// --- Helper Functions ---

// Sets a file descriptor to non-blocking mode
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Initializes the global epoll instance if it hasn't been already.
static int init_epoll() {
    if (epoll_fd == -1) {
        // The size hint (1) is ignored in modern Linux, but required for compatibility.
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            perror("epoll_create1");
            return -1;
        }
    }
    return 0;
}

// Registers or modifies a file descriptor in the epoll instance.
static int epoll_register(int op, int fd, uint32_t events, net_tcp_connection* conn) {
    if (epoll_fd == -1) {
        fprintf(stderr, "Error: epoll not initialized\n");
        return -1;
    }

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events | EPOLLET; // Edge-triggered mode is generally preferred for performance
    event.data.ptr = conn;           // Pass the connection object itself

    if (epoll_ctl(epoll_fd, op, fd, &event) == -1) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
}

// --- CORE FUNCTIONS (COME API Implementation) ---

// net.tcp.addr_make(ip, port)
net_tcp_addr* net_tcp_addr_make(void* mem_ctx, const char* ip, int port) {
    net_tcp_addr* a = talloc_type(mem_ctx, net_tcp_addr);
    if (!a) return NULL;

    a->mem_ctx = mem_ctx;
    a->port = port;
    strncpy(a->ip, ip, INET6_ADDRSTRLEN - 1);
    a->ip[INET6_ADDRSTRLEN - 1] = '\0';
    return a;
}

// net.tcp.listen(addr)
net_tcp_connection* net_tcp_listen(void* mem_ctx, net_tcp_addr* addr) {
    if (init_epoll() < 0) return NULL;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return NULL;

    // Setup socket options
    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (set_nonblocking(listen_fd) < 0) {
        close(listen_fd);
        return NULL;
    }

    // Bind and listen setup
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(addr->port);
    inet_pton(AF_INET, addr->ip, &saddr.sin_addr);

    if (bind(listen_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        close(listen_fd);
        return NULL;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        close(listen_fd);
        return NULL;
    }

    net_tcp_connection* c = talloc_type(mem_ctx, net_tcp_connection);
    if (!c) {
        close(listen_fd);
        return NULL;
    }

    c->mem_ctx = mem_ctx;
    c->fd = listen_fd;
    c->is_listening = 1;
    c->local_addr = addr;
    c->remote_addr = NULL;
    c->handler_accept = NULL;
    c->handler_connect_done = NULL;
    c->handler_data_ready = NULL;
    c->handler_write_done = NULL;
    c->handler_close = NULL;

    // Register the listener FD for incoming connections
    if (epoll_register(EPOLL_CTL_ADD, listen_fd, EPOLLIN, c) < 0) {
        // In a real talloc system, cleanup would be done via talloc_free(c);
        close(listen_fd);
        return NULL;
    }

    return c;
}

// net.tcp.connect(addr)
net_tcp_connection* net_tcp_connect(void* mem_ctx, net_tcp_addr* addr) {
    if (init_epoll() < 0) return NULL;

    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn_fd < 0) return NULL;

    if (set_nonblocking(conn_fd) < 0) {
        close(conn_fd);
        return NULL;
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(addr->port);
    inet_pton(AF_INET, addr->ip, &saddr.sin_addr);

    // connect() on a non-blocking socket will return EINPROGRESS (unless immediately successful)
    int ret = connect(conn_fd, (struct sockaddr*)&saddr, sizeof(saddr));
    
    net_tcp_connection* c = talloc_type(mem_ctx, net_tcp_connection);
    if (!c) {
        close(conn_fd);
        return NULL;
    }

    c->mem_ctx = mem_ctx;
    c->fd = conn_fd;
    c->is_listening = 0;
    c->local_addr = NULL;
    c->remote_addr = addr;
    c->handler_accept = NULL;
    c->handler_connect_done = NULL;
    c->handler_data_ready = NULL;
    c->handler_write_done = NULL;
    c->handler_close = NULL;
    
    // Register the FD for WRITE events (to detect connect completion)
    // If connect was immediately successful (ret == 0), we should still register for future events.
    if (epoll_register(EPOLL_CTL_ADD, conn_fd, EPOLLIN | EPOLLOUT, c) < 0) {
        close(conn_fd);
        // talloc cleanup here
        return NULL;
    }

    return c;
}

// net.tcp.close(conn)
void net_tcp_close(net_tcp_connection* conn) {
    if (conn && conn->fd >= 0) {
        // Remove from epoll instance
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        conn->fd = -1;
        // In a full talloc system, we would talloc_free(conn) to clean up memory.
    }
}

// net.tcp.read(conn, buffer, len)
ssize_t net_tcp_read(net_tcp_connection* conn, char* buf, size_t len) {
    if (!conn || conn->fd < 0) return -1;
    return recv(conn->fd, buf, len, 0);
}

// net.tcp.write(conn, buffer, len)
ssize_t net_tcp_write(net_tcp_connection* conn, const char* buf, size_t len) {
    if (!conn || conn->fd < 0) return -1;
    // Note: send() might not send the full amount in non-blocking mode; 
    // a real implementation would loop or use a write-buffer queue.
    return send(conn->fd, buf, len, 0);
}

// --- Event Dispatcher ---

// Dispatches a new accepted connection.
static void dispatch_accept(net_tcp_connection* listener) {
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    
    // Loop to handle all pending connections (necessary for Edge-Triggered mode)
    for (;;) {
        int conn_fd = accept(listener->fd, (struct sockaddr*)&cli_addr, &clilen);
        if (conn_fd < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // No more connections to accept
                break;
            }
            perror("accept");
            break;
        }

        // Set accepted socket to non-blocking
        if (set_nonblocking(conn_fd) < 0) {
            close(conn_fd);
            continue;
        }

        // Create new connection object and register it
        net_tcp_connection* new_conn = talloc_type(listener->mem_ctx, net_tcp_connection);
        if (!new_conn) {
            close(conn_fd);
            continue;
        }

        new_conn->mem_ctx = listener->mem_ctx;
        new_conn->fd = conn_fd;
        new_conn->is_listening = 0;
        new_conn->local_addr = listener->local_addr;
        // Remote addr setup would happen here based on cli_addr
        new_conn->remote_addr = NULL; 
        // Initialize handlers to NULL
        new_conn->handler_accept = NULL;
        new_conn->handler_connect_done = NULL;
        new_conn->handler_data_ready = NULL;
        new_conn->handler_write_done = NULL;
        new_conn->handler_close = NULL;

        // Register the new connection for READ events
        if (epoll_register(EPOLL_CTL_ADD, conn_fd, EPOLLIN, new_conn) < 0) {
            // talloc cleanup here
            close(conn_fd);
            continue;
        }

        // Dispatch to the COME-level handler
        if (listener->handler_accept) {
            listener->handler_accept(listener, new_conn);
        }
    }
}

// Dispatches the result of an asynchronous connect operation.
static void dispatch_connect_done(net_tcp_connection* conn) {
    int error = 0;
    socklen_t len = sizeof(error);
    // Check socket error status after connect completes
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        // Connection failed
        if (conn->handler_close) {
            conn->handler_close(conn);
        }
        net_tcp_close(conn);
        return;
    }

    // Connection successful! Now modify epoll to listen for IN events (data).
    // OUT event is no longer needed.
    epoll_register(EPOLL_CTL_MOD, conn->fd, EPOLLIN, conn);
    
    if (conn->handler_connect_done) {
        conn->handler_connect_done(conn);
    }
}

// The core event loop function called repeatedly by the COME runtime/scheduler.
// This is where coroutines would yield and resume.
int net_tcp_run_once(int timeout_ms) {
    if (epoll_fd == -1) return 0; 

    struct epoll_event events[MAX_EVENTS];
    // Wait for events (timeout in milliseconds)
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout_ms);

    if (nfds < 0) {
        if (errno != EINTR) perror("epoll_wait");
        return -1;
    }

    // Process triggered events
    for (int i = 0; i < nfds; i++) {
        net_tcp_connection* conn = (net_tcp_connection*)events[i].data.ptr;
        uint32_t evs = events[i].events;

        // 1. Check for errors (HUP or ERR) first
        if (evs & (EPOLLERR | EPOLLHUP)) {
            if (conn->handler_close) {
                conn->handler_close(conn);
            }
            net_tcp_close(conn);
            continue;
        }

        // 2. Listener Accept Event (New incoming connection)
        if (conn->is_listening && (evs & EPOLLIN)) {
            dispatch_accept(conn);
        }
        
        // 3. Connect Completion Event (Async client connect finished)
        else if (!conn->is_listening && (evs & EPOLLOUT)) {
            dispatch_connect_done(conn);
        }

        // 4. Read Ready Event (Data available to be read)
        else if (!conn->is_listening && (evs & EPOLLIN)) {
            if (conn->handler_data_ready) {
                conn->handler_data_ready(conn);
            }
        }

        // 5. Write Ready Event (Output buffer free space available)
        // A real system would use a write buffer and check if there's data to be written.
        else if (!conn->is_listening && (evs & EPOLLOUT)) {
            if (conn->handler_write_done) {
                conn->handler_write_done(conn);
            }
        }
    }

    return nfds;
}

// --- Event Registration Placeholders (COME compiler interface) ---

void net_tcp_on_accept(net_tcp_connection* listener, void (*handler)(net_tcp_connection*, net_tcp_connection*)) {
    listener->handler_accept = handler;
}

void net_tcp_on_connect_done(net_tcp_connection* conn, void (*handler)(net_tcp_connection*)) {
    conn->handler_connect_done = handler;
}

void net_tcp_on_data_ready(net_tcp_connection* conn, void (*handler)(net_tcp_connection*)) {
    conn->handler_data_ready = handler;
}

void net_tcp_on_write_done(net_tcp_connection* conn, void (*handler)(net_tcp_connection*)) {
    conn->handler_write_done = handler;
}

void net_tcp_on_close(net_tcp_connection* conn, void (*handler)(net_tcp_connection*)) {
    conn->handler_close = handler;
}
