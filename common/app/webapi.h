/**
 * webapi - minimal HTTP API exposing transcriptions to other systems.
 *
 * A tiny HTTP/1.1 server (built on GLib's GSocketService) listens on
 * 127.0.0.1 and is exposed through the device's Apache web server via the
 * manifest reverseProxy entry, so external callers reach it at
 *   http://<device>/local/Whisper_Subtitles/api/<endpoint>
 * reusing the device's normal authentication and TLS.
 *
 * Endpoints (GET):
 *   /latest   - the most recent transcript as JSON
 *   /history  - recent finalized transcripts as JSON
 *   /stream   - Server-Sent Events; pushes each new transcript live
 *
 * Runs entirely on the GLib main loop; no extra threads.
 */
#ifndef WEBAPI_H
#define WEBAPI_H

#include <glib.h>
#include <stdbool.h>

/* Start the API server on 127.0.0.1:port. No-op when enabled is FALSE.
 * Returns TRUE on success. Must be called after the GLib main loop exists
 * and after transcript_init(). */
gboolean webapi_start(guint16 port, gboolean enabled);

/* Stop the server and close any streaming clients. */
void webapi_stop(void);

#endif /* WEBAPI_H */
