// Filename: src/include/net/tls.h
#ifndef COME_NET_TLS_H
#define COME_NET_TLS_H

#include <sys/types.h>
#include <unistd.h>
#include "net/tcp.h"

// Forward declarations of structs defined in tls.c
// net.tls.context (TLS/SSL configuration)
typedef struct net_tls_context {
    void* mem_ctx;
    void* ssl_ctx; // Use void* to avoid including openssl/ssl.h in header, cast in implementation
    char* cert_file; // For server/client auth
    char* key_file;  // For server/client auth
    int is_server;
} net_tls_context;
typedef struct net_tls_connection net_tls_connection;
typedef struct net_tls_listener net_tls_listener;

// --- Public API Functions ---

// net.tls.context_make(cert, key, is_server)
net_tls_context* net_tls_context_make(void* mem_ctx, const char* cert_file, const char* key_file, int is_server);

// net.tls.listen(addr, ctx)
net_tls_listener* net_tls_listen(void* mem_ctx, net_tcp_addr* addr, net_tls_context* ctx);

// net.tls.connect(addr, ctx)
net_tls_connection* net_tls_connect(void* mem_ctx, net_tcp_addr* addr, net_tls_context* ctx);

// net.tls.read(conn, buffer, len)
ssize_t net_tls_read(net_tls_connection* conn, char* buf, size_t len);

// net.tls.write(conn, buffer, len)
ssize_t net_tls_write(net_tls_connection* conn, const char* buf, size_t len);

// --- Internal/COME Interface Handshake Functions ---
void net_tls_do_handshake(net_tls_connection* conn);

// --- Event Registration Placeholders ---

void net_tls_on_connect(net_tls_connection* conn, void (*handler)(net_tls_connection*));
void net_tls_on_data_ready(net_tls_connection* conn, void (*handler)(net_tls_connection*));
void net_tls_on_accept(net_tls_listener* listener, void (*handler)(net_tls_listener*, net_tls_connection*));

// Helpers
net_tls_listener* come_net_tls_listen_helper(void* mem_ctx, char* ip, int port, net_tls_context ctx_val);
net_tls_connection* net_tls_accept(net_tls_listener* listener);

#endif // COME_NET_TLS_H
