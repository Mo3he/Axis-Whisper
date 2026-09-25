PROG = Whisper_Subtitles
SRCS = axiswhisper.c transcript.c webapi.c mqtt.c settings.c
PKGS = glib-2.0 gio-2.0 axparameter libcurl
# axoverlay, cairo and libpipewire-0.3 are dlopen'd (optional), so only their
# headers are needed; the app still runs without video or local audio.
OPTIONAL_CFLAGS = $(shell pkg-config --cflags axoverlay cairo libpipewire-0.3)

CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += $(OPTIONAL_CFLAGS)
CFLAGS += -Iwhisper/include
CFLAGS += -O2 -W -Wall -pthread
CFLAGS += $(EXTRA_CFLAGS)

LDADD  = $(shell pkg-config --libs $(PKGS))
LDADD += -Lwhisper/lib -lwhisper -lggml -lggml-cpu -lggml-base
LDADD += -lstdc++ -lm -pthread -ldl

# The static whisper.cpp/ggml C++ libs contribute ~14 MB of symbols (89% of the
# binary). Keep the unstripped copy outside the app dir so acap-build cannot
# package it, but crashes can still be symbolised with addr2line.
STRIP ?= strip
DEBUGDIR ?= /opt/debug

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)
	mkdir -p $(DEBUGDIR) && cp $@ $(DEBUGDIR)/$@.unstripped
	$(STRIP) $@

clean:
	rm -f $(PROG) *.eap *_LICENSE.txt
