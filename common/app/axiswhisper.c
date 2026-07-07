/**
 * axiswhisper - on-camera speech-to-text subtitles for Axis cameras
 *
 * Pipeline (all on-device, no external dependencies):
 *
 *   PipeWire capture (16 kHz mono S16)
 *     -> whisper.cpp (tiny.en quantized, CPU inference)
 *       -> axoverlay/Cairo subtitle bar burned into all video streams
 *
 * Three threads:
 *   - main:        GLib main loop servicing axoverlay render callbacks
 *   - pipewire:    capture stream feeding a ring buffer
 *   - transcribe:  consumes fixed-size chunks from the ring, runs whisper,
 *                  posts subtitle text and schedules an overlay redraw
 */

#include <axoverlay.h>
#include <cairo/cairo.h>
#include <dlfcn.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <syslog.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#pragma GCC diagnostic pop

#include <axparameter.h>
#include <curl/curl.h>

#include "whisper.h"

#include "mqtt.h"
#include "settings.h"
#include "transcript.h"
#include "webapi.h"

#define APP_NAME "Whisper_Subtitles"
#define PKG_DIR "/usr/local/packages/" APP_NAME

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define MODEL_FALLBACK PKG_DIR "/ggml-tiny.en-q5_1.bin"

#define SAMPLE_RATE 16000

/* Speech segmentation: audio is chopped into utterances by a simple
 * energy VAD with an adaptive noise floor. An utterance starts when a
 * 100 ms frame rises above the gate (with PREROLL_FRAMES of context
 * prepended), and ends after hang_frames of silence or max_utter_seconds.
 *
 * The gate, silence timeout, minimum speech length, maximum utterance
 * length and on-screen subtitle duration are all runtime-tunable from the
 * application's Settings web page (see the manifest paramConfig and the
 * runtime configuration section below). The values here are only defaults. */
#define FRAME_MS 100
#define FRAME_SAMPLES (SAMPLE_RATE * FRAME_MS / 1000)
#define PREROLL_FRAMES 3     /* 300 ms of audio kept before speech onset */

#define DEFAULT_HANG_MS 800        /* silence gap that ends an utterance */
#define DEFAULT_MIN_SPEECH_MS 300  /* ignore blips shorter than this */
#define DEFAULT_MAX_UTTER_SECONDS 6
#define MAX_UTTER_SECONDS_CAP 15   /* hard cap; sizes the utterance buffer */

/* Ring buffer capacity. If whisper falls behind by more than this, the
 * oldest audio is dropped so subtitles stay (roughly) live. */
#define RING_SECONDS 32
#define RING_SAMPLES (SAMPLE_RATE * RING_SECONDS)

/* Inference threads. ARTPEC-8/9 have 4 cores; leave one for the video
 * pipeline and other apps. */
#define N_THREADS 3

/* The speech gate is noise_floor * vad_snr, but never below vad_abs_floor.
 * The noise floor adapts to the room (fans, hum) while idle. */
#define NOISE_FLOOR_MIN 0.0005f
#define NOISE_FLOOR_MAX 0.02f

/* An utterance whose peak amplitude never rises above this is treated as
 * noise/near-silence and not transcribed. Kept low so genuinely quiet but
 * real speech from a good microphone is still transcribed; the upstream VAD
 * gate is the primary speech/noise decision. */
#define SIGNAL_PEAK_MIN 0.02f

/* Adaptive noise guard: an utterance is only transcribed if its peak clears
 * the adaptive room noise floor by this margin (or SIGNAL_PEAK_MIN, whichever
 * is higher). In a quiet room the floor is tiny, so quiet speech passes; in a
 * noisy room (e.g. constant background chatter on a CCTV install) the floor
 * rises, so only clear foreground speech is transcribed. This keeps whisper
 * from running - and hallucinating - on background noise, which also prevents
 * the transcriber falling behind and dropping audio. */
#define SIGNAL_SNR_MARGIN 8.0f

/* Default subtitle on-screen duration after the last transcription. */
#define DEFAULT_SUBTITLE_TTL_SEC 6

/* Streaming captions: while speech continues, re-transcribe the utterance so
 * far every STREAM_STEP_MS and update the on-screen text, so captions build
 * up live as the person speaks instead of only appearing once the phrase
 * ends. Sized small so updates feel live; the dynamic encoder context keeps
 * each partial pass cheap. */
#define STREAM_STEP_MS 900
#define STREAM_STEP_SAMPLES (SAMPLE_RATE * STREAM_STEP_MS / 1000)

/* Streaming partials re-transcribe the growing utterance and multiply CPU
 * cost. Under continuous speech (a busy CCTV scene with people talking in the
 * background) that makes the single transcription thread fall behind realtime
 * and drop audio. So partials are only emitted while the reader is keeping up:
 * if it has fallen more than this far behind the capture head, partials are
 * skipped and only the final (one pass per utterance) is produced, letting the
 * backlog drain. */
#define STREAM_MAX_BACKLOG_SAMPLES (SAMPLE_RATE * 2) /* ~2 s */

#define SUBTITLE_MAX 512
/* Maximum caption lines the renderer can lay out. The number actually shown
 * is runtime-tunable (cfg.max_lines) up to this compile-time cap. */
#define MAX_LINES 3

/* Localhost port for the transcription HTTP API. Must match the reverseProxy
 * target in manifest.json. */
#define API_PORT 2721

/* Localhost port for the settings config endpoint (admin reverseProxy route).
 * Kept separate from API_PORT so the device can enforce admin access on
 * settings writes while the transcription API stays viewer-accessible. */
#define CONFIG_PORT 2722

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

static volatile bool g_running = true;
static GMainLoop *g_loop = NULL;

/* Audio ring buffer (16-bit mono @ 16 kHz) */
static int16_t g_ring[RING_SAMPLES];
static uint64_t g_written = 0; /* total samples ever written */
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ring_cond = PTHREAD_COND_INITIALIZER;

/* Current subtitle */
static char g_subtitle[SUBTITLE_MAX] = "";
static gint64 g_subtitle_ts = 0;
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;

static gint g_overlay_id = -1;
static bool g_overlay_available = false;

struct axoverlay_api {
    void *handle;
    typeof(axoverlay_redraw) *redraw;
    typeof(axoverlay_is_backend_supported) *is_backend_supported;
    typeof(axoverlay_init_axoverlay_settings) *init_settings;
    typeof(axoverlay_init) *init;
    typeof(axoverlay_init_overlay_data) *init_overlay_data;
    typeof(axoverlay_get_max_resolution_width) *get_max_resolution_width;
    typeof(axoverlay_get_max_resolution_height) *get_max_resolution_height;
    typeof(axoverlay_create_overlay) *create_overlay;
    typeof(axoverlay_destroy_overlay) *destroy_overlay;
    typeof(axoverlay_cleanup) *cleanup;
};

static struct axoverlay_api g_axoverlay = {0};

/* Cairo is only used from the overlay render callback, so like axoverlay it is
 * loaded at runtime and treated as optional: on devices without the video /
 * overlay stack neither library is present, and the app still transcribes. */
struct cairo_api {
    void *handle;
    typeof(cairo_set_operator) *set_operator;
    typeof(cairo_set_source_rgba) *set_source_rgba;
    typeof(cairo_paint) *paint;
    typeof(cairo_select_font_face) *select_font_face;
    typeof(cairo_set_font_size) *set_font_size;
    typeof(cairo_text_extents) *text_extents;
    typeof(cairo_rectangle) *rectangle;
    typeof(cairo_fill) *fill;
    typeof(cairo_move_to) *move_to;
    typeof(cairo_show_text) *show_text;
};

static struct cairo_api g_cairo = {0};

/* Optional path to a user-supplied model (downloaded from ModelUrl); when set
 * it is loaded in preference to the bundled ggml-*.bin. */
static gchar *g_model_path_override = NULL;

/* ------------------------------------------------------------------ */
/* Runtime configuration                                               */
/*                                                                     */
/* These values are tunable at runtime from the application's Settings  */
/* web page (Apps > Whisper Subtitles > Settings) or via VAPIX          */
/* param.cgi. Changes are applied live through axparameter callbacks    */
/* (param_changed_cb), except SubtitlePosition which is read once at     */
/* startup. Readers take a lock-free snapshot with cfg_snapshot().      */
/* ------------------------------------------------------------------ */

struct config {
    float vad_snr;         /* speech gate = noise_floor * vad_snr */
    float vad_abs_floor;   /* gate never drops below this */
    int hang_frames;       /* silence frames that end an utterance */
    int min_speech_frames; /* discard utterances shorter than this */
    int max_utter_seconds; /* longest utterance sent to whisper */
    gint64 subtitle_ttl_us;/* how long a subtitle stays on screen */
    double font_scale;     /* subtitle font size multiplier */
    bool streaming;        /* emit live partial captions during speech */

    /* On-screen subtitles. When disabled, transcription still runs and is
     * published to the HTTP API and MQTT, but nothing is burned into video. */
    bool subtitles_enabled;
    int max_lines;         /* caption lines shown (1..MAX_LINES) */
    double bar_height_frac;/* subtitle bar height as fraction of frame */
    double bg_opacity;     /* backing box alpha (0..1) */
    double text_r, text_g, text_b; /* subtitle text colour (0..1) */

    /* Whisper decode parameters. */
    int n_threads;         /* inference threads */
    int max_tokens;        /* max decoded tokens per segment */
    bool translate;        /* translate to English (multilingual models) */
    bool temp_fallback;    /* enable temperature fallback re-decode */
    char language[16];     /* whisper language code, e.g. "en" or "auto" */

    /* Advanced speech/noise tuning. */
    float snr_margin;      /* transcribe only if peak clears floor * this */
    float peak_min;        /* absolute peak floor below which audio is noise */
    float max_gain;        /* cap on per-utterance normalization gain */
    int stream_step_samples; /* streaming partial re-transcribe cadence */
};

