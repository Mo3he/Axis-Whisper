/**
 * webapi - minimal HTTP API exposing transcriptions and app settings.
 *
 * A tiny HTTP/1.1 server (built on GLib's GSocketService) listens on
 * 127.0.0.1 and is exposed through the device web server via the manifest
 * reverseProxy entries, so callers reach it reusing the device's normal
 * authentication and TLS.
 *
 * Two localhost ports are used so the device can enforce different access
 * levels per reverseProxy route:
 *   - api_port    (viewer)  transcription endpoints:
 *       /latest   - the most recent transcript as JSON
 *       /history  - recent finalized transcripts as JSON
 *       /stream   - Server-Sent Events; pushes each new transcript live
 *   - config_port (admin)   settings endpoint (works on devices without the
 *       VAPIX parameter system, e.g. recorders like the S3008):
 *       /settings?action=list             - all settings as a JSON object
 *       /settings?action=update&Name=val  - store settings, apply live ones
 *
 * Runs entirely on the GLib main loop; no extra threads.
 */
#ifndef WEBAPI_H
#define WEBAPI_H

#include <glib.h>
#include <stdbool.h>

/* Called (on the main loop thread) for each setting written via the config
 * endpoint, so the app can apply live-tunable settings immediately. */
typedef void (*webapi_apply_cb)(const char *name, const char *value,
                                void *user);

/* Start the servers. The transcription API on api_port is a no-op when
 * api_enabled is FALSE; the config server on config_port always starts so the
 * Settings page works regardless. setting_names/n_settings is the set of names
 * returned by the list action; secret_names/n_secret marks password settings
 * that are never returned and only updated when a non-empty value is sent.
 * Returns TRUE on success. Must be called after the GLib main loop exists and
 * after transcript_init(). */
gboolean webapi_start(guint16 api_port, gboolean api_enabled,
                      guint16 config_port, const char *const *setting_names,
                      guint n_settings, const char *const *secret_names,
                      guint n_secret, webapi_apply_cb apply_cb,
                      void *apply_user);

/* Stop the servers and close any streaming clients. */
void webapi_stop(void);

#endif /* WEBAPI_H */
