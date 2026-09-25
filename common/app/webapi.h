/**
 * webapi - minimal HTTP/1.1 server on 127.0.0.1, exposed via the manifest
 * reverseProxy routes so the device handles auth and TLS. Main loop only.
 *
 *   api_port (viewer):   /latest, /history (JSON), /stream (SSE)
 *   config_port (admin): /settings?action=list | action=update&Name=val
 */
#ifndef WEBAPI_H
#define WEBAPI_H

#include <glib.h>
#include <stdbool.h>

/* Called on the main loop thread for each setting written via /settings. */
typedef void (*webapi_apply_cb)(const char *name, const char *value,
                                void *user);

/* The config server always starts; the API only when api_enabled. Secret values
 * are never returned and only updated when non-empty. Always returns
 * TRUE (listen errors are logged). Call after transcript_init(). */
gboolean webapi_start(guint16 api_port, gboolean api_enabled,
                      guint16 config_port, const char *const *setting_names,
                      guint n_settings, const char *const *secret_names,
                      guint n_secret, webapi_apply_cb apply_cb,
                      void *apply_user);

/* Stop the servers and close any streaming clients. */
void webapi_stop(void);

#endif /* WEBAPI_H */