static struct config g_cfg = {
    .vad_snr = 2.2f,
    .vad_abs_floor = 0.0028f,
    .hang_frames = DEFAULT_HANG_MS / FRAME_MS,
    .min_speech_frames = DEFAULT_MIN_SPEECH_MS / FRAME_MS,
    .max_utter_seconds = DEFAULT_MAX_UTTER_SECONDS,
    .subtitle_ttl_us = (gint64)DEFAULT_SUBTITLE_TTL_SEC * G_USEC_PER_SEC,
    .font_scale = 1.0,
    .streaming = true,
    .subtitles_enabled = true,
    .max_lines = 2,
    .bar_height_frac = 0.2,
    .bg_opacity = 0.55,
    .text_r = 1.0,
    .text_g = 1.0,
    .text_b = 1.0,
    .n_threads = N_THREADS,
    .max_tokens = 64,
    .translate = false,
    .temp_fallback = false,
    .language = "en",
    .snr_margin = SIGNAL_SNR_MARGIN,
    .peak_min = SIGNAL_PEAK_MIN,
    .max_gain = 4.0f,
    .stream_step_samples = STREAM_STEP_SAMPLES,
};
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Read once at startup: place the subtitle bar near the top of the frame
 * instead of the bottom. Changing it takes effect after an app restart. */
static bool g_subtitle_top = false;

static void cfg_snapshot(struct config *out) {
    pthread_mutex_lock(&g_cfg_lock);
    *out = g_cfg;
    pthread_mutex_unlock(&g_cfg_lock);
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool parse_bool(const char *value) {
    return g_ascii_strcasecmp(value, "yes") == 0 ||
           g_ascii_strcasecmp(value, "true") == 0 ||
           g_ascii_strcasecmp(value, "on") == 0 || g_strcmp0(value, "1") == 0;
}

/* Redraw request; defined with the overlay code further down. */
static void request_redraw(void);

/* Map the Settings web page values onto the config struct. Called both at
 * startup (for every parameter) and from the change callback. Unknown
 * parameter names are ignored. */
static void cfg_apply(const char *name, const char *value) {
    if (name == NULL || value == NULL)
        return;

    if (g_str_has_suffix(name, "MicSensitivity")) {
        float snr, floor;
        if (g_strcmp0(value, "Low") == 0) {
            snr = 2.6f;
            floor = 0.0045f;
        } else if (g_strcmp0(value, "High") == 0) {
            snr = 1.8f;
            floor = 0.0016f;
        } else if (g_strcmp0(value, "Maximum") == 0) {
            snr = 1.5f;
            floor = 0.0009f;
        } else { /* Normal */
            snr = 2.2f;
            floor = 0.0028f;
        }
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.vad_snr = snr;
        g_cfg.vad_abs_floor = floor;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "FontScale")) {
        double s = 1.0;
        if (g_strcmp0(value, "Small") == 0)
            s = 0.8;
        else if (g_strcmp0(value, "Large") == 0)
            s = 1.3;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.font_scale = s;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "SilenceTimeoutMs")) {
        int frames = clampi(atoi(value), 200, 3000) / FRAME_MS;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.hang_frames = frames;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "MinSpeechMs")) {
        int frames = clampi(atoi(value), 100, 2000) / FRAME_MS;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.min_speech_frames = frames;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "MaxUtteranceSec")) {
        int s = clampi(atoi(value), 2, MAX_UTTER_SECONDS_CAP);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.max_utter_seconds = s;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "SubtitleDurationSec")) {
        gint64 ttl = (gint64)clampi(atoi(value), 1, 30) * G_USEC_PER_SEC;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.subtitle_ttl_us = ttl;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "SubtitlePosition")) {
        g_subtitle_top = (g_strcmp0(value, "Top") == 0);
    } else if (g_str_has_suffix(name, "StreamingCaptions")) {
        bool on = (g_ascii_strcasecmp(value, "yes") == 0 ||
                   g_ascii_strcasecmp(value, "true") == 0 ||
                   g_strcmp0(value, "1") == 0);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.streaming = on;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "SubtitlesEnabled")) {
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.subtitles_enabled = parse_bool(value);
        pthread_mutex_unlock(&g_cfg_lock);
        request_redraw(); /* clear or restore the overlay immediately */
    } else if (g_str_has_suffix(name, "MaxLines")) {
        int n = clampi(atoi(value), 1, MAX_LINES);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.max_lines = n;
        pthread_mutex_unlock(&g_cfg_lock);
        request_redraw();
    } else if (g_str_has_suffix(name, "BarHeightPct")) {
        double f = clampi(atoi(value), 5, 50) / 100.0;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.bar_height_frac = f;
        pthread_mutex_unlock(&g_cfg_lock);
        request_redraw();
    } else if (g_str_has_suffix(name, "BackgroundOpacity")) {
        double o = clampi(atoi(value), 0, 100) / 100.0;
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.bg_opacity = o;
        pthread_mutex_unlock(&g_cfg_lock);
        request_redraw();
    } else if (g_str_has_suffix(name, "TextColor")) {
        const char *h = value;
        if (h[0] == '#')
            h++;
        if (strlen(h) >= 6) {
            char c[3] = {0, 0, 0};
            c[0] = h[0]; c[1] = h[1];
            double r = (double)g_ascii_strtoll(c, NULL, 16) / 255.0;
            c[0] = h[2]; c[1] = h[3];
            double g = (double)g_ascii_strtoll(c, NULL, 16) / 255.0;
            c[0] = h[4]; c[1] = h[5];
            double b = (double)g_ascii_strtoll(c, NULL, 16) / 255.0;
            pthread_mutex_lock(&g_cfg_lock);
            g_cfg.text_r = r; g_cfg.text_g = g; g_cfg.text_b = b;
            pthread_mutex_unlock(&g_cfg_lock);
            request_redraw();
        }
    } else if (g_str_has_suffix(name, "InferenceThreads")) {
        int n = clampi(atoi(value), 1, 4);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.n_threads = n;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "MaxTokens")) {
        int n = clampi(atoi(value), 16, 224);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.max_tokens = n;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "TemperatureFallback")) {
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.temp_fallback = parse_bool(value);
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "Translate")) {
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.translate = parse_bool(value);
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "Language")) {
        pthread_mutex_lock(&g_cfg_lock);
        g_strlcpy(g_cfg.language, value[0] != '\0' ? value : "en",
                  sizeof(g_cfg.language));
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "NoiseGuardMargin")) {
        double v = clampd(g_ascii_strtod(value, NULL), 0.5, 40.0);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.snr_margin = (float)v;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "MinSignalPeak")) {
        double v = clampd(g_ascii_strtod(value, NULL), 0.0, 0.5);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.peak_min = (float)v;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "MaxGain")) {
        double v = clampd(g_ascii_strtod(value, NULL), 1.0, 10.0);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.max_gain = (float)v;
        pthread_mutex_unlock(&g_cfg_lock);
    } else if (g_str_has_suffix(name, "StreamStepMs")) {
        int ms = clampi(atoi(value), 300, 3000);
        pthread_mutex_lock(&g_cfg_lock);
        g_cfg.stream_step_samples = SAMPLE_RATE * ms / 1000;
        pthread_mutex_unlock(&g_cfg_lock);
    }
}

static void param_changed_cb(const gchar *name,
                             const gchar *value,
                             gpointer data) {
    (void)data;
    syslog(LOG_INFO, "setting %s changed to '%s'", name,
           value != NULL ? value : "");
    cfg_apply(name, value);
}

/* Read a parameter's current value, apply it, and register a callback so
 * later changes from the Settings web page are picked up live. */
static void load_and_watch(AXParameter *params, const char *name) {
    GError *err = NULL;
    gchar *value = NULL;
    if (ax_parameter_get(params, name, &value, &err) && value != NULL)
        cfg_apply(name, value);
    else if (err != NULL)
        g_clear_error(&err);
    g_free(value);

    err = NULL;
    if (!ax_parameter_register_callback(params, name, param_changed_cb, NULL,
                                        &err)) {
        syslog(LOG_WARNING, "could not watch setting %s: %s", name,
               err != NULL ? err->message : "unknown");
        g_clear_error(&err);
    }
}



/* ------------------------------------------------------------------ */
/* Overlay rendering                                                   */
/* ------------------------------------------------------------------ */

static gboolean redraw_idle_cb(gpointer user_data) {
    (void)user_data;
    if (!g_overlay_available)
        return G_SOURCE_REMOVE;

    GError *error = NULL;
    g_axoverlay.redraw(&error);
    if (error != NULL) {
        syslog(LOG_WARNING, "axoverlay_redraw failed: %s", error->message);
        g_error_free(error);
    }
    return G_SOURCE_REMOVE;
}

static void request_redraw(void) {
    if (!g_overlay_available)
        return;
    g_idle_add(redraw_idle_cb, NULL);
}

/* Greedy word wrap using cairo text metrics. Keeps the LAST MAX_LINES
 * lines when the text overflows, which reads naturally for live captions. */
static int wrap_text(cairo_t *cr,
                     const char *text,
                     double max_width,
                     int max_lines,
                     char lines[MAX_LINES][SUBTITLE_MAX]) {
    char tmp[8][SUBTITLE_MAX];
    int count = 0;
    char cur[SUBTITLE_MAX] = "";

    if (max_lines < 1)
        max_lines = 1;
    if (max_lines > MAX_LINES)
        max_lines = MAX_LINES;

    gchar **words = g_strsplit(text, " ", -1);
    for (gchar **w = words; *w != NULL; w++) {
        if (**w == '\0')
            continue;
        char candidate[SUBTITLE_MAX];
        if (cur[0] == '\0')
            g_strlcpy(candidate, *w, sizeof(candidate));
        else
            g_snprintf(candidate, sizeof(candidate), "%s %s", cur, *w);

        cairo_text_extents_t ext;
        g_cairo.text_extents(cr, candidate, &ext);
        if (ext.width <= max_width || cur[0] == '\0') {
            g_strlcpy(cur, candidate, sizeof(cur));
        } else {
            if (count < 8)
                g_strlcpy(tmp[count++], cur, SUBTITLE_MAX);
            g_strlcpy(cur, *w, sizeof(cur));
        }
    }
    if (cur[0] != '\0' && count < 8)
        g_strlcpy(tmp[count++], cur, SUBTITLE_MAX);
    g_strfreev(words);

    int start = count > max_lines ? count - max_lines : 0;
    for (int i = start; i < count; i++)
        g_strlcpy(lines[i - start], tmp[i], SUBTITLE_MAX);
    return count - start;
}

