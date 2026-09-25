/**
 * settings - GKeyFile settings store in the app's localdata directory.
 *
 * Source of truth for the Settings page so it works on devices without
 * param.cgi (e.g. AXIS S3008); axparameter is only a fallback.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <glib.h>

/* Load settings from path; a missing file starts empty. Call once first. */
void settings_init(const char *path);

/* Return the stored value for name, or NULL if absent. Caller frees. */
gchar *settings_get(const char *name);

/* Store name=value in memory. Call settings_save() to persist. Thread-safe. */
void settings_set(const char *name, const char *value);

/* Persist the in-memory settings to the file. Returns FALSE on error. */
gboolean settings_save(void);

/* Append {"Name":"value",...} to out; names in secret[] get an empty value. */
void settings_to_json(GString *out, const char *const *names, guint n,
                      const char *const *secret, guint n_secret);

#endif /* SETTINGS_H */
