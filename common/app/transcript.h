/**
 * transcript - shared subtitle/transcription store and pub/sub.
 *
 * Publish from any thread; subscribers (webapi, mqtt) are always called on the
 * GLib main loop thread via g_idle, so they need no locking of their own.
 */
#ifndef TRANSCRIPT_H
#define TRANSCRIPT_H

#include <glib.h>
#include <stdbool.h>

/* Main loop thread. is_final is FALSE for a live partial caption. */
typedef void (*transcript_cb)(const char *text,
                              gint64 ts_ms,
                              gboolean is_final,
                              void *user);

/* Initialise the store. Call once before any publish/subscribe. */
void transcript_init(void);

/* Call during startup, before the transcription thread; never removed. */
void transcript_subscribe(transcript_cb cb, void *user);

/* Publish a subtitle update. Safe to call from any thread. */
void transcript_publish(const char *text, gboolean is_final);

/* Append {"text":"...","timestampMs":N,"final":true} to out. Thread-safe. */
void transcript_latest_json(GString *out);

/* Append {"transcripts":[{"text":"...","timestampMs":N},...]} of recent
 * finals to out. Thread-safe. */
void transcript_history_json(GString *out);

/* Append s to out with JSON string-escaping (no surrounding quotes). */
void transcript_json_escape(GString *out, const char *s);

#endif /* TRANSCRIPT_H */
