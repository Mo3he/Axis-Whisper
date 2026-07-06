/**
 * mqtt - self-contained MQTT 3.1.1 publisher. See mqtt.h.
 */

#include "mqtt.h"
#include "transcript.h"

#include <gio/gio.h>
#include <pthread.h>
#include <string.h>
#include <syslog.h>

static struct mqtt_config g_cfg;
static GAsyncQueue *g_queue = NULL; /* char* JSON payloads (g_free'd) */
static pthread_t g_tid;
static gboolean g_started = FALSE;
static volatile gint g_running = 0;
static int g_sentinel; /* address used as a wake-up sentinel */

/* --- MQTT packet helpers ------------------------------------------------- */

static void put_remaining_length(GByteArray *pkt, guint len) {
    do {
        guint8 b = len % 128;
        len /= 128;
        if (len > 0)
            b |= 0x80;
        g_byte_array_append(pkt, &b, 1);
    } while (len > 0);
}

static void put_string(GByteArray *pkt, const char *s) {
    guint16 n = (guint16)strlen(s);
    guint8 hdr[2] = {(guint8)(n >> 8), (guint8)(n & 0xFF)};
    g_byte_array_append(pkt, hdr, 2);
    g_byte_array_append(pkt, (const guint8 *)s, n);
}

static gboolean read_n(GInputStream *in, guint8 *buf, gsize n) {
    gsize got = 0;
    while (got < n) {
        gssize r = g_input_stream_read(in, buf + got, n - got, NULL, NULL);
        if (r <= 0)
            return FALSE;
        got += (gsize)r;
    }
    return TRUE;
}

/* Send CONNECT and wait for a successful CONNACK. */
static gboolean mqtt_handshake(GSocketConnection *conn) {
    GByteArray *var = g_byte_array_new();
    put_string(var, "MQTT");
    guint8 level = 0x04; /* 3.1.1 */
    g_byte_array_append(var, &level, 1);

    guint8 flags = 0x02; /* clean session */
    gboolean have_user = g_cfg.user[0] != '\0';
    gboolean have_pass = g_cfg.pass[0] != '\0';
    if (have_user)
        flags |= 0x80;
    if (have_user && have_pass)
        flags |= 0x40;
    g_byte_array_append(var, &flags, 1);
    guint8 keepalive[2] = {0x00, 0x00}; /* 0 = disabled */
    g_byte_array_append(var, keepalive, 2);

    const char *cid = g_cfg.client_id[0] != '\0' ? g_cfg.client_id
                                                  : "axis-whisper";
    put_string(var, cid);
    if (have_user)
        put_string(var, g_cfg.user);
    if (have_user && have_pass)
        put_string(var, g_cfg.pass);

    GByteArray *pkt = g_byte_array_new();
    guint8 type = 0x10; /* CONNECT */
    g_byte_array_append(pkt, &type, 1);
    put_remaining_length(pkt, var->len);
    g_byte_array_append(pkt, var->data, var->len);
    g_byte_array_free(var, TRUE);

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gboolean ok = g_output_stream_write_all(out, pkt->data, pkt->len, NULL,
                                            NULL, NULL);
    g_byte_array_free(pkt, TRUE);
    if (!ok)
        return FALSE;

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    guint8 ack[4];
    if (!read_n(in, ack, 4))
        return FALSE;
    if (ack[0] != 0x20 || ack[3] != 0x00) {
        syslog(LOG_WARNING, "MQTT broker refused connection (code %u)", ack[3]);
        return FALSE;
    }
    return TRUE;
}

/* Publish payload to the configured topic (QoS 0). */
static gboolean mqtt_publish(GSocketConnection *conn, const char *payload) {
    GByteArray *var = g_byte_array_new();
    put_string(var, g_cfg.topic);
    g_byte_array_append(var, (const guint8 *)payload, strlen(payload));

    GByteArray *pkt = g_byte_array_new();
    guint8 type = 0x30; /* PUBLISH, QoS 0 */
    g_byte_array_append(pkt, &type, 1);
    put_remaining_length(pkt, var->len);
    g_byte_array_append(pkt, var->data, var->len);
    g_byte_array_free(var, TRUE);

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gboolean ok = g_output_stream_write_all(out, pkt->data, pkt->len, NULL,
                                            NULL, NULL);
    g_byte_array_free(pkt, TRUE);
    return ok;
}

