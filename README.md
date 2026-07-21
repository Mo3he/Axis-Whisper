# Whisper ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis-Whisper?style=flat)](https://github.com/Mo3he/Axis-Whisper/releases)
[![License](https://img.shields.io/github/license/Mo3he/Axis-Whisper?style=flat)](LICENSE)
[![Build](https://github.com/Mo3he/Axis-Whisper/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis-Whisper/actions/workflows/build.yml)
[![Super-Linter](https://github.com/Mo3he/Axis-Whisper/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis-Whisper/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

Live speech-to-text, running **entirely on the camera**. No cloud services, no
external server, no network dependencies.

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB or the whisper.cpp project. Use at your own risk.

## Table of Contents

- [Overview](#overview)
- [Compatibility](#compatibility)
- [Installation](#installation)
- [Configuration](#configuration)
- [Remote audio](#remote-audio)
- [Custom model](#custom-model)
- [API](#api)
- [MQTT](#mqtt)
- [Ports & security](#ports--security)
- [Build from source](#build-from-source)
- [Tuning](#tuning)
- [Limitations](#limitations)
- [Links](#links)
- [License](#license)

## Overview

```text
  Camera microphone (PipeWire)        Another Axis device's microphone
        |  16 kHz mono S16              |  VAPIX receive.cgi, G.711 u-law
        +--------------+----------------+
                       v
  whisper.cpp (OpenAI Whisper base.en, quantized, CPU inference)
        |  transcribed text
        +---------> axoverlay (Cairo) subtitle bar in all video streams
        +---------> HTTP API  /local/Whisper_Subtitles/api  (JSON + live SSE)
        +---------> MQTT broker (optional)
```

Speech picked up by the camera (or another Axis device) is transcribed on-device
by whisper.cpp and rendered as a subtitle bar into every video stream, and/or
published over an HTTP API and MQTT.

## Compatibility

- **AXIS OS:** 12.x through 13.
- **Architecture:** `aarch64` only (ARTPEC-8/9 recommended; Whisper inference is
  too slow to be useful on armv7 devices, so no arm build is provided).
- **Requires:** a microphone, either built-in or connected to audio-in (with **audio
  enabled** in `Video > Audio`, disabled by default on many models), or on
  another Axis device with a mic and VAPIX audio (see [Remote audio](#remote-audio)).

The app also runs on devices **without a video pipeline or a local microphone**
(for example recorders such as the AXIS S3008): it transcribes remote audio and
publishes to the HTTP API and MQTT. On-screen subtitles are only shown where the
device has a video overlay.

## Installation

> **Signed packages:** Release `.eap` files are signed with the Axis ACAP
> signing service and install normally on AXIS OS 12.10 and later.
>
> **Upgrading from an earlier version?** The signing vendor changed, so
> installing over a previously installed unsigned build can fail with
> **"Couldn't install: app"** (device log: *"Vendor ID in manifest does not
> match the vendor ID of the previous version"*). To upgrade: back up your app
> configuration, **uninstall** the old version, then install the signed one.

Download the `.eap` from the [releases page](https://github.com/Mo3he/Axis-Whisper/releases) (or build it yourself,
see [Build from source](#build-from-source)) and install it via the camera's web
interface under **Apps -> Add app**. Make sure audio is enabled on the camera
(`Video > Audio > Allow audio`), then start the app. After the model loads (a few
seconds), speech picked up by the microphone appears as subtitles at the bottom of
the video.

The subtitles are rendered into the video pipeline itself, so they show up in
live view, recordings, and every stream profile.

## Configuration

Open **Apps > Whisper Subtitles > Settings** in the camera web UI to tune the app
for your environment. Most changes apply live (no restart needed). The
exceptions, which take effect when the app is restarted, are *Subtitle position*,
*Custom model URL*, and the *HTTP API* and *MQTT* settings.

Settings are stored by the app itself (in its own data directory), so the
Settings page works on every device, including recorders that do not serve the
VAPIX `param.cgi` parameter system.

| Setting               | Default  | Meaning                                                                 |
|-----------------------|----------|-------------------------------------------------------------------------|
| `MicSensitivity`      | Normal   | Speech gate. Raise to **High**/**Maximum** if you speak but get no subtitles; lower to **Low** if noise is picked up as speech. |
| `SilenceTimeoutMs`    | 800      | Silence gap (ms) that ends an utterance.                                |
| `MinSpeechMs`         | 300      | Ignore speech bursts shorter than this (ms).                            |
| `MaxUtteranceSec`     | 6        | Longest single utterance sent to whisper (2-15 s).                      |
| `SubtitleDurationSec` | 6        | How long a subtitle stays on screen after the last speech.              |
| `FontScale`           | Medium   | Subtitle text size (Small / Medium / Large).                            |
| `SubtitlePosition`    | Bottom   | Place the subtitle bar at the Bottom or Top of the frame.               |

If you are talking but nothing appears, check the app log (**Apps > Whisper
Subtitles > App log**): lines like `utterance discarded: ... speech frames
(gate ...)` mean speech is below the gate, so raise `MicSensitivity`.

The same settings can be read and changed via VAPIX `param.cgi`, e.g.:

```sh
curl --anyauth -u admin:pass "http://<camera>/axis-cgi/param.cgi?action=update\
&Whisper_Subtitles.MicSensitivity=High"
```

### Subtitle appearance

| Setting             | Default   | Meaning                                                       |
|---------------------|-----------|---------------------------------------------------------------|
| `SubtitlesEnabled`  | yes       | Show captions on the video. Turn **off** to keep transcribing (API/MQTT) without burning anything into the stream. |
| `MaxLines`          | 2         | Maximum caption lines shown on screen (1-3).                  |
| `BarHeightPct`      | 20        | Subtitle bar height as a percentage of the frame (5-50).      |
| `BackgroundOpacity` | 55        | Opacity of the box behind the text (0-100).                   |
| `TextColor`         | `#FFFFFF` | Subtitle text color (hex).                                   |

### Transcription engine

| Setting               | Default | Meaning                                                     |
|-----------------------|---------|-------------------------------------------------------------|
| `InferenceThreads`    | 3       | CPU threads for whisper (1-4). More is faster but competes with the video pipeline. |
| `Language`            | en      | Whisper language code (`en`, `de`, `fr`, ... or `auto`). Only effective with a multilingual model. |
| `Translate`           | no      | Translate speech to English (multilingual models only).     |
| `MaxTokens`           | 64      | Upper bound on decoded tokens per phrase (16-224).          |
| `TemperatureFallback` | no      | Re-decode hard audio for accuracy. Can stall on noisy scenes; leave off for busy installs. |
| `ModelUrl`            | (empty) | URL of a custom whisper model (see [Custom model](#custom-model)). Restart to apply. |

### Advanced speech tuning

| Setting            | Default | Meaning                                                        |
|--------------------|---------|----------------------------------------------------------------|
| `NoiseGuardMargin` | 8.0     | How far speech must rise above room noise to be transcribed (0.5-40). Higher = fewer false triggers. |
| `MinSignalPeak`    | 0.02    | Absolute level below which audio is treated as noise (0-0.5).  |
| `MaxGain`          | 4.0     | Cap on how much quiet speech is amplified (1-10).              |
| `StreamStepMs`     | 900     | How often live captions refresh while speaking (300-3000 ms). Lower = more responsive, more CPU. |

### Microphone input

By default the app picks the microphone automatically. Axis devices expose the
mic as several PipeWire sources: a fully processed node (`AudioDevice0Input0`,
with the device's gain/AGC), one or more raw variants
(`AudioDevice0Input0.Unprocessed`, `AudioDevice0Input0.Uncompressed`), and a
silent `dummy-source` fallback. The app prefers the processed node, which is what
you want in almost all cases, so **leave `Local audio input` empty**.

You only need this setting on a device with more than one microphone, or to force
a specific raw feed. Enter **part of the source name** (case-insensitive):

| You type                          | Selects                                   |
|-----------------------------------|-------------------------------------------|
| *(empty)*                         | Automatic, the processed mic (recommended). |
| `Input1`                          | The second input on a multi-input device. |
| `AudioDevice1`                    | A second audio device (e.g. a USB mic).   |
| `AudioDevice0Input0.Unprocessed`  | Exactly the unprocessed raw feed.         |
| `AudioDevice0Input0.Uncompressed` | Exactly the uncompressed raw feed.        |

Find the exact names your device exposes in **Apps > Whisper Subtitles > App
log**, in lines like `audio source available: id 162 name 'AudioDevice0Input0'`.
`AudioInput` is read at startup, so use **Save & restart app** to apply it.

## Remote audio

If the camera running the app has no microphone, it can pull audio from any other
Axis device on the network. Set the app parameters (Apps > Whisper Subtitles >
Settings, or via VAPIX):

| Parameter         | Example         | Meaning                                  |
|-------------------|-----------------|------------------------------------------|
| `RemoteAudioHost` | `192.168.0.246` | Axis device to pull audio from. Empty = use this camera's own mic. |
| `RemoteAudioUser` | `admin`         | Credentials on the remote device.        |
| `RemoteAudioPass` | `...`           |                                          |

```sh
curl --digest -u admin:pass "http://<camera>/axis-cgi/param.cgi?action=update\
&Whisper_Subtitles.RemoteAudioHost=192.168.0.246\
&Whisper_Subtitles.RemoteAudioUser=admin\
&Whisper_Subtitles.RemoteAudioPass=secret"
```

Restart the app after changing parameters. The app configures the remote device
for G.711 streaming automatically and reconnects with backoff if the stream
drops. This is the only change the app makes to the remote device: a single
best-effort `param.cgi?action=update` that sets `Audio.A0.Enabled=yes` and
`AudioSource.A0.AudioEncoding=g711`. Remote audio is 8 kHz telephone bandwidth, so
expect slightly lower accuracy than a local 16 kHz microphone.

## Custom model

The bundled `base.en` model works well for English. To use a different model (a
smaller/faster one, or a multilingual one), set `ModelUrl` to a direct link to a
whisper.cpp `ggml` `.bin` file and restart the app. The model is downloaded once
to the camera's writable storage, cached, and loaded in preference to the bundled
model.

```sh
curl --anyauth -u admin:pass "http://<camera>/axis-cgi/param.cgi?action=update\
&Whisper_Subtitles.ModelUrl=https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin"
```

For a multilingual model, also set `Language` (or `auto`) and, if you want
English output from non-English speech, `Translate=yes`. Larger models are
slower; `small` and up are generally too slow for live captioning on-device.

## API

The app exposes an HTTP API so other systems can consume the transcriptions,
with or without on-screen subtitles. It is served through the camera's own web
server, so it uses the **same login and TLS as the rest of the device** (any user
with *viewer* rights or higher). Enable/disable it with the `ApiEnabled`
parameter (default on; restart to apply).

Base URL: `http://<camera>/local/Whisper_Subtitles/api`

| Endpoint   | Description                                                          |
|------------|----------------------------------------------------------------------|
| `/latest`  | The most recent transcript as JSON.                                  |
| `/history` | The recent finalized transcripts as JSON (up to 50).                 |
| `/stream`  | A live [Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events) feed; one event per update. |

```sh
# Latest transcript
curl --anyauth -u admin:pass http://<camera>/local/Whisper_Subtitles/api/latest
# {"text":"hello there","timestampMs":1751731200000,"final":true}

# Recent history
curl --anyauth -u admin:pass http://<camera>/local/Whisper_Subtitles/api/history
# {"transcripts":[{"text":"...","timestampMs":...}, ...]}

# Live stream (each line: data: {json})
curl --anyauth -u admin:pass -N http://<camera>/local/Whisper_Subtitles/api/stream
# data: {"text":"hello","timestampMs":1751731200000,"final":false}
```

`final` is `false` for live partial captions that are still being built up and
`true` for a completed phrase.

## MQTT

The app can publish each transcription to an MQTT broker (MQTT 3.1.1, QoS 0).
Configure it from the Settings page or via `param.cgi`, then restart the app.

| Parameter             | Default             | Meaning                                        |
|-----------------------|---------------------|------------------------------------------------|
| `MqttEnabled`         | no                  | Enable MQTT publishing.                         |
| `MqttHost`            | (empty)             | Broker hostname or IP.                          |
| `MqttPort`            | 1883                | Broker port (typically 8883 for TLS).           |
| `MqttTls`             | no                  | Connect over TLS.                               |
| `MqttTlsVerify`       | no                  | Require a valid broker certificate chain. Leave off for self-signed brokers on a trusted network. |
| `MqttUser`            | (empty)             | Username (optional).                            |
| `MqttPass`            | (empty)             | Password (optional).                            |
| `MqttTopic`           | `whisper/subtitles` | Topic to publish to.                            |
| `MqttClientId`        | (auto)              | MQTT client id (optional).                       |
| `MqttPublishPartials` | no                  | Also publish live partial captions, not just finalized phrases. |

Each message payload is the same JSON as the API:
`{"text":"...","timestampMs":...,"final":true}`. The client reconnects
automatically with backoff if the broker connection drops.

## Ports & security

The app does not open its own network ports. The HTTP API is served through the
camera's own web server, so it inherits the device's authentication and TLS (any
user with *viewer* rights or higher). The optional MQTT client is outbound only.

> By default a TLS connection to the MQTT broker does **not** validate the
> broker's certificate, which suits self-signed brokers on a trusted network. Set
> `MqttTlsVerify=yes` to require a valid certificate chain. The broker password
> is stored with the device's protected `password` parameter type (like
> `RemoteAudioPass`).

## Build from source

Whisper is aarch64-only (the ML workload is not practical on 32-bit armv7hf). Use
the top-level `build.sh` wrapper, which builds the ACAP `.eap` package and drops
it in the repository root:

```sh
./build.sh
```

It auto-detects `docker` or `podman`; override with `RUNTIME=docker`. To build
manually instead:

```sh
# Docker
docker build -f aarch64/Dockerfile --tag whisper_subtitles .
CID=$(docker create whisper_subtitles) && docker cp "$CID":/opt/app ./build && docker rm "$CID"

# Podman
podman build -f aarch64/Dockerfile --tag whisper_subtitles .
CID=$(podman create whisper_subtitles) && podman cp "$CID":/opt/app ./build && podman rm "$CID"

ls build/*.eap
```

The Dockerfile cross-compiles
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) with the ACAP Native SDK
toolchain, downloads the quantized `base.en` Whisper model (~58 MB), and packages
everything into a single self-contained `.eap`.

## Tuning

Almost everything is now tunable at runtime from the **Settings** page (see
[Configuration](#configuration)) or via `param.cgi`, including inference
threads, language, the model itself ([`ModelUrl`](#custom-model)), the speech
gate, silence timeout, utterance length, and all subtitle appearance options. No
rebuild is needed for these.

The only build-time knob left is the model bundled into the `.eap`:

| Setting              | Default      | Meaning                                                        |
|----------------------|--------------|----------------------------------------------------------------|
| `MODEL` (Dockerfile) | base.en q5_1 | Model baked into the `.eap`. `tiny.en-q5_1` = faster/lower latency, `small.en-q5_1` = most accurate but too slow for live. The app auto-detects whichever `ggml-*.bin` is bundled, and a runtime [`ModelUrl`](#custom-model) overrides it. |

Speech is segmented by an energy VAD with an adaptive noise floor: an utterance
starts when a 100 ms frame exceeds the gate (300 ms of pre-roll is prepended so
word onsets are not clipped) and ends after the silence timeout, so whisper
always sees whole phrases.

## Limitations

- **Latency:** subtitles trail speech by roughly 2-5 seconds on ARTPEC-8; think
  "live captions", not lip-sync.
- **Accuracy:** `base.en` is a small model listening to a far-field camera
  microphone. It performs well with clear speech within a few meters; noisy
  scenes will produce mistakes. An adaptive noise guard raises the speech
  threshold as the room gets louder. Obvious non-speech hallucinations
  (`[BLANK_AUDIO]`, `(music)`, `*sighs*`, ...) are filtered out.
- **Multiple speakers / background chatter:** the app transcribes whatever speech
  it hears and cannot tell a target talker from background conversation. For a
  specific talker, use a close or directional microphone.
- **Busy scenes stay live:** under continuous speech the transcriber skips the
  live word-by-word partial updates and emits one caption per phrase instead.
- **CPU:** inference uses 2-3 cores while speech is present. The app runs at
  lowered priority and gates on silence, but expect it to compete with heavy
  analytics apps.
- **English only** with the bundled `base.en` model. Multilingual models work;
  point [`ModelUrl`](#custom-model) at one and set `Language` (or `auto`), but
  they are slower.

## Links

- [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
- [OpenAI Whisper](https://github.com/openai/whisper)
- [PipeWire](https://pipewire.org)
- [Axis Communications](https://www.axis.com/)

## License

The packaging and app code in this repository is licensed under BSD 3-Clause (see
[LICENSE](LICENSE)). Bundled upstream components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
