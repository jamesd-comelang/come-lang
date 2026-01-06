// Filename: src/net/http.c - HTTP Protocol Implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// --- COME Dependencies ---
#include "mem/talloc.h"
#include "llhttp.h"
// We must define a generic connection interface, assuming either TCP or TLS fits it.
// For simplicity here, we assume a generic read/write interface. 
// In a full implementation, this would be an Interface/vtable.

// Placeholder for generic connection interface (can be net_tcp_connection or net_tls_connection)
typedef struct generic_connection {
    void* mem_ctx;
    ssize_t (*read)(struct generic_connection* conn, char* buf, size_t len);
    ssize_t (*write)(struct generic_connection* conn, const char* buf, size_t len);
    void (*close)(struct generic_connection* conn);
} generic_connection;

// --- Struct Definitions ---

// Structs moved to header
// net.http.session_internal
typedef struct net_http_session_internal {
    void* mem_ctx;
    net_http_request* req;
    net_http_response* resp;
    generic_connection* transport;

    // llhttp parser
    llhttp_t parser;
    llhttp_settings_t settings;
    int is_server_side;

    
} net_http_session_internal;

// --- llhttp Callbacks Prototypes ---
int handle_on_message_begin(llhttp_t* parser);
int handle_on_url(llhttp_t* parser, const char* at, size_t length);
int handle_on_header_field(llhttp_t* parser, const char* at, size_t length);
int handle_on_header_value(llhttp_t* parser, const char* at, size_t length);
int handle_on_headers_complete(llhttp_t* parser);
int handle_on_body(llhttp_t* parser, const char* at, size_t length);
int handle_on_message_complete(llhttp_t* parser);

// --- Private Parsing Helpers ---

// --- llhttp Callbacks ---

int handle_on_message_begin(llhttp_t* parser) {
    return 0;
}

int handle_on_url(llhttp_t* parser, const char* at, size_t length) {
    // net_http_session* session = (net_http_session*)parser->data;
    // Store URL/Path
    return 0;
}

int handle_on_header_field(llhttp_t* parser, const char* at, size_t length) {
    return 0;
}

int handle_on_header_value(llhttp_t* parser, const char* at, size_t length) {
    return 0;
}

int handle_on_headers_complete(llhttp_t* parser) {
    net_http_session_internal* session = (net_http_session_internal*)parser->data;
    if (session->is_server_side) {
        if (session->req->handler_header_ready) session->req->handler_header_ready(session->req);
    } else {
        if (session->resp->handler_header_ready) session->resp->handler_header_ready(session->resp);
    }
    return 0;
}

int handle_on_body(llhttp_t* parser, const char* at, size_t length) {
    net_http_session_internal* session = (net_http_session_internal*)parser->data;
    // Append body data
    if (session->is_server_side) {
        if (session->req->handler_data_ready) session->req->handler_data_ready(session->req);
    } else {
        if (session->resp->handler_data_ready) session->resp->handler_data_ready(session->resp);
    }
    return 0;
}

int handle_on_message_complete(llhttp_t* parser) {
    net_http_session_internal* session = (net_http_session_internal*)parser->data;
    if (session->is_server_side) {
        if (session->req->handler_ready) session->req->handler_ready(session->req);
    } else {
        if (session->resp->handler_ready) session->resp->handler_ready(session->resp);
    }
    return 0;
}

// Low-level handler that is called when the transport has data
static void __attribute__((unused)) http_transport_data_ready_handler(generic_connection* transport_conn) {
    net_http_session_internal* session = (net_http_session_internal*)transport_conn->mem_ctx;
    
    // Read data from the transport layer
    char buffer[4096];
    ssize_t bytes_read = transport_conn->read(transport_conn, buffer, sizeof(buffer));

    if (bytes_read > 0) {
        enum llhttp_errno err = llhttp_execute(&session->parser, buffer, bytes_read);
        if (err != HPE_OK) {
            fprintf(stderr, "Parse error: %s %s\n", llhttp_errno_name(err), llhttp_get_error_reason(&session->parser));
            // Handle error (close connection, etc.)
        }
    } else if (bytes_read == 0) {
        // Connection closed gracefully
        // We would finalize the message parsing here.
    } else if (bytes_read < 0) {
        // Error handling
    }
}

// --- Public API Functions ---

// net.http.new()
net_http_session* net_http_new(void* mem_ctx, int is_server_side) {
    net_http_session_internal* session = (net_http_session_internal*)mem_talloc_alloc(mem_ctx, sizeof(net_http_session_internal));
    if (!session) return NULL;

    session->mem_ctx = session;
    session->is_server_side = is_server_side;
    session->transport = NULL;

    // Initialize llhttp
    llhttp_settings_init(&session->settings);
    session->settings.on_message_begin = handle_on_message_begin;
    session->settings.on_url = handle_on_url;
    session->settings.on_header_field = handle_on_header_field;
    session->settings.on_header_value = handle_on_header_value;
    session->settings.on_headers_complete = handle_on_headers_complete;
    session->settings.on_body = handle_on_body;
    session->settings.on_message_complete = handle_on_message_complete;

    llhttp_init(&session->parser, is_server_side ? HTTP_REQUEST : HTTP_RESPONSE, &session->settings);
    session->parser.data = session;

    // Allocate child objects
    session->req = (net_http_request*)mem_talloc_alloc(session, sizeof(net_http_request));
    session->resp = (net_http_response*)mem_talloc_alloc(session, sizeof(net_http_response));

    if (!session->req || !session->resp) { mem_talloc_free(session); return NULL; }

    session->req->mem_ctx = session;
    session->resp->mem_ctx = session;
    // Initialize other fields/handlers to NULL

    return (net_http_session*)session;
}

// Helper for net.http.new() - Defaults to server side
net_http_session* come_net_http_new_default(void* mem_ctx) {
    return net_http_new(mem_ctx, 1);
}

// net.http.attach(conn)
void net_http_attach(net_http_session* session, generic_connection* conn) {
    session->transport = conn;
    // Hook the low-level transport's DATA_READY event to our HTTP parser
    // NOTE: This assumes a generic way to register the handler on the transport layer.
    // In reality, this requires a vtable or a type-check for tcp/tls.
    // Placeholder: conn->on_data_ready(http_transport_data_ready_handler);
}

// net.http.request.send(content)
void net_http_request_send(net_http_request* req, const char* content) {
    // Simplified: format headers + content and send over the transport layer
    net_http_session_internal* session = (net_http_session_internal*)req->mem_ctx;
    if (!session->transport) return;

    // Form the HTTP message (omitted: complex formatting)
    char buffer[2048];
    size_t len = snprintf(buffer, sizeof(buffer), 
        "GET / HTTP/1.1\r\nHost: example.com\r\nContent-Length: %zu\r\n\r\n%s", strlen(content), content);

    session->transport->write(session->transport, buffer, len);
    
    // Trigger Outgoing DONE event
    if (req->handler_done) {
        req->handler_done(req);
    }
}

// net.http.response.send(content) - Similar implementation
void net_http_response_send(net_http_response* resp, const char* content) {
    // ... (Similar simplified writing logic for response) ...
    // Trigger Outgoing DONE event
    if (resp->handler_done) {
        resp->handler_done(resp);
    }
}

// --- Event Registration Placeholders ---
// ... (Multiple functions for attaching all the LINE_READY, DATA_READY, DONE, etc. handlers) ...