static void render_overlay_cb(gpointer render_context,
                              gint id,
                              struct axoverlay_stream_data *stream,
                              enum axoverlay_position_type postype,
                              gfloat overlay_x,
                              gfloat overlay_y,
                              gint overlay_width,
                              gint overlay_height,
                              gpointer user_data) {
    (void)id;
    (void)stream;
    (void)postype;
    (void)overlay_x;
    (void)overlay_y;
    (void)user_data;
    cairo_t *cr = (cairo_t *)render_context;

    /* Clear to fully transparent */
    g_cairo.set_operator(cr, CAIRO_OPERATOR_SOURCE);
    g_cairo.set_source_rgba(cr, 0, 0, 0, 0);
    g_cairo.paint(cr);
    g_cairo.set_operator(cr, CAIRO_OPERATOR_OVER);

    char text[SUBTITLE_MAX];
    pthread_mutex_lock(&g_sub_lock);
    g_strlcpy(text, g_subtitle, sizeof(text));
    pthread_mutex_unlock(&g_sub_lock);
    if (text[0] == '\0')
        return;

    struct config cfg;
    cfg_snapshot(&cfg);
    if (!cfg.subtitles_enabled)
        return; /* transcription still runs; nothing is burned into video */

    double font_size = overlay_height / 3.2 * cfg.font_scale;
    g_cairo.select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
    g_cairo.set_font_size(cr, font_size);

    char lines[MAX_LINES][SUBTITLE_MAX];
    int n_lines = wrap_text(cr, text, overlay_width * 0.92, cfg.max_lines, lines);
    if (n_lines == 0)
        return;

    double line_h = font_size * 1.35;
    double pad = font_size * 0.35;
    double block_top = overlay_height - line_h * n_lines - pad;

    for (int i = 0; i < n_lines; i++) {
        cairo_text_extents_t ext;
        g_cairo.text_extents(cr, lines[i], &ext);
        double tx = (overlay_width - ext.width) / 2.0;
        double baseline = block_top + line_h * i + font_size;

        /* Semi-transparent backing box */
        g_cairo.set_source_rgba(cr, 0, 0, 0, cfg.bg_opacity);
        g_cairo.rectangle(cr, tx - pad, baseline - font_size,
                          ext.width + 2 * pad, line_h);
        g_cairo.fill(cr);

        /* Subtitle text */
        g_cairo.set_source_rgba(cr, cfg.text_r, cfg.text_g, cfg.text_b, 1);
        g_cairo.move_to(cr, tx, baseline);
        g_cairo.show_text(cr, lines[i]);
    }
}

static void adjustment_cb(gint id,
                          struct axoverlay_stream_data *stream,
                          enum axoverlay_position_type *postype,
                          gfloat *overlay_x,
                          gfloat *overlay_y,
                          gint *overlay_width,
                          gint *overlay_height,
                          gpointer user_data) {
    (void)id;
    (void)postype;
    (void)overlay_x;
    (void)overlay_y;
    (void)user_data;
    /* Subtitle bar: full stream width, a configurable fraction of the frame */
    struct config cfg;
    cfg_snapshot(&cfg);
    *overlay_width = stream->width;
    *overlay_height = (gint)(stream->height * cfg.bar_height_frac);
}

/* Clear stale subtitles */
static gboolean expire_cb(gpointer user_data) {
    (void)user_data;
    struct config cfg;
    cfg_snapshot(&cfg);
    bool changed = false;
    pthread_mutex_lock(&g_sub_lock);
    if (g_subtitle[0] != '\0' &&
        g_get_monotonic_time() - g_subtitle_ts > cfg.subtitle_ttl_us) {
        g_subtitle[0] = '\0';
        changed = true;
    }
    pthread_mutex_unlock(&g_sub_lock);
    if (changed)
        request_redraw();
    return G_SOURCE_CONTINUE;
}

static void post_subtitle(const char *text) {
    pthread_mutex_lock(&g_sub_lock);
    g_strlcpy(g_subtitle, text, sizeof(g_subtitle));
    g_subtitle_ts = g_get_monotonic_time();
    pthread_mutex_unlock(&g_sub_lock);
    request_redraw();
}

/* ------------------------------------------------------------------ */
/* Audio ring buffer                                                   */
/* ------------------------------------------------------------------ */

static void ring_push(const int16_t *samples, size_t n) {
    pthread_mutex_lock(&g_ring_lock);
    for (size_t i = 0; i < n; i++)
        g_ring[(g_written + i) % RING_SAMPLES] = samples[i];
    g_written += n;
    pthread_cond_broadcast(&g_ring_cond);
    pthread_mutex_unlock(&g_ring_lock);
}

static gboolean quit_idle_cb(gpointer user_data) {
    (void)user_data;
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Audio capture (PipeWire)                                            */
/* ------------------------------------------------------------------ */

static struct pw_thread_loop *g_pw_loop = NULL;
static struct pw_context *g_pw_context = NULL;
static struct pw_core *g_pw_core = NULL;
static struct pw_registry *g_pw_registry = NULL;
static struct spa_hook g_pw_registry_listener;
static struct pw_stream *g_pw_stream = NULL;
static struct spa_hook g_pw_stream_listener;
static uint32_t g_pw_node_id = SPA_ID_INVALID;

/* Capture source selection. PipeWire on Axis devices exposes a silent fallback
 * 'dummy-source' node alongside the real microphone(s); binding the first
 * Audio/Source that appears often grabs the dummy. So we log every source, skip
 * the dummy, optionally pick one the user selected by name (AudioInput), and
 * only fall back to the dummy if nothing better appears. */
static char g_audio_input[128] = "";  /* user selection substring; "" = auto */
static bool g_capture_bound = false;
static uint32_t g_fallback_id = SPA_ID_INVALID;
static char g_fallback_name[256] = "";
static int g_fallback_rank = 0;
static guint g_fallback_timer_id = 0;

/* PipeWire is only used for LOCAL microphone capture. On devices without a
 * local audio pipeline (e.g. video-less units used purely with remote audio)
 * libpipewire is absent, so like axoverlay/cairo it is loaded at runtime and
 * only when local capture is actually requested. The header still provides the
 * types, constants and inline/vtable helpers (PW_KEY_*, spa_* builders,
 * pw_core_get_registry, pw_registry_add_listener); only the real exported
 * functions are bound here. */
struct pw_api {
    void *handle;
    typeof(pw_init) *init;
    typeof(pw_deinit) *deinit;
    typeof(pw_thread_loop_new) *thread_loop_new;
    typeof(pw_thread_loop_start) *thread_loop_start;
    typeof(pw_thread_loop_lock) *thread_loop_lock;
    typeof(pw_thread_loop_unlock) *thread_loop_unlock;
    typeof(pw_thread_loop_stop) *thread_loop_stop;
    typeof(pw_thread_loop_destroy) *thread_loop_destroy;
    typeof(pw_thread_loop_get_loop) *thread_loop_get_loop;
    typeof(pw_context_new) *context_new;
    typeof(pw_context_connect) *context_connect;
    typeof(pw_context_destroy) *context_destroy;
    typeof(pw_core_disconnect) *core_disconnect;
    typeof(pw_proxy_destroy) *proxy_destroy;
    typeof(pw_stream_new) *stream_new;
    typeof(pw_stream_add_listener) *stream_add_listener;
    typeof(pw_stream_connect) *stream_connect;
    typeof(pw_stream_destroy) *stream_destroy;
    typeof(pw_stream_dequeue_buffer) *stream_dequeue_buffer;
    typeof(pw_stream_queue_buffer) *stream_queue_buffer;
    typeof(pw_stream_state_as_string) *stream_state_as_string;
    typeof(pw_properties_new) *properties_new;
};

static struct pw_api g_pw = {0};

static bool load_pw_symbol(void *handle, void **slot, const char *name) {
    dlerror();
    *slot = dlsym(handle, name);
    if (*slot != NULL)
        return true;

    syslog(LOG_WARNING, "missing pipewire symbol %s: %s", name,
           dlerror() != NULL ? dlerror() : "unknown");
    return false;
}

static bool load_pw_api(void) {
    if (g_pw.handle != NULL)
        return true;

    g_pw.handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (g_pw.handle == NULL) {
        syslog(LOG_WARNING, "pipewire unavailable: %s", dlerror());
        return false;
    }

    void *h = g_pw.handle;
    if (!load_pw_symbol(h, (void **)&g_pw.init, "pw_init") ||
        !load_pw_symbol(h, (void **)&g_pw.deinit, "pw_deinit") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_new,
                        "pw_thread_loop_new") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_start,
                        "pw_thread_loop_start") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_lock,
                        "pw_thread_loop_lock") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_unlock,
                        "pw_thread_loop_unlock") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_stop,
                        "pw_thread_loop_stop") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_destroy,
                        "pw_thread_loop_destroy") ||
        !load_pw_symbol(h, (void **)&g_pw.thread_loop_get_loop,
                        "pw_thread_loop_get_loop") ||
        !load_pw_symbol(h, (void **)&g_pw.context_new, "pw_context_new") ||
        !load_pw_symbol(h, (void **)&g_pw.context_connect,
                        "pw_context_connect") ||
        !load_pw_symbol(h, (void **)&g_pw.context_destroy,
                        "pw_context_destroy") ||
        !load_pw_symbol(h, (void **)&g_pw.core_disconnect,
                        "pw_core_disconnect") ||
        !load_pw_symbol(h, (void **)&g_pw.proxy_destroy, "pw_proxy_destroy") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_new, "pw_stream_new") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_add_listener,
                        "pw_stream_add_listener") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_connect,
                        "pw_stream_connect") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_destroy,
                        "pw_stream_destroy") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_dequeue_buffer,
                        "pw_stream_dequeue_buffer") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_queue_buffer,
                        "pw_stream_queue_buffer") ||
        !load_pw_symbol(h, (void **)&g_pw.stream_state_as_string,
                        "pw_stream_state_as_string") ||
        !load_pw_symbol(h, (void **)&g_pw.properties_new,
                        "pw_properties_new")) {
        dlclose(g_pw.handle);
        memset(&g_pw, 0, sizeof(g_pw));
        return false;
    }

    return true;
}

/* Runs in the PipeWire loop thread. The stream is negotiated to 16 kHz
 * mono S16, so samples go straight into the ring buffer. */
