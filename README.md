# Axis Whisper Subtitles

Live speech-to-text — running **entirely on the camera**. No cloud services, no external
server, no network dependencies.

```
  Camera microphone (PipeWire)        Another Axis device's microphone
        │  16 kHz mono S16              │  VAPIX receive.cgi, G.711 u-law
        └──────────────┬────────────────┘
                       ▼
  whisper.cpp (OpenAI Whisper base.en, quantized, CPU inference)
        │  transcribed text
        ├──────────► axoverlay (Cairo) subtitle bar in all video streams
        ├──────────► HTTP API  /local/Whisper_Subtitles/api  (JSON + live SSE)
        └──────────► MQTT broker (optional)
```

## Requirements

- Axis camera with an **aarch64** SoC: ARTPEC-8 or ARTPEC-9 recommended.
  (Whisper inference is too slow to be useful on older/armv7 devices, so no
  arm build is provided.)
- AXIS OS 12.x
- A microphone — either:
  - built-in or connected to audio-in, with **audio enabled** in the camera
    settings (`Video > Audio` — it is disabled by default on many models), or
  - on **another Axis device** on the network (camera, speaker, sensor —
    anything with a mic and VAPIX audio), see *Remote audio* below.

The app also runs on devices **without a video pipeline or a local microphone**
(for example recorders such as the AXIS S3008): it transcribes remote audio and
publishes to the HTTP API and MQTT. On-screen subtitles are only shown where the
device has a video overlay; on other devices the app starts, stays running and
serves its Settings page so you can point it at a remote audio source.

