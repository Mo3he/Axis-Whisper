/**
 * transcript - shared subtitle/transcription store and pub/sub.
 *
 * The transcription thread publishes every subtitle update here (live
 * partials and finalized phrases). Consumers - the HTTP API (webapi) and the
 * MQTT publisher (mqtt) - subscribe to receive them, and the API also polls
 * the retained "latest" value and recent history.
 *
 * Publishing may happen on the transcription thread; subscriber callbacks are
 * always invoked on the GLib main loop thread (via g_idle), so consumers do
 * not need their own locking against the audio pipeline.
 */
#ifndef TRANSCRIPT_H
#define TRANSCRIPT_H

#include <glib.h>
#include <stdbool.h>

/* Invoked (on the main loop thread) for every published transcript.
 * is_final is FALSE for a live partial caption, TRUE for a completed phrase. */
typedef void (*transcript_cb)(const char *text,
                              gint64 ts_ms,
                              gboolean is_final,
                              void *user);

/* Initialise the store. Call once before any publish/subscribe. */
void transcript_init(void);

/* Register a subscriber. Call during startup, before the transcription
 * thread starts; subscribers are never removed. */
void transcript_subscribe(transcript_cb cb, void *user);

/* Publish a subtitle update. Safe to call from any thread. */
void transcript_publish(const char *text, gboolean is_final);

/* Append a JSON object for the most recent transcript to out:
 * {"text":"...","timestampMs":N,"final":true}. Thread-safe. */
void transcript_latest_json(GString *out);

/* Append a JSON object with recent finalized transcripts to out:
 * {"transcripts":[{"text":"...","timestampMs":N}, ...]}. Thread-safe. */
void transcript_history_json(GString *out);

/* Append s to out with JSON string-escaping (no surrounding quotes). */
void transcript_json_escape(GString *out, const char *s);

#endif /* TRANSCRIPT_H */
