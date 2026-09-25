/**
 * mqtt - minimal MQTT 3.1.1 publisher (QoS 0, optional TLS) over GLib sockets.
 *
 * A worker thread owns the connection and reconnects on failure; the transcript
 * callback only enqueues JSON payloads, so the main loop never blocks.
 */
#ifndef MQTT_H
#define MQTT_H

#include <glib.h>
#include <stdbool.h>

struct mqtt_config {
    gboolean enabled;
    char host[128];
    int port;
    gboolean tls;              /* connect over TLS */
    gboolean tls_verify;       /* validate the broker certificate chain */
    char user[128];            /* empty = no username */
    char pass[128];
    char topic[192];
    char client_id[128];       /* empty = auto */
    gboolean publish_partials; /* also publish live partial captions */
};

/* Start publishing. Copies cfg. No-op when cfg->enabled is FALSE.
 * Must be called after transcript_init(). */
void mqtt_start(const struct mqtt_config *cfg);

/* Stop the worker thread and disconnect. */
void mqtt_stop(void);

#endif /* MQTT_H */
