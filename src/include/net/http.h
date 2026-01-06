// Filename: src/include/net/http.h
#ifndef COME_NET_HTTP_H
#define COME_NET_HTTP_H

#include <sys/types.h>
#include <unistd.h>

// Forward declarations
typedef struct generic_connection generic_connection;
typedef struct net_http_session net_http_session;

// HTTP Message Headers (Simplified)
typedef struct http_headers {
    void* mem_ctx;
    // In a real implementation, this would be a hash map or linked list
    char* host;
    int content_length;
} http_headers;

// net.http.request (req)
typedef struct net_http_request {
    void* mem_ctx;
    char* method; // GET, POST, etc.
    char* path;
    char* version;
    http_headers* headers;
    char* body; // Simplified: holds the full body once READY
    
    // Event Handlers (Incoming and Outgoing)
    void (*handler_line_ready)(struct net_http_request* req);
    void (*handler_header_ready)(struct net_http_request* req);
    void (*handler_data_ready)(struct net_http_request* req);
    void (*handler_ready)(struct net_http_request* req); // Incoming READY
    void (*handler_done)(struct net_http_request* req); // Outgoing DONE

} net_http_request;

// net.http.response (resp)
typedef struct net_http_response {
    void* mem_ctx;
    int status_code;
    char* status_text;
    char* version;
    http_headers* headers;
    char* body; // Simplified: holds the full body once READY
    
    // Event Handlers (Incoming and Outgoing)
    void (*handler_line_ready)(struct net_http_response* resp);
    void (*handler_header_ready)(struct net_http_response* resp);
    void (*handler_data_ready)(struct net_http_response* resp);
    void (*handler_ready)(struct net_http_response* resp); // Incoming READY
    void (*handler_done)(struct net_http_response* resp); // Outgoing DONE

} net_http_response;

// net.http.session (Public View)
// Matches layout of internal struct up to transport
struct net_http_session {
    void* mem_ctx;
    net_http_request* req;
    net_http_response* resp;
    generic_connection* transport;
    // Opaque storage for implementation specific data (llhttp, logic)
    // We assume public usage only accesses the fields above.
    // If strict compliance is needed, we'd use a void* implementation pointer everywhere,
    // but code generation assumes direct access.
    char opaque_impl[512]; // Buffer to ensure size sort of matches if copied (but mostly for pointer access)
};

// --- Public API Functions ---

// net.http.new()
net_http_session* net_http_new(void* mem_ctx, int is_server_side);
// Helper
net_http_session* come_net_http_new_default(void* mem_ctx);

// net.http.attach(conn) - Accepts a pointer to a generic connection (e.g., net_tcp_connection or net_tls_connection)
void net_http_attach(net_http_session* session, generic_connection* conn);

// net.http.request.send(content)
void net_http_request_send(net_http_request* req, const char* content);

// net.http.response.send(content)
void net_http_response_send(net_http_response* resp, const char* content);

// --- Event Registration Placeholders (Incoming READ -> READY) ---

// For Request object
void net_http_req_on_line_ready(net_http_request* req, void (*handler)(net_http_request*));
void net_http_req_on_header_ready(net_http_request* req, void (*handler)(net_http_request*));
void net_http_req_on_data_ready(net_http_request* req, void (*handler)(net_http_request*));
void net_http_req_on_ready(net_http_request* req, void (*handler)(net_http_request*));

// For Response object
void net_http_resp_on_line_ready(net_http_response* resp, void (*handler)(net_http_response*));
void net_http_resp_on_header_ready(net_http_response* resp, void (*handler)(net_http_response*));
void net_http_resp_on_data_ready(net_http_response* resp, void (*handler)(net_http_response*));
void net_http_resp_on_ready(net_http_response* resp, void (*handler)(net_http_response*));

// --- Event Registration Placeholders (Outgoing WRITE -> DONE) ---

// For Request object
void net_http_req_on_header_done(net_http_request* req, void (*handler)(net_http_request*));
void net_http_req_on_data_done(net_http_request* req, void (*handler)(net_http_request*));
void net_http_req_on_done(net_http_request* req, void (*handler)(net_http_request*));

// For Response object
void net_http_resp_on_header_done(net_http_response* resp, void (*handler)(net_http_response*));
void net_http_resp_on_data_done(net_http_response* resp, void (*handler)(net_http_response*));
void net_http_resp_on_done(net_http_response* resp, void (*handler)(net_http_response*));

#endif // COME_NET_HTTP_H