static GSocketConnection *connect_broker(void) {
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 10);
    if (g_cfg.tls) {
        g_socket_client_set_tls(client, TRUE);
        /* By default accept broker certificates that don't chain to a system
         * CA (self-signed brokers are common on local networks). Set
         * MqttTlsVerify to require a valid certificate chain instead. */
        if (!g_cfg.tls_verify)
            g_socket_client_set_tls_validation_flags(client, 0);
    }
    GError *err = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host(
        client, g_cfg.host, (guint16)g_cfg.port, NULL, &err);
    g_object_unref(client);
    if (conn == NULL) {
        syslog(LOG_WARNING, "MQTT connect to %s:%d failed: %s", g_cfg.host,
               g_cfg.port, err != NULL ? err->message : "unknown");
        g_clear_error(&err);
    }
    return conn;
}

static void *mqtt_thread(void *arg) {
    (void)arg;
    int backoff = 1;
    while (g_atomic_int_get(&g_running)) {
        GSocketConnection *conn = connect_broker();
        if (conn == NULL || !mqtt_handshake(conn)) {
            if (conn != NULL) {
                g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
                g_object_unref(conn);
            }
            for (int i = 0; i < backoff * 10 && g_atomic_int_get(&g_running);
                 i++)
                g_usleep(100 * 1000);
            if (backoff < 30)
                backoff *= 2;
            continue;
        }
        syslog(LOG_INFO, "MQTT connected to %s:%d, publishing to '%s'",
               g_cfg.host, g_cfg.port, g_cfg.topic);
        backoff = 1;

        while (g_atomic_int_get(&g_running)) {
            gpointer msg = g_async_queue_timeout_pop(g_queue, 1000 * 1000);
            if (msg == NULL)
                continue; /* idle tick; re-check running */
            if (msg == &g_sentinel)
                break;
            gboolean ok = mqtt_publish(conn, (const char *)msg);
            g_free(msg);
            if (!ok) {
                syslog(LOG_WARNING, "MQTT publish failed, reconnecting");
                break;
            }
        }

        guint8 disconnect[2] = {0xE0, 0x00};
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        g_output_stream_write_all(out, disconnect, 2, NULL, NULL, NULL);
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
    }
    return NULL;
}

/* transcript subscriber: enqueue a JSON payload (main loop thread). */
static void on_transcript(const char *text,
                          gint64 ts_ms,
                          gboolean is_final,
                          void *user) {
    (void)user;
    if (!is_final && !g_cfg.publish_partials)
        return;
    GString *p = g_string_new("{\"text\":\"");
    transcript_json_escape(p, text);
    g_string_append_printf(p, "\",\"timestampMs\":%" G_GINT64_FORMAT
                              ",\"final\":%s}",
                           ts_ms, is_final ? "true" : "false");
    /* Parenthesize to bypass the GLib 2.76+ macro that would emit a
     * reference to g_string_free_and_steal, which is absent on older
     * firmware GLib. Call the real function so we stay backward compatible. */
    g_async_queue_push(g_queue, (g_string_free)(p, FALSE));
}

void mqtt_start(const struct mqtt_config *cfg) {
    if (cfg == NULL || !cfg->enabled) {
        syslog(LOG_INFO, "MQTT publishing disabled");
        return;
    }
    if (cfg->host[0] == '\0') {
        syslog(LOG_WARNING, "MQTT enabled but no broker host set; disabled");
        return;
    }
    g_cfg = *cfg;
    if (g_cfg.port <= 0)
        g_cfg.port = g_cfg.tls ? 8883 : 1883;
    if (g_cfg.topic[0] == '\0')
        g_strlcpy(g_cfg.topic, "whisper/subtitles", sizeof(g_cfg.topic));

    g_queue = g_async_queue_new();
    g_atomic_int_set(&g_running, 1);
    if (pthread_create(&g_tid, NULL, mqtt_thread, NULL) != 0) {
        syslog(LOG_ERR, "failed to start MQTT thread");
        g_atomic_int_set(&g_running, 0);
        g_async_queue_unref(g_queue);
        g_queue = NULL;
        return;
    }
    g_started = TRUE;
    transcript_subscribe(on_transcript, NULL);
}

void mqtt_stop(void) {
    if (!g_started)
        return;
    g_atomic_int_set(&g_running, 0);
    g_async_queue_push(g_queue, &g_sentinel);
    pthread_join(g_tid, NULL);
    g_started = FALSE;

    /* Drain any leftover payloads. */
    gpointer msg;
    while ((msg = g_async_queue_try_pop(g_queue)) != NULL)
        if (msg != &g_sentinel)
            g_free(msg);
    g_async_queue_unref(g_queue);
    g_queue = NULL;
}
