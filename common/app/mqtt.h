/**
 * mqtt - self-contained MQTT publisher for transcriptions.
 *
 * A minimal MQTT 3.1.1 client (publish only, QoS 0) implemented directly over
 * GLib's GSocketClient, so no external MQTT library is required. It connects
 * to a user-configured broker (optionally over TLS), keeps the connection
 * open, and publishes each finalized transcription (and optionally live
 * partials) as a JSON message to a configurable topic.
 *
 * Networking runs on a dedicated worker thread; the transcript subscriber
 * callback only enqueues payloads, so the audio/main threads never block on
 * the broker. The connection is re-established automatically on failure.
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
