## 📦 `net.http` Module Overview

The `net.http` module provides high-level handling for the HTTP protocol. A new session is created with `net.http.new()`, which yields an object containing both a **request object** (`http.req`) and a **response object** (`http.resp`).

| Method/Field | Type | Description |
| :--- | :--- | :--- |
| **`net.http.new()`** | `func` | Creates and returns a new `net.http.session` object. |
| **`session.attach(conn)`** | `func` | Binds the HTTP session to an underlying **transport connection** (e.g., `net.tcp.connection` or `net.tls.connection`). This starts the message parsing loop. |
| **`session.req`** | `struct` | The message object for the **outgoing** request (client) or **incoming** request (server). |
| **`session.resp`** | `struct` | The message object for the **incoming** response (client) or **outgoing** response (server). |

---

## ⚡ HTTP Message Events (`req` and `resp`)

Events are attached using the universal **`.on(EVENT_NAME, handler_func)`** method. The events are named based on the underlying I/O operation: **`_READY`** for **Incoming (Read)** and **`_DONE`** for **Outgoing (Write)**.

### A. Incoming Message Events (Read $\rightarrow$ READY)

These events fire when data is **received** from the connection and ready to be processed or consumed by the object. They apply to the **Response** object on a client, or the **Request** object on a server.

| Event Name | Applies To | Fired When | Purpose |
| :--- | :--- | :--- | :--- |
| **`LINE_READY`** | `req`, `resp` | The entire **Start Line** (Request Line on `req` or Status Line on `resp`) has been received and parsed. | Allows immediate inspection of the method, path, or status code before headers arrive. |
| **`HEADER_READY`** | `req`, `resp` | All message headers have been received and parsed. | Allows access to headers like `Content-Length` or `Content-Type`. |
| **`DATA_READY`** | `req`, `resp` | A partial chunk of the message body (from a large or chunked transfer) is available to be read from the object's internal buffer. | Essential for streaming and handling large bodies without loading everything into memory. |
| **`READY`** | `req`, `resp` | The **entire message** (headers and full body) has been received, parsed, and is complete. | The object is fully initialized and ready for application logic. |

---

### B. Outgoing Message Events (Write $\rightarrow$ DONE)

These events fire when data has been successfully **written** to the underlying connection's buffer. These are often used for flow control or confirmation. They apply to the **Request** object on a client, or the **Response** object on a server.

| Event Name | Applies To | Fired When | Purpose |
| :--- | :--- | :--- | :--- |
| **`HEADER_DONE`** | `req`, `resp` | The message's headers (including the Start Line) have been fully written to the connection. | Confirmation that the message structure is on the wire. |
| **`DATA_DONE`** | `req`, `resp` | A partial chunk of the message body has been successfully written to the connection's buffer. | Primarily used for flow control: signals that the buffer is ready for more data to be written (similar to a `DRAIN` event). |
| **`DONE`** | `req`, `resp` | The **entire message** (headers and full body) has been written to the connection and the transaction is complete from the object's perspective. | **Final completion** of the outgoing transmission. |

---

## ✍️ Message Methods (`req` and `resp`)

These methods initiate the sending of an HTTP message.

### `http.req` Methods (Client Outgoing / Server Incoming)

| Method | Signature | Client Use (Outgoing) | Server Use (Incoming) |
| :--- | :--- | :--- | :--- |
| **`.send(content)`** | `send(string)` | Sends the request headers and body content to the server. Triggers the outgoing `HEADER_DONE` and `DONE` events. | Not typically used; message flow is incoming. |
| **`.on(EVENT, handler)`** | `on(int, func)` | Attaches an event handler for incoming or outgoing events. | Attaches an event handler for incoming or outgoing events. |

### `http.resp` Methods (Client Incoming / Server Outgoing)

| Method | Signature | Client Use (Incoming) | Server Use (Outgoing) |
| :--- | :--- | :--- | :--- |
| **`.send(content)`** | `send(string)` | Not typically used; message flow is incoming. | Sends the response headers and body content to the client. Triggers the outgoing `HEADER_DONE` and `DONE` events. |
| **`.on(EVENT, handler)`** | `on(int, func)` | Attaches an event handler for incoming or outgoing events. | Attaches an event handler for incoming or outgoing events. |