// Filename: src/net/tls.c - TLS implementation with OpenSSL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// --- COME Dependencies ---
#include "mem/talloc.h"
#include "net/tcp.h" // Assumed header for net.tcp.c

// --- OpenSSL Dependencies ---
#include <openssl/ssl.h>
#include <openssl/err.h>

// --- Struct Definitions ---

// net.tls.context (TLS/SSL configuration)
// net.tls.context defined in header

// net.tls.connection (A secure connection)
typedef struct net_tls_connection {
    void* mem_ctx;
    net_tcp_connection* tcp_conn; // The underlying TCP connection
    SSL* ssl;
    net_tls_context* tls_ctx;
    int handshake_done;

    // Event Handlers (Mirroring the COME event model)
    void (*handler_connect)(struct net_tls_connection* conn); // Corresponds to CONNECT event (Handshake completion)
    void (*handler_data_ready)(struct net_tls_connection* conn); // Corresponds to DATA_READY
    void (*handler_write_done)(struct net_tls_connection* conn); // Corresponds to DATA_DONE
    void (*handler_close)(struct net_tls_connection* conn);

    // Note: Accept is handled by net_tls_listener

} net_tls_connection;

// net.tls.listener
typedef struct net_tls_listener {
    void* mem_ctx;
    net_tcp_connection* tcp_listener; // Underlying TCP listener
    net_tls_context* tls_ctx;
    
    // Event Handler
    void (*handler_accept)(struct net_tls_listener* listener, net_tls_connection* new_conn);
    
    // Pending connection for synchronous accept via callback
    net_tls_connection* pending_conn;
} net_tls_listener;

// --- OpenSSL and Error Handling Helpers ---

static void tls_handle_error(net_tls_connection* conn, int ret) {
    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // Expected non-blocking behavior. Wait for next epoll event.
        return;
    } else {
        // A real implementation would log the error and free the connection
        if (conn->handler_close) conn->handler_close(conn);
    }
}

// --- Public API Functions ---

// net.tls.context_make(cert, key, is_server)
net_tls_context* net_tls_context_make(void* mem_ctx, const char* cert_file, const char* key_file, int is_server) {
    // Initialize OpenSSL globally (omitted for brevity, assume caller handles)

    net_tls_context* ctx = mem_talloc_alloc(mem_ctx, sizeof(net_tls_context));
    if (!ctx) return NULL;

    ctx->mem_ctx = mem_ctx;
    ctx->is_server = is_server;
    
    const SSL_METHOD* method = is_server ? TLS_server_method() : TLS_client_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) { mem_talloc_free(ctx); return NULL; }

    // Simplified loading of cert/key - only for server/client auth scenarios
    if (cert_file && key_file) {
        SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_file, SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_file, SSL_FILETYPE_PEM);
    }

    return ctx;
}

// --- I/O Callbacks (The bridge between TCP and OpenSSL) ---

// Wrapper for OpenSSL read
ssize_t net_tls_read(net_tls_connection* conn, char* buf, size_t len) {
    int ret = SSL_read(conn->ssl, buf, len);
    if (ret <= 0) {
        tls_handle_error(conn, ret);
        return ret;
    }
    return ret;
}

// Wrapper for OpenSSL write
ssize_t net_tls_write(net_tls_connection* conn, const char* buf, size_t len) {
    int ret = SSL_write(conn->ssl, buf, len);
    if (ret <= 0) {
        tls_handle_error(conn, ret);
        return ret;
    }
    return ret;
}

// --- Connection Handlers (The glue logic) ---

// TLS Handshake Logic (called when TCP is connected or has data)
void net_tls_do_handshake(net_tls_connection* conn) {
    if (conn->handshake_done) return; 
    
    int ret = SSL_accept(conn->ssl); // Works for client (SSL_connect) and server (SSL_accept) depending on SSL object setup
    if (ret <= 0) {
        int err = SSL_get_error(conn->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // Handshake not complete, wait for the next TCP event.
            return; 
        }
        // Failure
        if (conn->handler_close) conn->handler_close(conn);
        return;
    }
    
    // Success
    conn->handshake_done = 1;
    if (conn->handler_connect) {
        conn->handler_connect(conn);
    }
}

// General TCP data event handler - now triggers handshake or decrypted data event
static void tls_tcp_data_ready_handler(net_tcp_connection* tcp_conn) {
    net_tls_connection* tls_conn = (net_tls_connection*)tcp_conn->mem_ctx; // Simplified talloc context retrieval

    if (!tls_conn->handshake_done) {
        net_tls_do_handshake(tls_conn); // Continue handshake
    } else {
        if (tls_conn->handler_data_ready) {
            tls_conn->handler_data_ready(tls_conn); // Dispatch decrypted data event
        }
    }
}

// General TCP connection event handler - now triggers TLS handshake
static void tls_tcp_connect_done_handler(net_tcp_connection* tcp_conn) {
    net_tls_connection* tls_conn = (net_tls_connection*)tcp_conn->mem_ctx;
    
    // Connection is established, start/continue the TLS handshake
    net_tls_do_handshake(tls_conn);
}