static void on_pw_process(void *data) {
    (void)data;
    struct pw_buffer *b = g_pw.stream_dequeue_buffer(g_pw_stream);
    if (b == NULL)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data != NULL && d->chunk->size > 0) {
        const int16_t *samples = SPA_PTROFF(d->data, d->chunk->offset, const int16_t);
        ring_push(samples, d->chunk->size / sizeof(int16_t));
    }
    g_pw.stream_queue_buffer(g_pw_stream, b);
}

static void on_pw_state_changed(void *data,
                                enum pw_stream_state old,
                                enum pw_stream_state state,
                                const char *error) {
    (void)data;
    (void)old;
    syslog(LOG_INFO, "capture stream state: %s%s%s",
           g_pw.stream_state_as_string(state), error != NULL ? " - " : "",
           error != NULL ? error : "");
}

static const struct pw_stream_events pw_stream_events_impl = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_pw_process,
    .state_changed = on_pw_state_changed,
};

/* Case-insensitive substring match of want against a node's name/description. */
static bool source_matches(const char *name, const char *desc,
                           const char *want) {
    if (want == NULL || want[0] == '\0')
        return false;
    gchar *lw = g_ascii_strdown(want, -1);
    gchar *ln = name != NULL ? g_ascii_strdown(name, -1) : NULL;
    gchar *ld = desc != NULL ? g_ascii_strdown(desc, -1) : NULL;
    bool m = (ln != NULL && strstr(ln, lw) != NULL) ||
             (ld != NULL && strstr(ld, lw) != NULL);
    g_free(lw);
    g_free(ln);
    g_free(ld);
    return m;
}

/* Desirability of an audio source: 3 = the plain, fully processed microphone
 * (with the device's gain/AGC, best for speech), 2 = a raw variant such as
 * '.Unprocessed' or '.Uncompressed' (a dotted suffix on the input, often too
 * quiet), 1 = the silent 'dummy-source' fallback. Higher is better. */
static int source_rank(const char *name) {
    gchar *l = g_ascii_strdown(name, -1);
    int rank;
    if (strstr(l, "dummy") != NULL)
        rank = 1;
    else if (strchr(l, '.') != NULL)
        rank = 2; /* AudioDevice0Input0.Unprocessed / .Uncompressed / ... */
    else
        rank = 3; /* AudioDevice0Input0 */
    g_free(l);
    return rank;
}

/* Create and connect the capture stream to the given node. Must be called with
 * the PipeWire thread loop locked. */
static bool bind_capture_source(uint32_t id, const char *name) {
    struct pw_properties *stream_props =
        g_pw.properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                            PW_KEY_MEDIA_CATEGORY, "Capture",
                            PW_KEY_TARGET_OBJECT, name, NULL);
    g_pw_stream = g_pw.stream_new(g_pw_core, "whisper-subtitles", stream_props);
    if (g_pw_stream == NULL) {
        syslog(LOG_ERR, "pw_stream_new failed");
        return false;
    }
    g_pw.stream_add_listener(g_pw_stream, &g_pw_stream_listener,
                             &pw_stream_events_impl, NULL);

    /* Ask for exactly what whisper wants; the stream adapter resamples
     * and downmixes from the device native format. */
    uint8_t buf[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16,
                                 .rate = SAMPLE_RATE,
                                 .channels = 1));

    int res = g_pw.stream_connect(g_pw_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                  PW_STREAM_FLAG_AUTOCONNECT |
                                      PW_STREAM_FLAG_MAP_BUFFERS,
                                  params, 1);
    if (res < 0) {
        syslog(LOG_ERR, "pw_stream_connect failed: %s", strerror(-res));
        g_pw.stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
        return false;
    }
    g_pw_node_id = id;
    g_capture_bound = true;
    syslog(LOG_INFO, "capturing from audio source '%s' (id %u)", name, id);
    return true;
}

/* Called from the PipeWire loop thread for every global object; attach a
 * capture stream to a suitable audio source node. Every audio source is
 * logged so users can see what to select via the AudioInput setting. */
static void on_registry_global(void *data,
                               uint32_t id,
                               uint32_t permissions,
                               const char *type,
                               uint32_t version,
                               const struct spa_dict *props) {
    (void)data;
    (void)permissions;
    (void)version;

    if (g_capture_bound || !spa_streq(type, PW_TYPE_INTERFACE_Node))
        return;
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (media_class == NULL || name == NULL)
        return;
    if (strncmp(media_class, "Audio/Source", strlen("Audio/Source")) != 0)
        return;
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    int rank = source_rank(name);
    const char *tag =
        rank == 1 ? " (dummy fallback)" : rank == 2 ? " (raw variant)" : "";
    syslog(LOG_INFO, "audio source available: id %u name '%s' desc '%s'%s", id,
           name, desc != NULL ? desc : "", tag);

    /* In manual mode (AudioInput set) only the user-selected input is eligible;
     * in auto mode every source is. Either way the choice is ranked, so the
     * plain processed mic wins over raw variants and the dummy. */
    if (g_audio_input[0] != '\0' && !source_matches(name, desc, g_audio_input))
        return;

    if (rank >= 3) {
        bind_capture_source(id, name);
        return;
    }
    if (rank > g_fallback_rank) {
        g_fallback_rank = rank;
        g_fallback_id = id;
        g_strlcpy(g_fallback_name, name, sizeof(g_fallback_name));
    }
}

/* After enumeration settles, bind a fallback source if a real one was not
 * found, so devices that only expose a dummy node still behave as before. */
static gboolean bind_fallback_cb(gpointer user) {
    (void)user;
    g_fallback_timer_id = 0;
    if (g_pw_loop == NULL || g_capture_bound)
        return G_SOURCE_REMOVE;
    g_pw.thread_loop_lock(g_pw_loop);
    if (!g_capture_bound) {
        if (g_fallback_id != SPA_ID_INVALID) {
            syslog(LOG_WARNING,
                   "no fully processed microphone found; using '%s'. If "
                   "captions never appear it may be too quiet - check that the "
                   "microphone is enabled and raise its input gain.",
                   g_fallback_name);
            bind_capture_source(g_fallback_id, g_fallback_name);
        } else if (g_audio_input[0] != '\0') {
            syslog(LOG_WARNING,
                   "selected audio input '%s' not found; see the 'audio source "
                   "available' log lines for valid names",
                   g_audio_input);
        } else {
            syslog(LOG_WARNING, "no audio source found");
        }
    }
    g_pw.thread_loop_unlock(g_pw_loop);
    return G_SOURCE_REMOVE;
}

static void on_registry_global_remove(void *data, uint32_t id) {
    (void)data;
    if (g_pw_stream != NULL && id == g_pw_node_id) {
        syslog(LOG_WARNING, "capture node removed, waiting for a new one");
        spa_hook_remove(&g_pw_stream_listener);
        g_pw.stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
        g_pw_node_id = SPA_ID_INVALID;
        g_capture_bound = false;
        g_fallback_id = SPA_ID_INVALID;
        g_fallback_rank = 0;
    }
}

static const struct pw_registry_events pw_registry_events_impl = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static bool start_audio(void) {
    if (!load_pw_api()) {
        syslog(LOG_ERR, "local audio capture unavailable (no PipeWire on this "
                        "device); set RemoteAudioHost to use remote audio");
        return false;
    }
    g_pw.init(NULL, NULL);
    g_pw_loop = g_pw.thread_loop_new("audio-capture", NULL);
    if (g_pw_loop == NULL || g_pw.thread_loop_start(g_pw_loop) != 0) {
        syslog(LOG_ERR, "failed to start PipeWire loop");
        return false;
    }

    g_pw.thread_loop_lock(g_pw_loop);
    g_pw_context = g_pw.context_new(g_pw.thread_loop_get_loop(g_pw_loop), NULL, 0);
    if (g_pw_context != NULL)
        g_pw_core = g_pw.context_connect(g_pw_context, NULL, 0);
    if (g_pw_core != NULL) {
        g_pw_registry = pw_core_get_registry(g_pw_core, PW_VERSION_REGISTRY, 0);
        if (g_pw_registry != NULL)
            pw_registry_add_listener(g_pw_registry, &g_pw_registry_listener,
                                     &pw_registry_events_impl, NULL);
    }
    g_pw.thread_loop_unlock(g_pw_loop);

    if (g_pw_registry == NULL) {
        syslog(LOG_ERR, "cannot connect to PipeWire - is audio enabled on the "
                        "camera and the app user in the 'pipewire' group?");
        return false;
    }

    /* Give the registry a moment to enumerate, then bind a fallback source if
     * no real microphone was found. */
    g_fallback_timer_id = g_timeout_add(2000, bind_fallback_cb, NULL);
    return true;
}

static void stop_audio(void) {
    if (g_fallback_timer_id != 0) {
        g_source_remove(g_fallback_timer_id);
        g_fallback_timer_id = 0;
    }
    if (g_pw_loop == NULL)
        return;
    g_pw.thread_loop_lock(g_pw_loop);
    if (g_pw_stream != NULL) {
        spa_hook_remove(&g_pw_stream_listener);
        g_pw.stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
    }
    if (g_pw_registry != NULL) {
        spa_hook_remove(&g_pw_registry_listener);
        g_pw.proxy_destroy((struct pw_proxy *)g_pw_registry);
        g_pw_registry = NULL;
    }
    g_pw.thread_loop_unlock(g_pw_loop);
    g_pw.thread_loop_stop(g_pw_loop);
    if (g_pw_core != NULL)
        g_pw.core_disconnect(g_pw_core);
    if (g_pw_context != NULL)
        g_pw.context_destroy(g_pw_context);
    g_pw.thread_loop_destroy(g_pw_loop);
    g_pw_loop = NULL;
    g_pw.deinit();
}

/* ------------------------------------------------------------------ */
/* Audio capture (remote Axis device over VAPIX)                       */
/*                                                                     */
/* For cameras without a microphone: stream G.711 u-law audio from     */
/* another Axis device's /axis-cgi/audio/receive.cgi and feed it into  */
/* the same ring buffer. u-law @ 8 kHz is decoded with a lookup table  */
/* and linearly upsampled to 16 kHz.                                   */
/* ------------------------------------------------------------------ */

#define REMOTE_RETRY_SECONDS 5

struct remote_ctx {
    char url[512];
    char config_url[512];
    char userpwd[256];
    int16_t last_sample; /* upsampler continuity across callbacks */
    int16_t *out;        /* decoded+upsampled scratch buffer */
    size_t out_cap;
    bool logged_type;
};

