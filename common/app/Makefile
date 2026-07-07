PROG = Whisper_Subtitles
SRCS = axiswhisper.c transcript.c webapi.c mqtt.c
PKGS = glib-2.0 gio-2.0 cairo libpipewire-0.3 axparameter libcurl
AXOVERLAY_CFLAGS = $(shell pkg-config --cflags axoverlay)

CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += $(AXOVERLAY_CFLAGS)
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
