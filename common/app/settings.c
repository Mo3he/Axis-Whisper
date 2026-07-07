/**
 * settings - file-backed settings store. See settings.h.
 */

#include "settings.h"

#include <glib/gstdio.h>
#include <string.h>

#define SETTINGS_GROUP "Whisper_Subtitles"

static GKeyFile *g_kf = NULL;
static gchar *g_path = NULL;
static GMutex g_lock;

void settings_init(const char *path) {
    g_mutex_init(&g_lock);
    g_free(g_path);
    g_path = g_strdup(path);
    if (g_kf != NULL)
        g_key_file_free(g_kf);
    g_kf = g_key_file_new();
    /* A missing or unparsable file is fine: we simply start empty. */
    g_key_file_load_from_file(g_kf, path, G_KEY_FILE_NONE, NULL);
}

gchar *settings_get(const char *name) {
    if (name == NULL)
        return NULL;
    g_mutex_lock(&g_lock);
    gchar *v =
        g_kf != NULL ? g_key_file_get_string(g_kf, SETTINGS_GROUP, name, NULL)
                     : NULL;
    g_mutex_unlock(&g_lock);
    return v;
}

void settings_set(const char *name, const char *value) {
    if (name == NULL)
        return;
    g_mutex_lock(&g_lock);
    if (g_kf != NULL)
        g_key_file_set_string(g_kf, SETTINGS_GROUP, name,
                              value != NULL ? value : "");
    g_mutex_unlock(&g_lock);
}

gboolean settings_save(void) {
    g_mutex_lock(&g_lock);
    gboolean ok = FALSE;
    if (g_kf != NULL && g_path != NULL)
        ok = g_key_file_save_to_file(g_kf, g_path, NULL);
    g_mutex_unlock(&g_lock);
    return ok;
}

static gboolean name_in(const char *name, const char *const *set, guint n) {
    for (guint i = 0; i < n; i++)
        if (g_strcmp0(name, set[i]) == 0)
            return TRUE;
    return FALSE;
}

void settings_to_json(GString *out, const char *const *names, guint n,
                      const char *const *secret, guint n_secret) {
    g_string_append_c(out, '{');
    for (guint i = 0; i < n; i++) {
        /* Never expose secret values to the browser. */
        gchar *v =
            name_in(names[i], secret, n_secret) ? NULL : settings_get(names[i]);
        if (i > 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, '"');
        g_string_append(out, names[i]); /* setting names are JSON-safe */
        g_string_append(out, "\":\"");
        for (const char *p = (v != NULL ? v : ""); *p != '\0'; p++) {
            if (*p == '"' || *p == '\\')
                g_string_append_c(out, '\\');
            if (*p == '\n') {
                g_string_append(out, "\\n");
                continue;
            }
            if ((unsigned char)*p < 0x20)
                continue; /* drop other control chars */
            g_string_append_c(out, *p);
        }
        g_string_append_c(out, '"');
        g_free(v);
    }
    g_string_append_c(out, '}');
}
