/**
 * webapi - minimal HTTP API exposing transcriptions. See webapi.h.
 */

#include "webapi.h"
#include "transcript.h"

#include <gio/gio.h>
#include <string.h>
#include <syslog.h>

/* A connected Server-Sent-Events client we push transcripts to. */
struct sse_client {
    GSocketConnection *conn;
    GOutputStream *out;
};

static GSocketService *g_service = NULL;
static GList *g_clients = NULL; /* struct sse_client* */
static guint g_heartbeat_id = 0;

/* Per-request parsing context. */
struct req_ctx {
    GSocketConnection *conn;
    GDataInputStream *din;
};

static void client_free(struct sse_client *c) {
    if (c == NULL)
        return;
    g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
    g_object_unref(c->conn);
    g_free(c);
}

/* Best-effort blocking write of a small buffer. Returns FALSE on error. */
static gboolean write_all(GOutputStream *out, const char *data, gsize len) {
    return g_output_stream_write_all(out, data, len, NULL, NULL, NULL);
}

static void send_simple(GSocketConnection *conn,
                        const char *status,
                        const char *content_type,
                        const char *body) {
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gsize blen = body != NULL ? strlen(body) : 0;
    char *hdr = g_strdup_printf("HTTP/1.1 %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %zu\r\n"
                                "Access-Control-Allow-Origin: *\r\n"
                                "Cache-Control: no-store\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                status, content_type, blen);
    write_all(out, hdr, strlen(hdr));
    if (blen > 0)
        write_all(out, body, blen);
    g_free(hdr);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
}

/* Push one transcript to every connected SSE client (main loop thread). */
static void sse_broadcast(const char *text,
                          gint64 ts_ms,
                          gboolean is_final,
                          void *user) {
    (void)user;
    if (g_clients == NULL)
        return;
    GString *ev = g_string_new("data: {\"text\":\"");
    transcript_json_escape(ev, text);
    g_string_append_printf(ev, "\",\"timestampMs\":%" G_GINT64_FORMAT
                               ",\"final\":%s}\n\n",
                           ts_ms, is_final ? "true" : "false");

    GList *dead = NULL;
    for (GList *l = g_clients; l != NULL; l = l->next) {
        struct sse_client *c = l->data;
        if (!write_all(c->out, ev->str, ev->len))
            dead = g_list_prepend(dead, c);
    }
    for (GList *l = dead; l != NULL; l = l->next) {
        g_clients = g_list_remove(g_clients, l->data);
        client_free(l->data);
    }
    g_list_free(dead);
    g_string_free(ev, TRUE);
}

/* Periodic SSE comment so dead connections are detected and reaped. */
static gboolean heartbeat_cb(gpointer user) {
    (void)user;
    GList *dead = NULL;
    for (GList *l = g_clients; l != NULL; l = l->next) {
        struct sse_client *c = l->data;
        if (!write_all(c->out, ":\n\n", 3))
            dead = g_list_prepend(dead, c);
    }
    for (GList *l = dead; l != NULL; l = l->next) {
        g_clients = g_list_remove(g_clients, l->data);
        client_free(l->data);
    }
    g_list_free(dead);
    return G_SOURCE_CONTINUE;
}

static void req_ctx_free(struct req_ctx *ctx, gboolean close_conn) {
    if (ctx == NULL)
        return;
    if (close_conn)
        g_io_stream_close(G_IO_STREAM(ctx->conn), NULL, NULL);
    if (ctx->din != NULL)
        g_object_unref(ctx->din);
    if (ctx->conn != NULL)
        g_object_unref(ctx->conn);
    g_free(ctx);
}

static void on_request_line(GObject *source, GAsyncResult *res, gpointer user) {
    (void)source;
    struct req_ctx *ctx = user;
    GError *err = NULL;
    gsize len = 0;
    char *line =
        g_data_input_stream_read_line_finish(ctx->din, res, &len, &err);
    if (line == NULL) {
        if (err != NULL)
            g_clear_error(&err);
        req_ctx_free(ctx, TRUE);
        return;
    }

    /* Parse "GET <path> HTTP/1.1"; only the path matters. Routing is by
     * suffix so it works regardless of how the reverse proxy rewrites the
     * path prefix. */
    char *path = NULL;
    if (strncmp(line, "GET ", 4) == 0) {
        path = line + 4;
        char *sp = strchr(path, ' ');
        if (sp != NULL)
            *sp = '\0';
    }

    if (path == NULL) {
        send_simple(ctx->conn, "405 Method Not Allowed", "text/plain",
                    "only GET is supported\n");
        g_free(line);
        req_ctx_free(ctx, FALSE);
        return;
    }

    /* Strip any query string. */
    char *q = strchr(path, '?');
    if (q != NULL)
        *q = '\0';

    if (g_str_has_suffix(path, "/stream")) {
        GOutputStream *out =
            g_io_stream_get_output_stream(G_IO_STREAM(ctx->conn));
        const char *hdr = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";
        if (write_all(out, hdr, strlen(hdr))) {
            struct sse_client *c = g_new0(struct sse_client, 1);
            c->conn = g_object_ref(ctx->conn);
            c->out = out;
            g_clients = g_list_prepend(g_clients, c);
            /* Prime the stream with the current transcript. */
            GString *ev = g_string_new("data: ");
            transcript_latest_json(ev);
            g_string_append(ev, "\n\n");
            if (!write_all(out, ev->str, ev->len)) {
                g_clients = g_list_remove(g_clients, c);
                client_free(c);
            }
            g_string_free(ev, TRUE);
        }
        g_free(line);
        req_ctx_free(ctx, FALSE); /* connection kept open for SSE */
        return;
    }

    if (g_str_has_suffix(path, "/latest")) {
        GString *body = g_string_new(NULL);
        transcript_latest_json(body);
        send_simple(ctx->conn, "200 OK", "application/json", body->str);
        g_string_free(body, TRUE);
    } else if (g_str_has_suffix(path, "/history")) {
        GString *body = g_string_new(NULL);
        transcript_history_json(body);
        send_simple(ctx->conn, "200 OK", "application/json", body->str);
        g_string_free(body, TRUE);
    } else if (g_str_has_suffix(path, "/api") || g_str_has_suffix(path, "/") ||
               g_str_has_suffix(path, "/api/")) {
        send_simple(ctx->conn, "200 OK", "application/json",
                    "{\"endpoints\":[\"latest\",\"history\",\"stream\"]}");
    } else {
        send_simple(ctx->conn, "404 Not Found", "application/json",
                    "{\"error\":\"not found\"}");
    }

    g_free(line);
    req_ctx_free(ctx, FALSE);
}

static gboolean on_incoming(GSocketService *service,
                            GSocketConnection *connection,
                            GObject *source_object,
                            gpointer user_data) {
    (void)service;
    (void)source_object;
    (void)user_data;

    struct req_ctx *ctx = g_new0(struct req_ctx, 1);
    ctx->conn = g_object_ref(connection);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    ctx->din = g_data_input_stream_new(in);
    g_data_input_stream_set_newline_type(ctx->din,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    g_data_input_stream_read_line_async(ctx->din, G_PRIORITY_DEFAULT, NULL,
                                        on_request_line, ctx);
    return TRUE; /* handled */
}

gboolean webapi_start(guint16 port, gboolean enabled) {
    if (!enabled) {
        syslog(LOG_INFO, "transcription HTTP API disabled");
        return TRUE;
    }

    GError *err = NULL;
    g_service = g_socket_service_new();

    GInetAddress *addr = g_inet_address_new_from_string("127.0.0.1");
    GSocketAddress *saddr = g_inet_socket_address_new(addr, port);
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(g_service), saddr, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, NULL, NULL, &err);
    g_object_unref(saddr);
    g_object_unref(addr);
    if (!ok) {
        syslog(LOG_ERR, "HTTP API: cannot listen on 127.0.0.1:%u: %s", port,
               err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        g_clear_object(&g_service);
        return FALSE;
    }

    g_signal_connect(g_service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(g_service);
    transcript_subscribe(sse_broadcast, NULL);
    g_heartbeat_id = g_timeout_add_seconds(15, heartbeat_cb, NULL);
    syslog(LOG_INFO, "transcription HTTP API listening on 127.0.0.1:%u", port);
    return TRUE;
}

void webapi_stop(void) {
    if (g_heartbeat_id != 0) {
        g_source_remove(g_heartbeat_id);
        g_heartbeat_id = 0;
    }
    if (g_service != NULL) {
        g_socket_service_stop(g_service);
        g_clear_object(&g_service);
    }
    g_list_free_full(g_clients, (GDestroyNotify)client_free);
    g_clients = NULL;
}
