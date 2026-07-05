/**
 * transcript - shared subtitle/transcription store and pub/sub.
 * See transcript.h for the interface contract.
 */

#include "transcript.h"

#include <pthread.h>
#include <string.h>

#define TRANSCRIPT_TEXT_MAX 512
#define TRANSCRIPT_HISTORY 50
#define TRANSCRIPT_MAX_SUBS 8

struct entry {
    char text[TRANSCRIPT_TEXT_MAX];
    gint64 ts_ms;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Most recent update (partial or final). */
static struct entry g_latest = {"", 0};
static gboolean g_latest_final = FALSE;

/* Ring of finalized transcripts, oldest-to-newest on read. */
static struct entry g_history[TRANSCRIPT_HISTORY];
static int g_history_count = 0;
static int g_history_head = 0; /* index of next slot to write */

/* Subscribers (set up before threads start; read-only afterwards). */
static struct {
    transcript_cb cb;
    void *user;
} g_subs[TRANSCRIPT_MAX_SUBS];
static int g_sub_count = 0;

void transcript_init(void) {
    pthread_mutex_lock(&g_lock);
    g_latest.text[0] = '\0';
    g_latest.ts_ms = 0;
    g_latest_final = FALSE;
    g_history_count = 0;
    g_history_head = 0;
    pthread_mutex_unlock(&g_lock);
}

void transcript_subscribe(transcript_cb cb, void *user) {
    if (cb == NULL || g_sub_count >= TRANSCRIPT_MAX_SUBS)
        return;
    g_subs[g_sub_count].cb = cb;
    g_subs[g_sub_count].user = user;
    g_sub_count++;
}

struct broadcast {
    char text[TRANSCRIPT_TEXT_MAX];
    gint64 ts_ms;
    gboolean is_final;
};

/* Runs on the main loop thread: fan out to subscribers. */
static gboolean broadcast_idle(gpointer data) {
    struct broadcast *b = data;
    for (int i = 0; i < g_sub_count; i++)
        g_subs[i].cb(b->text, b->ts_ms, b->is_final, g_subs[i].user);
    g_free(b);
    return G_SOURCE_REMOVE;
}

void transcript_publish(const char *text, gboolean is_final) {
    if (text == NULL)
        text = "";
    gint64 ts = g_get_real_time() / 1000; /* ms since epoch */

    pthread_mutex_lock(&g_lock);
    g_strlcpy(g_latest.text, text, sizeof(g_latest.text));
    g_latest.ts_ms = ts;
    g_latest_final = is_final;
    if (is_final && text[0] != '\0') {
        struct entry *e = &g_history[g_history_head];
        g_strlcpy(e->text, text, sizeof(e->text));
        e->ts_ms = ts;
        g_history_head = (g_history_head + 1) % TRANSCRIPT_HISTORY;
        if (g_history_count < TRANSCRIPT_HISTORY)
            g_history_count++;
    }
    pthread_mutex_unlock(&g_lock);

    if (g_sub_count == 0)
        return;
    struct broadcast *b = g_new0(struct broadcast, 1);
    g_strlcpy(b->text, text, sizeof(b->text));
    b->ts_ms = ts;
    b->is_final = is_final;
    g_idle_add(broadcast_idle, b);
}

void transcript_json_escape(GString *out, const char *s) {
    if (s == NULL)
        return;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, (gchar)*p);
        }
    }
}

void transcript_latest_json(GString *out) {
    pthread_mutex_lock(&g_lock);
    g_string_append(out, "{\"text\":\"");
    transcript_json_escape(out, g_latest.text);
    g_string_append_printf(out, "\",\"timestampMs\":%" G_GINT64_FORMAT
                                ",\"final\":%s}",
                           g_latest.ts_ms, g_latest_final ? "true" : "false");
    pthread_mutex_unlock(&g_lock);
}

void transcript_history_json(GString *out) {
    pthread_mutex_lock(&g_lock);
    g_string_append(out, "{\"transcripts\":[");
    int start = (g_history_head - g_history_count + TRANSCRIPT_HISTORY) %
                TRANSCRIPT_HISTORY;
    for (int i = 0; i < g_history_count; i++) {
        struct entry *e = &g_history[(start + i) % TRANSCRIPT_HISTORY];
        if (i > 0)
            g_string_append_c(out, ',');
        g_string_append(out, "{\"text\":\"");
        transcript_json_escape(out, e->text);
        g_string_append_printf(out, "\",\"timestampMs\":%" G_GINT64_FORMAT "}",
                               e->ts_ms);
    }
    g_string_append(out, "]}");
    pthread_mutex_unlock(&g_lock);
}
