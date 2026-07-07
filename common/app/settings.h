/**
 * settings - file-backed settings store.
 *
 * Whisper's settings are kept in an INI file (GKeyFile) in the app's writable
 * localdata directory, so the settings page works on devices that do not serve
 * the VAPIX parameter system / param.cgi (e.g. recorders such as the AXIS
 * S3008). The device parameter system (axparameter) is still consulted as a
 * fallback where present, but this file is the source of truth for the app's
 * own Settings web page.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <glib.h>

/* Load settings from path. A missing file is fine (starts empty). Call once
 * at startup before any other settings_* call. */
void settings_init(const char *path);

/* Return the stored value for name, or NULL if absent. Caller frees. */
gchar *settings_get(const char *name);

/* Store name=value in memory. Call settings_save() to persist. Thread-safe. */
void settings_set(const char *name, const char *value);

/* Persist the in-memory settings to the file. Returns FALSE on error. */
gboolean settings_save(void);

/* Append a JSON object {"Name":"value",...} for the given names to out.
 * Any name also present in secret[] is emitted with an empty value, so
 * passwords are never returned to the browser. */
void settings_to_json(GString *out, const char *const *names, guint n,
                      const char *const *secret, guint n_secret);

#endif /* SETTINGS_H */