static pthread_t g_remote_tid;
static bool g_remote_started = false;
static int16_t g_ulaw2pcm[256];

static void ulaw_table_init(void) {
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int sign = u & 0x80;
        int exponent = (u >> 4) & 7;
        int mantissa = u & 0x0F;
        int t = (((mantissa << 3) + 0x84) << exponent) - 0x84;
        g_ulaw2pcm[i] = (int16_t)(sign ? -t : t);
    }
}

static size_t remote_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct remote_ctx *ctx = userdata;
    size_t n = size * nmemb;

    if (!g_running)
        return 0; /* abort the transfer */

    if (!ctx->logged_type) {
        ctx->logged_type = true;
        /* audio/basic = G.711 u-law, 8 kHz mono, per RFC 2046 */
        syslog(LOG_INFO, "remote audio stream connected (%zu byte first chunk)", n);
    }

    if (ctx->out_cap < n * 2) {
        ctx->out_cap = n * 2;
        ctx->out = g_realloc(ctx->out, ctx->out_cap * sizeof(int16_t));
    }

    /* Decode u-law and upsample 8 kHz -> 16 kHz by linear interpolation */
    size_t n_out = 0;
    int16_t last = ctx->last_sample;
    for (size_t i = 0; i < n; i++) {
        int16_t s = g_ulaw2pcm[(unsigned char)ptr[i]];
        ctx->out[n_out++] = (int16_t)(((int32_t)last + s) / 2);
        ctx->out[n_out++] = s;
        last = s;
    }
    ctx->last_sample = last;

    ring_push(ctx->out, n_out);
    return n;
}

/* Best effort: make sure audio is enabled and G.711-encoded on the
 * remote device, so receive.cgi delivers a u-law stream. */
static void remote_configure_source(struct remote_ctx *ctx) {
    CURL *curl = curl_easy_init();
    if (curl == NULL)
        return;
    curl_easy_setopt(curl, CURLOPT_URL, ctx->config_url);
    curl_easy_setopt(curl, CURLOPT_USERPWD, ctx->userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL); /* discard body */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull != NULL)
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, devnull);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        syslog(LOG_WARNING, "could not configure remote audio source: %s",
               curl_easy_strerror(res));
    if (devnull != NULL)
        fclose(devnull);
    curl_easy_cleanup(curl);
}

static void *remote_audio_thread(void *arg) {
    struct remote_ctx *ctx = arg;

    remote_configure_source(ctx);

    while (g_running) {
        CURL *curl = curl_easy_init();
        if (curl == NULL) {
            syslog(LOG_ERR, "curl_easy_init failed");
            break;
        }
        ctx->logged_type = false;
        ctx->last_sample = 0;

        curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
        curl_easy_setopt(curl, CURLOPT_USERPWD, ctx->userpwd);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, remote_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx);
        /* The stream always flows (silence is still bytes); treat a stall
         * as a dead connection. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 500L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);

        if (!g_running)
            break;
        syslog(LOG_WARNING,
               "remote audio stream ended (%s, HTTP %ld), retrying in %d s",
               curl_easy_strerror(res), code, REMOTE_RETRY_SECONDS);
        for (int i = 0; i < REMOTE_RETRY_SECONDS * 10 && g_running; i++)
            g_usleep(100 * 1000);
    }

    g_free(ctx->out);
    g_free(ctx);
    return NULL;
}

static bool start_remote_audio(const char *host, const char *user, const char *pass) {
    ulaw_table_init();

    struct remote_ctx *ctx = g_malloc0(sizeof(*ctx));
    g_snprintf(ctx->url, sizeof(ctx->url),
               "http://%s/axis-cgi/audio/receive.cgi", host);
    g_snprintf(ctx->config_url, sizeof(ctx->config_url),
               "http://%s/axis-cgi/param.cgi?action=update"
               "&Audio.A0.Enabled=yes&AudioSource.A0.AudioEncoding=g711",
               host);
    g_snprintf(ctx->userpwd, sizeof(ctx->userpwd), "%s:%s", user, pass);

    syslog(LOG_INFO, "using remote audio from %s", host);
    if (pthread_create(&g_remote_tid, NULL, remote_audio_thread, ctx) != 0) {
        syslog(LOG_ERR, "failed to start remote audio thread");
        g_free(ctx);
        return false;
    }
    g_remote_started = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Transcription                                                       */
/* ------------------------------------------------------------------ */

static void whisper_log_cb(enum ggml_log_level level,
                           const char *text,
                           void *user_data) {
    (void)user_data;
    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR)
        syslog(LOG_INFO, "whisper: %s", text);
}

/* Pull the next 100 ms frame out of the ring buffer.
 * Returns 0 on success, 1 if audio was dropped (caller should reset its
 * VAD state), -1 on shutdown. */
static int read_frame(uint64_t *next, int16_t *frame) {
    int dropped = 0;
    pthread_mutex_lock(&g_ring_lock);
    while (g_running && g_written < *next + FRAME_SAMPLES)
        pthread_cond_wait(&g_ring_cond, &g_ring_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_ring_lock);
        return -1;
    }
    if (g_written - *next > RING_SAMPLES - FRAME_SAMPLES) {
        uint64_t skip_to = g_written - FRAME_SAMPLES;
        syslog(LOG_WARNING,
               "transcription lagging, dropping %" G_GUINT64_FORMAT " samples",
               (guint64)(skip_to - *next));
        *next = skip_to;
        dropped = 1;
    }
    for (int i = 0; i < FRAME_SAMPLES; i++)
        frame[i] = g_ring[(*next + i) % RING_SAMPLES];
    pthread_mutex_unlock(&g_ring_lock);
    *next += FRAME_SAMPLES;
    return dropped;
}

/* True when the whole line is a single non-speech annotation that whisper
 * emits on silence or music, e.g. "[BLANK_AUDIO]", "(bell dings)", or a line
 * of musical notes. A line that merely contains a parenthetical or starts
 * with an accented (non-ASCII) word is real speech and is kept. */
static bool is_nonspeech_annotation(const char *text) {
    if (text == NULL || text[0] == '\0')
        return true;
    /* Musical note glyphs U+2669..U+266C (UTF-8 E2 99 A9..AC) => music. */
    if ((unsigned char)text[0] == 0xE2 && (unsigned char)text[1] == 0x99)
        return true;
    size_t len = strlen(text);
    if (len >= 2) {
        if (text[0] == '[' && text[len - 1] == ']')
            return true;
        if (text[0] == '(' && text[len - 1] == ')')
            return true;
        /* Asterisk-wrapped stage directions, e.g. "*crying*", "*cackling*". */
        if (text[0] == '*' && text[len - 1] == '*')
            return true;
    }
    return false;
}

/* Detect whisper's degenerate repetition loops (e.g. "Long day. Long day.
 * Long day. ..." x50), a common failure mode on quiet or non-speech audio.
 * Returns true when the phrase has many words but very few distinct ones,
 * so it can be filtered out instead of posted as a subtitle. Thresholds are
 * deliberately conservative so legitimately repetitive real speech (phone
 * numbers, "no no no", counting) is not misfiltered. */
static bool looks_repetitive(const char *text) {
    gchar **words = g_strsplit(text, " ", -1);
    int total = 0;
    for (int i = 0; words[i] != NULL; i++)
        if (words[i][0] != '\0')
            total++;
    if (total < 16) {
        g_strfreev(words);
        return false;
    }
    int distinct = 0;
    for (int i = 0; words[i] != NULL; i++) {
        if (words[i][0] == '\0')
            continue;
        bool seen = false;
        for (int j = 0; j < i; j++) {
            if (words[j][0] != '\0' &&
                g_ascii_strcasecmp(words[i], words[j]) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen)
            distinct++;
    }
    g_strfreev(words);
    /* <= 20% unique words over a long phrase => hallucinated loop */
    return distinct * 5 <= total;
}

/* Run whisper on utt[0..n_samples) with per-utterance normalization, a
 * dynamic encoder context (cost scales with the actual audio length, which
 * keeps streaming partials cheap), and hallucination filtering. Writes the
 * transcribed text to `out` (also on the filtered path, for logging) and
 * returns true only when the text is displayable. `out_ms` (optional) gets
 * the inference time. */
static bool transcribe_buffer(struct whisper_context *ctx,
                              const int16_t *utt,
                              int n_samples,
                              float *pcmf,
                              char *out,
                              size_t out_sz,
                              gint64 *out_ms,
                              float noise_floor) {
    out[0] = '\0';
    if (out_ms != NULL)
        *out_ms = 0;

    struct config cfg;
    cfg_snapshot(&cfg);

    /* whisper needs at least ~1 s of audio; pad with silence */
    int n = n_samples;
    for (int i = 0; i < n; i++)
        pcmf[i] = (float)utt[i] / 32768.0f;

    /* Per-utterance peak normalization. Genuinely quiet speech is boosted so
     * whisper sees a strong, consistent level. Near-silence is NOT boosted:
     * in a quiet room the mic noise floor would otherwise be amplified to
     * full scale and whisper hallucinates movie-caption text on it. Anything
     * that never rises above SIGNAL_PEAK_MIN is treated as noise and skipped. */
    float peak = 1e-6f;
    for (int i = 0; i < n_samples; i++) {
        float a = fabsf(pcmf[i]);
        if (a > peak)
            peak = a;
    }
    float skip_thresh = noise_floor * cfg.snr_margin;
    if (skip_thresh < cfg.peak_min)
        skip_thresh = cfg.peak_min;
    if (peak < skip_thresh)
        return false; /* below the adaptive noise guard: do not transcribe */
    float norm = 0.85f / peak;
    if (norm > cfg.max_gain)
        norm = cfg.max_gain;
    if (norm > 1.0f)
        for (int i = 0; i < n_samples; i++)
            pcmf[i] *= norm;

    while (n < SAMPLE_RATE + SAMPLE_RATE / 5)
        pcmf[n++] = 0.0f;

    struct whisper_full_params wp =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = cfg.n_threads;
    wp.language = cfg.language;
    wp.translate = cfg.translate;
    wp.no_context = true;
    wp.single_segment = true;
    wp.no_timestamps = true;
    wp.print_realtime = false;
    wp.print_progress = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    wp.suppress_blank = true;
    /* Single greedy decode by default, no temperature fallback. Fallback
     * re-decodes a degenerate segment up to ~6 times at rising temperatures;
     * on the unintelligible background speech common to CCTV installs that
     * costs many seconds (observed 8-68 s) per utterance, which makes the
     * transcriber fall behind and drop audio. Repetition loops are caught
     * cheaply afterwards by looks_repetitive() instead. Users can opt into
     * fallback (better accuracy on hard speech) via the settings. */
    wp.temperature_inc = cfg.temp_fallback ? 0.2f : 0.0f;
    /* Cap decoded tokens per segment. On unintelligible audio whisper never
     * emits an end-of-text token and greedily decodes to its internal
     * maximum (~224 tokens), which makes a single pass take many seconds and
     * causes the transcriber to fall behind and drop audio. */
    wp.max_tokens = cfg.max_tokens;
    /* Encoder context sized to the audio (~50 mel-ctx units per second) plus
     * margin, capped at the full 1500. Short partial buffers encode far
     * faster than the fixed 30 s window, which is what makes ~1 s streaming
     * updates practical. */
    int actx = (int)((double)n_samples / SAMPLE_RATE * 50.0) + 32;
    if (actx > 1500)
        actx = 1500;
    wp.audio_ctx = actx;

    gint64 t0 = g_get_monotonic_time();
    if (whisper_full(ctx, wp, pcmf, n) != 0) {
        syslog(LOG_WARNING, "whisper_full failed");
        return false;
    }
    if (out_ms != NULL)
        *out_ms = (g_get_monotonic_time() - t0) / 1000;

    char text[SUBTITLE_MAX] = "";
    int n_seg = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_seg; i++) {
        const char *seg = whisper_full_get_segment_text(ctx, i);
        if (seg != NULL) {
            if (text[0] != '\0')
                g_strlcat(text, " ", sizeof(text));
            g_strlcat(text, seg, sizeof(text));
        }
    }
    g_strstrip(text);
    g_strlcpy(out, text, out_sz);

    /* Drop typical non-speech hallucinations: whole-line annotations like
     * "[BLANK_AUDIO]" or "(bell dings)", musical notes, and repetition
     * loops. Lines that merely start with an accented word or contain a
     * parenthetical are kept. */
    if (text[0] == '\0' || is_nonspeech_annotation(text) ||
        looks_repetitive(text))
        return false;
    return true;
}

