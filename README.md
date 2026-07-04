# Axis Whisper Subtitles

Live speech-to-text subtitles, burned directly into your Axis camera's video
streams — running **entirely on the camera**. No cloud services, no external
server, no network dependencies.

```
  Camera microphone (PipeWire)        Another Axis device's microphone
        │  16 kHz mono S16              │  VAPIX receive.cgi, G.711 u-law
        └──────────────┬────────────────┘
                       ▼
  whisper.cpp (OpenAI Whisper base.en, quantized, CPU inference)
        │  transcribed text
        ▼
  axoverlay (Cairo) — subtitle bar rendered into all video streams
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
app for your environment. Changes apply live (no restart needed), except
*Subtitle position*, which takes effect when the app is restarted.

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

## Remote audio

If the camera running the app has no microphone, it can pull audio from any
other Axis device on the network. Set the app parameters (Apps > Whisper
Subtitles > Settings, or via VAPIX):

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
device for G.711 streaming automatically (enables audio, sets the encoding)
and reconnects with backoff if the stream drops. Remote audio is 8 kHz
telephone bandwidth, so expect slightly lower accuracy than a local 16 kHz
microphone.

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
and a static alsa-lib with the ACAP Native SDK toolchain, downloads the
quantized `base.en` Whisper model (~58 MB), and packages everything into a
single self-contained `.eap`.

## Tuning

Most day-to-day tuning is done at runtime from the **Settings** page (see
above). The remaining build-time knobs live at the top of
[common/app/axiswhisper.c](common/app/axiswhisper.c):

| Define              | Default | Meaning                                                        |
|---------------------|---------|----------------------------------------------------------------|
| `N_THREADS`         | 3       | Whisper inference threads (leave a core for the video pipeline).|
| `MODEL` (Dockerfile) | base.en q5_1 | Model bundled into the `.eap`. `tiny.en-q5_1` = faster/lower latency, `small.en-q5_1` = most accurate but too slow for live. The app auto-detects whichever `ggml-*.bin` is bundled. |

The encoder context is sized automatically to each utterance's length, so
short streaming updates stay fast without a fixed accuracy/speed knob.

The speech gate, silence timeout, minimum speech length, maximum utterance
length, subtitle duration, font size and subtitle position are all runtime
[settings](#settings).

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
- **English only** with the bundled `base.en` model. Multilingual models work
  (change `MODEL` in the Dockerfile) but are slower.

## Credits

- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) by Georgi Gerganov
  and contributors (MIT)
- [OpenAI Whisper](https://github.com/openai/whisper) models (MIT)
- [alsa-lib](https://www.alsa-project.org) (LGPL-2.1)

## License

BSD 3-Clause — see [LICENSE](LICENSE).
