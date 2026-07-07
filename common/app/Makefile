PROG = Whisper_Subtitles
SRCS = axiswhisper.c transcript.c webapi.c mqtt.c settings.c
PKGS = glib-2.0 gio-2.0 axparameter libcurl
# axoverlay, cairo and libpipewire-0.3 are dlopen'd at runtime (all optional:
# overlay/cairo for local video subtitles, pipewire for local mic capture), so
# only their headers are needed at build time, not their link libraries. This
# lets the app load and run on devices without a video or local-audio pipeline
# (using remote audio and the HTTP/MQTT outputs).
OPTIONAL_CFLAGS = $(shell pkg-config --cflags axoverlay cairo libpipewire-0.3)

CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += $(OPTIONAL_CFLAGS)
CFLAGS += -Iwhisper/include
CFLAGS += -O2 -W -Wall -pthread
CFLAGS += $(EXTRA_CFLAGS)

LDADD  = $(shell pkg-config --libs $(PKGS))
LDADD += -Lwhisper/lib -lwhisper -lggml -lggml-cpu -lggml-base
LDADD += -lstdc++ -lm -pthread -ldl

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)

clean:
	rm -f $(PROG) *.eap *_LICENSE.txt