/* Final pass for a completed utterance: transcribe, log, and post. */
static void transcribe_utterance(struct whisper_context *ctx,
                                 const int16_t *utt,
                                 int n_samples,
                                 float *pcmf,
                                 float rms,
                                 float noise_floor) {
    char text[SUBTITLE_MAX];
    gint64 ms = 0;
    bool ok = transcribe_buffer(ctx, utt, n_samples, pcmf, text, sizeof(text),
                                &ms, noise_floor);
    if (ok) {
        syslog(LOG_INFO,
               "[%.1f s final, %" G_GINT64_FORMAT " ms, rms %.4f] %s",
               (double)n_samples / SAMPLE_RATE, ms, rms, text);
        post_subtitle(text);
        transcript_publish(text, TRUE);
    } else {
        syslog(LOG_INFO, "[%.1f s final, rms %.4f] filtered/skipped: \"%s\"",
               (double)n_samples / SAMPLE_RATE, rms, text);
    }
}

static void *transcribe_thread(void *arg) {
    (void)arg;
    /* Yield to the encoder/analytics under contention, but only slightly:
     * a real microphone triggers inference intermittently, so a heavy nice
     * handicap just slows each transcription for no benefit. (Was +5, chosen
     * when constant mic noise triggered whisper nonstop.) */
    setpriority(PRIO_PROCESS, 0, 1);

    whisper_log_set(whisper_log_cb, NULL);

    /* Use whichever ggml-*.bin model is bundled (set via the Dockerfile
     * MODEL arg), so the model can be swapped without changing code. */
    gchar *model_path = NULL;
    GError *derr = NULL;
    if (g_model_path_override != NULL)
        model_path = g_strdup(g_model_path_override);
    GDir *dir = model_path == NULL ? g_dir_open(PKG_DIR, 0, &derr) : NULL;
    if (dir != NULL) {
        const gchar *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (g_str_has_prefix(name, "ggml-") && g_str_has_suffix(name, ".bin")) {
                model_path = g_build_filename(PKG_DIR, name, NULL);
                break;
            }
        }
        g_dir_close(dir);
    } else if (derr != NULL) {
        g_clear_error(&derr);
    }
    if (model_path == NULL)
        model_path = g_strdup(MODEL_FALLBACK);

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    struct whisper_context *ctx =
        whisper_init_from_file_with_params(model_path, cparams);
    if (ctx == NULL) {
        syslog(LOG_ERR, "failed to load whisper model %s", model_path);
        g_free(model_path);
        g_idle_add(quit_idle_cb, NULL);
        return NULL;
    }
    syslog(LOG_INFO, "whisper model loaded (%s)", model_path);
    g_free(model_path);

    const int max_samples = SAMPLE_RATE * MAX_UTTER_SECONDS_CAP +
                            FRAME_SAMPLES * (PREROLL_FRAMES + 1);
    int16_t *utt = g_malloc(max_samples * sizeof(int16_t));
    float *pcmf = g_malloc((max_samples + 2 * SAMPLE_RATE) * sizeof(float));
    int16_t frame[FRAME_SAMPLES];
    int16_t preroll[PREROLL_FRAMES][FRAME_SAMPLES];
    int preroll_n = 0;
    int utt_len = 0; /* samples collected; 0 = idle, waiting for speech */
    int speech_frames = 0;
    int silence_run = 0;
    int last_partial = 0; /* utt_len at last streaming partial */
    double utt_energy = 0.0;
    float noise_floor = 0.004f;

    /* Skip any audio captured while the model was loading */
    pthread_mutex_lock(&g_ring_lock);
    uint64_t next = g_written;
    pthread_mutex_unlock(&g_ring_lock);

    while (g_running) {
        int r = read_frame(&next, frame);
        if (r < 0)
            break;
        if (r > 0) { /* audio dropped: restart segmentation cleanly */
            utt_len = 0;
            speech_frames = 0;
            silence_run = 0;
            preroll_n = 0;
            last_partial = 0;
            utt_energy = 0.0;
            continue;
        }

        struct config cfg;
        cfg_snapshot(&cfg);
        int max_utter_samples = cfg.max_utter_seconds * SAMPLE_RATE;

        double sum_sq = 0.0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float f = (float)frame[i] / 32768.0f;
            sum_sq += (double)f * f;
        }
        float rms = sqrtf((float)(sum_sq / FRAME_SAMPLES));
        float gate = noise_floor * cfg.vad_snr;
        if (gate < cfg.vad_abs_floor)
            gate = cfg.vad_abs_floor;

        if (utt_len == 0) {
            if (rms <= gate) {
                /* Idle: adapt the noise floor and keep pre-roll context */
                noise_floor = 0.97f * noise_floor + 0.03f * rms;
                if (noise_floor < NOISE_FLOOR_MIN)
                    noise_floor = NOISE_FLOOR_MIN;
                if (noise_floor > NOISE_FLOOR_MAX)
                    noise_floor = NOISE_FLOOR_MAX;
                if (preroll_n == PREROLL_FRAMES) {
                    memmove(preroll[0], preroll[1],
                            (PREROLL_FRAMES - 1) * sizeof(preroll[0]));
                    preroll_n--;
                }
                memcpy(preroll[preroll_n++], frame, sizeof(frame));
                continue;
            }
            /* Speech onset: start an utterance with the pre-roll */
            for (int i = 0; i < preroll_n; i++) {
                memcpy(utt + utt_len, preroll[i], sizeof(preroll[0]));
                utt_len += FRAME_SAMPLES;
            }
            preroll_n = 0;
            speech_frames = 0;
            silence_run = 0;
            utt_energy = 0.0;
        }

        memcpy(utt + utt_len, frame, sizeof(frame));
        utt_len += FRAME_SAMPLES;
        utt_energy += sum_sq;
        if (rms > gate) {
            speech_frames++;
            silence_run = 0;
        } else {
            silence_run++;
        }

        if (silence_run < cfg.hang_frames &&
            utt_len < max_utter_samples &&
            utt_len + FRAME_SAMPLES <= max_samples) {
            /* Utterance still in progress. In streaming mode, re-transcribe
             * the audio so far every STREAM_STEP_SAMPLES and update the
             * on-screen caption so it builds up live as the person speaks -
             * but only while the transcriber is keeping up. If it has fallen
             * behind (busy/continuous audio), skip the partial and let the
             * reader drain the backlog; the final pass still captions the
             * utterance. */
            bool keeping_up = true;
            if (cfg.streaming) {
                pthread_mutex_lock(&g_ring_lock);
                uint64_t backlog = g_written - next;
                pthread_mutex_unlock(&g_ring_lock);
                keeping_up = backlog < STREAM_MAX_BACKLOG_SAMPLES;
            }
            if (cfg.streaming && keeping_up &&
                speech_frames >= cfg.min_speech_frames &&
                utt_len - last_partial >= cfg.stream_step_samples) {
                char part[SUBTITLE_MAX];
                gint64 pms = 0;
                if (transcribe_buffer(ctx, utt, utt_len, pcmf, part,
                                      sizeof(part), &pms, noise_floor)) {
                    syslog(LOG_INFO,
                           "[%.1f s partial, %" G_GINT64_FORMAT " ms] %s",
                           (double)utt_len / SAMPLE_RATE, pms, part);
                    post_subtitle(part);
                    transcript_publish(part, FALSE);
                }
                last_partial = utt_len;
            }
            continue;
        }

        if (speech_frames >= cfg.min_speech_frames) {
            float utt_rms = sqrtf((float)(utt_energy / utt_len));
            transcribe_utterance(ctx, utt, utt_len, pcmf, utt_rms, noise_floor);
        } else {
            syslog(LOG_INFO,
                   "utterance discarded: %.1f s, %d speech frames "
                   "(gate %.4f, noise floor %.4f)",
                   (double)utt_len / SAMPLE_RATE, speech_frames, gate,
                   noise_floor);
        }
        utt_len = 0;
        speech_frames = 0;
        silence_run = 0;
        last_partial = 0;
        utt_energy = 0.0;
    }

    g_free(utt);
    g_free(pcmf);
    whisper_free(ctx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */

static gboolean signal_handler_cb(gpointer user_data) {
    (void)user_data;
    syslog(LOG_INFO, "received stop signal");
    g_running = false;
    pthread_cond_broadcast(&g_ring_cond);
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static bool load_overlay_symbol(void *handle, void **slot, const char *name) {
    dlerror();
    *slot = dlsym(handle, name);
    if (*slot != NULL)
        return true;

    syslog(LOG_WARNING, "missing overlay symbol %s: %s", name,
           dlerror() != NULL ? dlerror() : "unknown");
    return false;
}

static void unload_overlay_api(void) {
    if (g_axoverlay.handle != NULL)
        dlclose(g_axoverlay.handle);
    memset(&g_axoverlay, 0, sizeof(g_axoverlay));
    if (g_cairo.handle != NULL)
        dlclose(g_cairo.handle);
    memset(&g_cairo, 0, sizeof(g_cairo));
    g_overlay_available = false;
    g_overlay_id = -1;
}

static bool load_cairo_api(void) {
    if (g_cairo.handle != NULL)
        return true;

    g_cairo.handle = dlopen("libcairo.so.2", RTLD_NOW | RTLD_LOCAL);
    if (g_cairo.handle == NULL) {
        syslog(LOG_WARNING, "cairo unavailable: %s", dlerror());
        return false;
    }

    void *h = g_cairo.handle;
    if (!load_overlay_symbol(h, (void **)&g_cairo.set_operator,
                             "cairo_set_operator") ||
        !load_overlay_symbol(h, (void **)&g_cairo.set_source_rgba,
                             "cairo_set_source_rgba") ||
        !load_overlay_symbol(h, (void **)&g_cairo.paint, "cairo_paint") ||
        !load_overlay_symbol(h, (void **)&g_cairo.select_font_face,
                             "cairo_select_font_face") ||
        !load_overlay_symbol(h, (void **)&g_cairo.set_font_size,
                             "cairo_set_font_size") ||
        !load_overlay_symbol(h, (void **)&g_cairo.text_extents,
                             "cairo_text_extents") ||
        !load_overlay_symbol(h, (void **)&g_cairo.rectangle,
                             "cairo_rectangle") ||
        !load_overlay_symbol(h, (void **)&g_cairo.fill, "cairo_fill") ||
        !load_overlay_symbol(h, (void **)&g_cairo.move_to, "cairo_move_to") ||
        !load_overlay_symbol(h, (void **)&g_cairo.show_text,
                             "cairo_show_text")) {
        return false;
    }

    return true;
}

static bool load_overlay_api(void) {
    if (g_axoverlay.handle != NULL)
        return true;

    g_axoverlay.handle = dlopen("libaxoverlay.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_axoverlay.handle == NULL) {
        syslog(LOG_WARNING, "axoverlay unavailable: %s", dlerror());
        return false;
    }

    void *h = g_axoverlay.handle;
    if (!load_overlay_symbol(h, (void **)&g_axoverlay.redraw,
                             "axoverlay_redraw") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.is_backend_supported,
                             "axoverlay_is_backend_supported") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.init_settings,
                             "axoverlay_init_axoverlay_settings") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.init,
                             "axoverlay_init") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.init_overlay_data,
                             "axoverlay_init_overlay_data") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.get_max_resolution_width,
                             "axoverlay_get_max_resolution_width") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.get_max_resolution_height,
                             "axoverlay_get_max_resolution_height") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.create_overlay,
                             "axoverlay_create_overlay") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.destroy_overlay,
                             "axoverlay_destroy_overlay") ||
        !load_overlay_symbol(h, (void **)&g_axoverlay.cleanup,
                             "axoverlay_cleanup") ||
        !load_cairo_api()) {
        unload_overlay_api();
        return false;
    }

    return true;
}