// New connection accepted by the TCP listener
static void tls_tcp_accept_handler(net_tcp_connection* tcp_listener, net_tcp_connection* new_tcp_conn) {
    net_tls_listener* tls_listener = (net_tls_listener*)tcp_listener->mem_ctx;
    
    // 1. Create the new TLS connection object
    net_tls_connection* new_tls_conn = mem_talloc_alloc(tls_listener->mem_ctx, sizeof(net_tls_connection));
    if (!new_tls_conn) { net_tcp_close(new_tcp_conn); return; }
    
    new_tls_conn->mem_ctx = new_tls_conn; // Self-referencing context
    new_tls_conn->tcp_conn = new_tcp_conn;
    new_tls_conn->tls_ctx = tls_listener->tls_ctx;
    new_tls_conn->handshake_done = 0;
    new_tls_conn->ssl = SSL_new(tls_listener->tls_ctx->ssl_ctx);
    SSL_set_fd(new_tls_conn->ssl, new_tcp_conn->fd);
    SSL_set_accept_state(new_tls_conn->ssl);

    // 2. Set up event forwarding from TCP to TLS
    net_tcp_on_data_ready(new_tcp_conn, tls_tcp_data_ready_handler);
    net_tcp_on_write_done(new_tcp_conn, NULL); // Simplified: Assume TLS layer handles flow control

    // 3. Dispatch the high-level COME accept event
    if (tls_listener->handler_accept) {
        tls_listener->pending_conn = new_tls_conn;
        tls_listener->handler_accept(tls_listener, new_tls_conn);
        tls_listener->pending_conn = NULL;
    }
    
    // The COME user's handler should call net_tls_do_handshake() or it will be done on first data read.
    // Here, we automatically trigger the first handshake attempt.
    net_tls_do_handshake(new_tls_conn);
}

// net.tls.listen(addr, ctx)
net_tls_listener* net_tls_listen(void* mem_ctx, net_tcp_addr* addr, net_tls_context* ctx) {
    net_tls_listener* listener = mem_talloc_alloc(mem_ctx, sizeof(net_tls_listener));
    if (!listener) return NULL;

    listener->mem_ctx = listener; // Self-referencing context for talloc children
    listener->tls_ctx = ctx;

    // Create and configure the underlying TCP listener
    net_tcp_connection* tcp_listener = net_tcp_listen(listener, addr); // listener as talloc parent
    if (!tcp_listener) { mem_talloc_free(listener); return NULL; }
    
    listener->tcp_listener = tcp_listener;

    // Hook the TCP ACCEPT event to our TLS handler
    net_tcp_on_accept(tcp_listener, tls_tcp_accept_handler);

    return listener;
}

// net.tls.connect(addr, ctx)
net_tls_connection* net_tls_connect(void* mem_ctx, net_tcp_addr* addr, net_tls_context* ctx) {
    net_tls_connection* conn = mem_talloc_alloc(mem_ctx, sizeof(net_tls_connection));
    if (!conn) return NULL;

    conn->mem_ctx = conn; // Self-referencing context
    conn->tls_ctx = ctx;
    conn->handshake_done = 0;

    // Create the underlying TCP connection
    net_tcp_connection* tcp_conn = net_tcp_connect(conn, addr); // conn as talloc parent
    if (!tcp_conn) { mem_talloc_free(conn); return NULL; }

    conn->tcp_conn = tcp_conn;
    conn->ssl = SSL_new(ctx->ssl_ctx);
    SSL_set_fd(conn->ssl, tcp_conn->fd);
    SSL_set_connect_state(conn->ssl); // Set as client

    // Hook TCP events to our TLS handlers
    net_tcp_on_connect_done(tcp_conn, tls_tcp_connect_done_handler);
    net_tcp_on_data_ready(tcp_conn, tls_tcp_data_ready_handler);
    net_tcp_on_write_done(tcp_conn, NULL); // Simplified
    
    // Handshake starts immediately or on TCP connection completion

    return conn;
}

// --- Event Registration Placeholders ---

void net_tls_on_connect(net_tls_connection* conn, void (*handler)(net_tls_connection*)) {
    conn->handler_connect = handler;
}

void net_tls_on_data_ready(net_tls_connection* conn, void (*handler)(net_tls_connection*)) {
    conn->handler_data_ready = handler;
}

void net_tls_on_accept(net_tls_listener* listener, void (*handler)(net_tls_listener*, net_tls_connection*)) {
    listener->handler_accept = handler;
}

// --- Helpers for COME Interop ---

// Helper for net.tls.listen("ip", port, ctx_val)
// Takes context by value to match COME semantics, copies to heap for persistence.
net_tls_listener* come_net_tls_listen_helper(void* mem_ctx, char* ip, int port, net_tls_context ctx_val) {
    // Determine context for allocations (listener will own everything usually)
    // Create new address
    net_tcp_addr* addr = net_tcp_addr_make(ip, (uint16_t)port);
    if (!addr) return NULL;

    // Persist the context
    // We allocate a new context on the heap, attached to mem_ctx (or we will attach to listener later)
    net_tls_context* ctx_ptr = mem_talloc_alloc(mem_ctx, sizeof(net_tls_context));
    if (!ctx_ptr) return NULL;
    *ctx_ptr = ctx_val;
    // Note: Shallow copy of fields (cert_file strings). Assuming they are static or managed elsewhere.
    // If they are local strings in COME, they might die. But COME strings usually talloc'd.
    
    // Delegate to real listen
    net_tls_listener* l = net_tls_listen(mem_ctx, addr, ctx_ptr);
    // Address likely talloc'd on mem_ctx/tcp module context. 
    // Ideally net_tls_listen should take ownership or we rely on it being valid.
    
    return l;
}

// Helper for listener.accept() - Retrieves the connection being processed in the callback
net_tls_connection* net_tls_accept(net_tls_listener* listener) {
    if (!listener) return NULL;
    return listener->pending_conn;
}