On devices with more than one microphone the input is auto-selected: the app
prefers the fully processed microphone (with the device's gain/AGC) over the raw
`.Unprocessed` input and the silent internal fallback source. To force a specific
input, set **Local audio input** in Settings to part of the input name shown in
the App log (lines like `audio source available: ... name '...'`).

## Installation

1. Download the `.eap` file from the [releases page](../../releases), or build
   it yourself (below).
2. In the camera web UI go to **Apps**, click **Add app** and upload the
   `.eap` file.
3. Make sure audio is enabled on the camera (`Video > Audio > Allow audio`).
4. Start the app. After the model loads (a few seconds), speech picked up by
   the microphone appears as subtitles at the bottom of the video.

The subtitles are rendered into the video pipeline itself, so they show up in
live view, recordings, and every stream profile.

## Settings

Open **Apps > Whisper Subtitles > Settings** in the camera web UI to tune the
app for your environment. Most changes apply live (no restart needed). The
exceptions, which take effect when the app is restarted, are *Subtitle
position*, *Custom model URL*, and the *HTTP API* and *MQTT* settings.

Settings are stored by the app itself (in its own data directory), so the
Settings page works on every device — including recorders that do not serve the
VAPIX `param.cgi` parameter system.

| Setting               | Default  | Meaning                                                                 |
|-----------------------|----------|-------------------------------------------------------------------------|
| `MicSensitivity`      | Normal   | Speech gate. Raise to **High**/**Maximum** if you speak but get no subtitles (quiet or far-field mic, remote audio); lower to **Low** if noise is picked up as speech. |
| `SilenceTimeoutMs`    | 800      | Silence gap (ms) that ends an utterance.                                |
| `MinSpeechMs`         | 300      | Ignore speech bursts shorter than this (ms).                            |
| `MaxUtteranceSec`     | 6        | Longest single utterance sent to whisper (2–15 s).                      |
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
| `MaxLines`          | 2         | Maximum caption lines shown on screen (1–3).                  |
| `BarHeightPct`      | 20        | Subtitle bar height as a percentage of the frame (5–50).      |
| `BackgroundOpacity` | 55        | Opacity of the box behind the text (0–100).                   |
| `TextColor`         | `#FFFFFF` | Subtitle text colour (hex).                                   |

### Transcription engine

| Setting               | Default | Meaning                                                     |
|-----------------------|---------|-------------------------------------------------------------|
| `InferenceThreads`    | 3       | CPU threads for whisper (1–4). More is faster but competes with the video pipeline. |
| `Language`            | en      | Whisper language code (`en`, `de`, `fr`, … or `auto`). Only effective with a multilingual model. |
| `Translate`           | no      | Translate speech to English (multilingual models only).     |
| `MaxTokens`           | 64      | Upper bound on decoded tokens per phrase (16–224).          |
| `TemperatureFallback` | no      | Re-decode hard audio for accuracy. Can stall on noisy scenes; leave off for busy installs. |
| `ModelUrl`            | (empty) | URL of a custom whisper model — see [Custom model](#custom-model). Restart to apply. |

### Advanced speech tuning

| Setting            | Default | Meaning                                                        |
|--------------------|---------|----------------------------------------------------------------|
| `NoiseGuardMargin` | 8.0     | How far speech must rise above room noise to be transcribed (0.5–40). Higher = fewer false triggers. |
| `MinSignalPeak`    | 0.02    | Absolute level below which audio is treated as noise (0–0.5).  |
| `MaxGain`          | 4.0     | Cap on how much quiet speech is amplified (1–10).              |
| `StreamStepMs`     | 900     | How often live captions refresh while speaking (300–3000 ms). Lower = more responsive, more CPU. |

## Remote audio

If the camera running the app has no microphone, it can pull audio from any
other Axis device on the network. Set the app parameters (Apps > Whisper
Subtitles > Settings, or via VAPIX):

On devices without a video pipeline or `axoverlay`, the app now still starts
and transcribes remote or local audio, but burned-in on-stream subtitles are
unavailable. In that mode, use the HTTP API and/or MQTT outputs instead.

| Parameter         | Example         | Meaning                                  |
|-------------------|-----------------|------------------------------------------|
| `RemoteAudioHost` | `192.168.0.246` | Axis device to pull audio from. Empty = use this camera's own mic. |
| `RemoteAudioUser` | `admin`         | Credentials on the remote device.        |
| `RemoteAudioPass` | `…`             |                                          |

```sh
curl --digest -u admin:pass "http://<camera>/axis-cgi/param.cgi?action=update\
&Whisper_Subtitles.RemoteAudioHost=192.168.0.246\
&Whisper_Subtitles.RemoteAudioUser=admin\
&Whisper_Subtitles.RemoteAudioPass=secret"
```

Restart the app after changing parameters. The app configures the remote
device for G.711 streaming automatically and reconnects with backoff if the
stream drops. This is the only change the app makes to the remote device: a
single best-effort `param.cgi?action=update` that sets `Audio.A0.Enabled=yes`
and `AudioSource.A0.AudioEncoding=g711`. Remote audio is 8 kHz telephone
bandwidth, so expect slightly lower accuracy than a local 16 kHz microphone.

## Transcription API

The app exposes an HTTP API so other systems can consume the transcriptions —
with or without on-screen subtitles. It is served through the camera's own web
server, so it uses the **same login and TLS as the rest of the device** (any
user with *viewer* rights or higher). Enable/disable it with the `ApiEnabled`
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
`{"text":"…","timestampMs":…,"final":true}`. The client reconnects
automatically with backoff if the broker connection drops.

> By default a TLS connection does **not** validate the broker's certificate,
> which suits self-signed brokers on a trusted network. Set `MqttTlsVerify=yes`
> to require a valid certificate chain. The broker password is stored with the
> device's protected `password` parameter type (like `RemoteAudioPass`).

## Custom model

The bundled `base.en` model works well for English. To use a different model —
a smaller/faster one, or a multilingual one — set `ModelUrl` to a direct link
to a whisper.cpp `ggml` `.bin` file and restart the app. The model is
downloaded once to the camera's writable storage, cached, and loaded in
preference to the bundled model.

```sh
curl --anyauth -u admin:pass "http://<camera>/axis-cgi/param.cgi?action=update\
&Whisper_Subtitles.ModelUrl=https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin"
```

For a multilingual model, also set `Language` (or `auto`) and, if you want
English output from non-English speech, `Translate=yes`. Larger models are
slower; `small` and up are generally too slow for live captioning on-device.

## Building

Build from the repository root (Docker and Podman both work):

```sh
# Docker
docker build -f aarch64/Dockerfile --tag whisper_subtitles .
CID=$(docker create whisper_subtitles) && docker cp "$CID":/opt/app ./build && docker rm "$CID"

# Podman
podman build -f aarch64/Dockerfile --tag whisper_subtitles .
CID=$(podman create whisper_subtitles) && podman cp "$CID":/opt/app ./build && podman rm "$CID"

ls build/*.eap
```

The Dockerfile cross-compiles [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
with the ACAP Native SDK toolchain, downloads the quantized `base.en` Whisper
model (~58 MB), and packages everything into a single self-contained `.eap`.

## Tuning

Almost everything is now tunable at runtime from the **Settings** page (see
[Settings](#settings)) or via `param.cgi` — including inference threads,
language, the model itself ([`ModelUrl`](#custom-model)), the speech gate,
silence timeout, utterance length, and all subtitle appearance options. No
rebuild is needed for these.

The only build-time knob left is the model bundled into the `.eap`:

| Setting              | Default      | Meaning                                                        |
|----------------------|--------------|----------------------------------------------------------------|
| `MODEL` (Dockerfile) | base.en q5_1 | Model baked into the `.eap`. `tiny.en-q5_1` = faster/lower latency, `small.en-q5_1` = most accurate but too slow for live. The app auto-detects whichever `ggml-*.bin` is bundled, and a runtime [`ModelUrl`](#custom-model) overrides it. |

The encoder context is sized automatically to each utterance's length, so
short streaming updates stay fast without a fixed accuracy/speed knob.

Speech is segmented by an energy VAD with an adaptive noise floor: an
utterance starts when a 100 ms frame exceeds the gate (300 ms of pre-roll is
prepended so word onsets are not clipped) and ends after the silence timeout,
so whisper always sees whole phrases.

## Expectations & limitations

- **Latency:** subtitles trail speech by roughly 2–5 seconds on ARTPEC-8 —
  think "live captions", not lip-sync.
- **Accuracy:** `base.en` is a small model listening to a far-field camera
  microphone. It performs well with clear speech within a few meters; noisy
  scenes will produce mistakes. An adaptive noise guard raises the speech
  threshold as the room gets louder, so quiet rooms still pick up soft speech
  while noisy rooms only transcribe clear foreground speech. Obvious non-speech
  hallucinations (`[BLANK_AUDIO]`, `(music)`, `*sighs*`, ...) are filtered out.
- **Multiple speakers / background chatter:** the app transcribes whatever
  speech it hears and cannot tell a target talker from background
  conversation. In a busy scene it will caption background voices too. For a
  specific talker, use a close or directional microphone (for example a door
  station or intercom) so that talker dominates the audio.
- **Busy scenes stay live:** under continuous speech the transcriber skips the
  live word-by-word partial updates and emits one caption per phrase instead,
  so it keeps up with the audio rather than falling behind and dropping it.
- **CPU:** inference uses 2-3 cores while speech is present. The app runs at
  lowered priority and gates on silence, but expect it to compete with heavy
  analytics apps.
- **English only** with the bundled `base.en` model. Multilingual models work —
  point [`ModelUrl`](#custom-model) at one and set `Language` (or `auto`) — but
  are slower.

## Credits

- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) by Georgi Gerganov
  and contributors (MIT)
- [OpenAI Whisper](https://github.com/openai/whisper) models (MIT)
- [PipeWire](https://pipewire.org) (MIT)

## License

BSD 3-Clause — see [LICENSE](LICENSE).