static void cleanup_overlay(void) {
    GError *error = NULL;

    if (g_overlay_available && g_overlay_id >= 0)
        g_axoverlay.destroy_overlay(g_overlay_id, &error);
    if (error != NULL) {
        syslog(LOG_WARNING, "axoverlay_destroy_overlay failed: %s",
               error->message);
        g_error_free(error);
    }

    if (g_axoverlay.handle != NULL)
        g_axoverlay.cleanup();
    unload_overlay_api();
}

static bool setup_overlay(void) {
    GError *error = NULL;

    if (!load_overlay_api())
        return false;

    if (!g_axoverlay.is_backend_supported(AXOVERLAY_CAIRO_IMAGE_BACKEND)) {
        syslog(LOG_WARNING,
               "cairo overlay backend not supported on this device");
        cleanup_overlay();
        return false;
    }

    struct axoverlay_settings settings;
    g_axoverlay.init_settings(&settings);
    settings.render_callback = render_overlay_cb;
    settings.adjustment_callback = adjustment_cb;
    settings.select_callback = NULL;
    settings.backend = AXOVERLAY_CAIRO_IMAGE_BACKEND;
    g_axoverlay.init(&settings, &error);
    if (error != NULL) {
        syslog(LOG_WARNING, "axoverlay_init failed: %s", error->message);
        g_error_free(error);
        cleanup_overlay();
        return false;
    }

    struct axoverlay_overlay_data data;
    g_axoverlay.init_overlay_data(&data);
    data.postype = AXOVERLAY_CUSTOM_NORMALIZED;
    data.anchor_point = AXOVERLAY_ANCHOR_CENTER;
    data.x = 0.0;
    data.y = g_subtitle_top ? 0.15 : 0.75; /* top or bottom of the frame */
    data.scale_to_stream = FALSE;
    data.colorspace = AXOVERLAY_COLORSPACE_ARGB32;

    /* Initial size; the adjustment callback resizes per stream */
    data.width = g_axoverlay.get_max_resolution_width(1, &error);
    if (error != NULL) {
        data.width = 1920;
        g_clear_error(&error);
    }
    data.height = g_axoverlay.get_max_resolution_height(1, &error);
    if (error != NULL) {
        data.height = 1080;
        g_clear_error(&error);
    }
    struct config cfg0;
    cfg_snapshot(&cfg0);
    data.height = (gint)(data.height * cfg0.bar_height_frac);

    g_overlay_id = g_axoverlay.create_overlay(&data, NULL, &error);
    if (error != NULL) {
        syslog(LOG_WARNING, "axoverlay_create_overlay failed: %s",
               error->message);
        g_error_free(error);
        cleanup_overlay();
        return false;
    }

    g_overlay_available = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* User-supplied model                                                 */
/* ------------------------------------------------------------------ */

/* Read an application parameter; returns a newly allocated string (possibly
 * NULL). Caller frees. */
static gchar *param_get(AXParameter *params, const char *name) {
    if (params == NULL)
        return NULL;
    gchar *value = NULL;
    if (!ax_parameter_get(params, name, &value, NULL))
        return NULL;
    return value;
}

/* Download the GGML model at url into the app's writable localdata directory
 * (cached by URL so it is fetched only once) and return its path, or any
 * previously downloaded model on failure. Returns NULL when url is empty or
 * nothing is available. Caller frees. */
static gchar *download_user_model(const char *url) {
    if (url == NULL || url[0] == '\0')
        return NULL;

    const char *model_path = PKG_DIR "/localdata/user-model.bin";
    const char *url_path = PKG_DIR "/localdata/user-model.url";
    const char *tmp_path = PKG_DIR "/localdata/user-model.tmp";

    gchar *cached_url = NULL;
    if (g_file_get_contents(url_path, &cached_url, NULL, NULL) &&
        cached_url != NULL) {
        g_strstrip(cached_url);
        if (g_strcmp0(cached_url, url) == 0 &&
            g_file_test(model_path, G_FILE_TEST_EXISTS)) {
            g_free(cached_url);
            syslog(LOG_INFO, "using cached user model (%s)", url);
            return g_strdup(model_path);
        }
    }
    g_free(cached_url);

    syslog(LOG_INFO, "downloading model from %s", url);
    FILE *fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        syslog(LOG_ERR, "cannot open %s for writing", tmp_path);
        return g_file_test(model_path, G_FILE_TEST_EXISTS)
                   ? g_strdup(model_path)
                   : NULL;
    }

    CURL *curl = curl_easy_init();
    CURLcode res = CURLE_FAILED_INIT;
    if (curl != NULL) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    fclose(fp);

    if (res != CURLE_OK) {
        syslog(LOG_WARNING, "model download failed: %s",
               curl_easy_strerror(res));
        g_unlink(tmp_path);
        return g_file_test(model_path, G_FILE_TEST_EXISTS)
                   ? g_strdup(model_path)
                   : NULL;
    }
    if (g_rename(tmp_path, model_path) != 0) {
        syslog(LOG_WARNING, "could not store downloaded model");
        g_unlink(tmp_path);
        return NULL;
    }
    g_file_set_contents(url_path, url, -1, NULL);
    syslog(LOG_INFO, "user model ready");
    return g_strdup(model_path);
}

/* Full set of app settings with their defaults, mirroring the manifest
 * paramConfig. This is the source of truth for the app's own Settings web page
 * and is used to seed the settings file on devices without the VAPIX parameter
 * system (e.g. recorders such as the S3008). */
struct setting_def {
    const char *name;
    const char *def;
};

static const struct setting_def SETTINGS[] = {
    {"MicSensitivity", "Normal"},      {"SilenceTimeoutMs", "800"},
    {"MinSpeechMs", "300"},            {"MaxUtteranceSec", "6"},
    {"SubtitleDurationSec", "6"},      {"FontScale", "Medium"},
    {"SubtitlePosition", "Bottom"},    {"StreamingCaptions", "yes"},
    {"SubtitlesEnabled", "yes"},       {"MaxLines", "2"},
    {"BarHeightPct", "20"},            {"BackgroundOpacity", "55"},
    {"TextColor", "#FFFFFF"},          {"InferenceThreads", "3"},
    {"Language", "en"},                {"Translate", "no"},
    {"MaxTokens", "64"},               {"TemperatureFallback", "no"},
    {"NoiseGuardMargin", "8.0"},       {"MinSignalPeak", "0.02"},
    {"MaxGain", "4.0"},                {"StreamStepMs", "900"},
    {"ModelUrl", ""},                  {"ApiEnabled", "yes"},
    {"MqttEnabled", "no"},             {"MqttHost", ""},
    {"MqttPort", "1883"},              {"MqttTls", "no"},
    {"MqttTlsVerify", "no"},           {"MqttUser", ""},
    {"MqttPass", ""},                  {"MqttTopic", "whisper/subtitles"},
    {"MqttClientId", ""},              {"MqttPublishPartials", "no"},
    {"RemoteAudioHost", ""},           {"RemoteAudioUser", ""},
    {"RemoteAudioPass", ""},           {"AudioInput", ""},
};

static const char *g_setting_names[G_N_ELEMENTS(SETTINGS)];

/* Password settings: never written to the settings file or returned to the
 * browser. They are read from the device parameter system at runtime, or set
 * (write-only) from the Settings page. */
static const char *const SECRET_SETTINGS[] = {
    "MqttPass",
    "RemoteAudioPass",
};

static gboolean is_secret_setting(const char *name) {
    for (guint i = 0; i < G_N_ELEMENTS(SECRET_SETTINGS); i++)
        if (g_strcmp0(name, SECRET_SETTINGS[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Resolve a setting: a non-empty settings-file value wins (the source of
 * truth, available on every device), otherwise the VAPIX parameter system
 * (axparameter). An empty file value falls through so unset/secret values are
 * taken from the device parameter system where present. Caller frees. */
static gchar *setting_read(AXParameter *params, const char *name) {
    gchar *v = settings_get(name);
    if (v != NULL && v[0] != '\0')
        return v;
    g_free(v);
    return param_get(params, name);
}

/* Apply a setting written through the config endpoint. Live-tunable settings
 * take effect immediately; startup-only ones are ignored here (they are read
 * at startup) and take effect on the next app restart. */
static void apply_setting_cb(const char *name, const char *value, void *user) {
    (void)user;
    cfg_apply(name, value);
}

int main(void) {
    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "starting %s", APP_NAME);

    /* Give fontconfig (used by cairo text rendering) a writable cache dir */
    setenv("XDG_CACHE_HOME", PKG_DIR "/localdata", 0);

    g_loop = g_main_loop_new(NULL, FALSE);
    transcript_init();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Settings are stored in a file in the app's writable localdata directory
     * so the Settings web page works on devices that do not serve the VAPIX
     * parameter system / param.cgi (e.g. recorders such as the S3008). The
     * device parameter system is still used for live change callbacks and as a
     * fallback where present, but this file is the source of truth. */
    settings_init(PKG_DIR "/localdata/settings.conf");
    for (guint i = 0; i < G_N_ELEMENTS(SETTINGS); i++)
        g_setting_names[i] = SETTINGS[i].name;

    /* Read settings before creating the overlay, and keep the AXParameter
     * handle alive so later VAPIX changes are delivered live to
     * param_changed_cb for the lifetime of the app.
     *
     * RemoteAudioHost set -> pull audio from another Axis device;
     * empty -> capture from this camera via PipeWire. */
    static const char *const tuning_params[] = {
        "MicSensitivity",      "SilenceTimeoutMs",    "MinSpeechMs",
        "MaxUtteranceSec",     "SubtitleDurationSec", "FontScale",
        "SubtitlePosition",    "StreamingCaptions",   "SubtitlesEnabled",
        "MaxLines",            "BarHeightPct",        "BackgroundOpacity",
        "TextColor",           "InferenceThreads",    "Language",
        "Translate",           "MaxTokens",           "TemperatureFallback",
        "NoiseGuardMargin",    "MinSignalPeak",       "MaxGain",
        "StreamStepMs",
    };
    gchar *remote_host = NULL;
    gchar *remote_user = NULL;
    gchar *remote_pass = NULL;
    gchar *model_url = NULL;
    gboolean api_enabled = TRUE;
    struct mqtt_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    GError *perr = NULL;
    AXParameter *params = ax_parameter_new(APP_NAME, &perr);
    if (params != NULL) {
        /* Register live-change callbacks for the tunable parameters where the
         * parameter system is available. */
        for (guint i = 0; i < G_N_ELEMENTS(tuning_params); i++)
            load_and_watch(params, tuning_params[i]);
    } else {
        syslog(LOG_WARNING, "axparameter unavailable: %s",
               perr != NULL ? perr->message : "unknown");
        g_clear_error(&perr);
    }

    /* Seed the settings file for any setting it does not yet contain, taking
     * an existing VAPIX value (pre-upgrade) if present, else the default, so
     * the Settings page always returns meaningful values. Passwords are never
     * written to the file; they stay in the device parameter system. */
    gboolean seeded = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(SETTINGS); i++) {
        if (is_secret_setting(SETTINGS[i].name))
            continue;
        gchar *fv = settings_get(SETTINGS[i].name);
        if (fv != NULL) {
            g_free(fv);
            continue;
        }
        gchar *av = param_get(params, SETTINGS[i].name);
        settings_set(SETTINGS[i].name, av != NULL ? av : SETTINGS[i].def);
        g_free(av);
        seeded = TRUE;
    }
    if (seeded)
        settings_save();

    /* Apply the effective values (file wins) for live-tunable settings. */
    for (guint i = 0; i < G_N_ELEMENTS(tuning_params); i++) {
        gchar *v = setting_read(params, tuning_params[i]);
        if (v != NULL) {
            cfg_apply(tuning_params[i], v);
            g_free(v);
        }
    }

    /* Startup-only settings (need an app restart to change). */
    remote_host = setting_read(params, "RemoteAudioHost");
    remote_user = setting_read(params, "RemoteAudioUser");
    remote_pass = setting_read(params, "RemoteAudioPass");
    model_url = setting_read(params, "ModelUrl");

    gchar *audio_input = setting_read(params, "AudioInput");
    if (audio_input != NULL) {
        g_strlcpy(g_audio_input, audio_input, sizeof(g_audio_input));
        g_free(audio_input);
    }

    gchar *v;
    if ((v = setting_read(params, "ApiEnabled")) != NULL) {
        api_enabled = parse_bool(v);
        g_free(v);
    }
    if ((v = setting_read(params, "MqttEnabled")) != NULL) {
        mcfg.enabled = parse_bool(v);
        g_free(v);
    }
    if ((v = setting_read(params, "MqttHost")) != NULL) {
        g_strlcpy(mcfg.host, v, sizeof(mcfg.host));
        g_free(v);
    }
    if ((v = setting_read(params, "MqttPort")) != NULL) {
        mcfg.port = atoi(v);
        g_free(v);
    }
    if ((v = setting_read(params, "MqttTls")) != NULL) {
        mcfg.tls = parse_bool(v);
        g_free(v);
    }
    if ((v = setting_read(params, "MqttTlsVerify")) != NULL) {
        mcfg.tls_verify = parse_bool(v);
        g_free(v);
    }
    if ((v = setting_read(params, "MqttUser")) != NULL) {
        g_strlcpy(mcfg.user, v, sizeof(mcfg.user));
        g_free(v);
    }
    if ((v = setting_read(params, "MqttPass")) != NULL) {
        g_strlcpy(mcfg.pass, v, sizeof(mcfg.pass));
        g_free(v);
    }
    if ((v = setting_read(params, "MqttTopic")) != NULL) {
        g_strlcpy(mcfg.topic, v, sizeof(mcfg.topic));
        g_free(v);
    }
    if ((v = setting_read(params, "MqttClientId")) != NULL) {
        g_strlcpy(mcfg.client_id, v, sizeof(mcfg.client_id));
        g_free(v);
    }
    if ((v = setting_read(params, "MqttPublishPartials")) != NULL) {
        mcfg.publish_partials = parse_bool(v);
        g_free(v);
    }

    if (!setup_overlay())
        syslog(LOG_WARNING,
               "starting without video subtitles; transcription outputs remain available");

    /* Audio source: RemoteAudioHost set -> pull from another Axis device
     * (VAPIX receive.cgi); empty -> capture from this camera's own mic
     * (PipeWire). */
    bool audio_ok;
    if (remote_host != NULL && remote_host[0] != '\0')
        audio_ok = start_remote_audio(remote_host,
                                      remote_user != NULL ? remote_user : "",
                                      remote_pass != NULL ? remote_pass : "");
    else
        audio_ok = start_audio();
    g_free(remote_host);
    g_free(remote_user);
    g_free(remote_pass);

    if (!audio_ok)
        syslog(LOG_WARNING,
               "no audio source active; the app is idle and serving its "
               "Settings page. Set RemoteAudioHost (or enable the camera mic) "
               "and restart the app.");

    g_unix_signal_add(SIGTERM, signal_handler_cb, NULL);
    g_unix_signal_add(SIGINT, signal_handler_cb, NULL);
    g_timeout_add(500, expire_cb, NULL);

    /* Resolve a user-supplied model (downloaded from ModelUrl) before the
     * transcription thread loads it. */
    g_model_path_override = download_user_model(model_url);
    g_free(model_url);

    /* Expose transcriptions to other systems, and serve the settings config
     * endpoint (admin route) so the Settings page works without param.cgi.
     * This runs even with no audio source so settings stay editable on
     * devices that need to be pointed at a remote audio source first. */
    webapi_start(API_PORT, api_enabled, CONFIG_PORT, g_setting_names,
                 G_N_ELEMENTS(SETTINGS), SECRET_SETTINGS,
                 G_N_ELEMENTS(SECRET_SETTINGS), apply_setting_cb, NULL);
    mqtt_start(&mcfg);

    /* Transcription only runs when there is an audio source; otherwise the app
     * idles (serving Settings/API) until reconfigured and restarted. */
    pthread_t transcribe_tid;
    bool transcribe_started = false;
    if (audio_ok) {
        pthread_create(&transcribe_tid, NULL, transcribe_thread, NULL);
        transcribe_started = true;
    }

    g_main_loop_run(g_loop);

    g_running = false;
    pthread_cond_broadcast(&g_ring_cond);
    webapi_stop();
    mqtt_stop();
    stop_audio();
    if (g_remote_started)
        pthread_join(g_remote_tid, NULL);
    if (transcribe_started)
        pthread_join(transcribe_tid, NULL);
    curl_global_cleanup();

    cleanup_overlay();
    if (params != NULL)
        ax_parameter_free(params);
    g_free(g_model_path_override);
    g_main_loop_unref(g_loop);

    syslog(LOG_INFO, "stopped");
    return 0;
}
