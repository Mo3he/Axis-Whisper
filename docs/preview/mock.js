// Fictional settings for the offline UI; refresh the copied settings UI and sample values from the app UI when it changes.
"use strict";

const sampleSettings = {
  MicSensitivity: "Normal",
  SilenceTimeoutMs: "700",
  MinSpeechMs: "250",
  MaxUtteranceSec: "8",
  SubtitleDurationSec: "5",
  FontScale: "Medium",
  SubtitlePosition: "Bottom",
  StreamingCaptions: "yes",
  SubtitlesEnabled: "yes",
  MaxLines: "2",
  BarHeightPct: "18",
  BackgroundOpacity: "70",
  TextColor: "#ffffff",
  InferenceThreads: "2",
  Language: "en",
  Translate: "no",
  MaxTokens: "128",
  TemperatureFallback: "no",
  ModelUrl: "",
  NoiseGuardMargin: "8",
  MinSignalPeak: "0.03",
  MaxGain: "4",
  StreamStepMs: "900",
  ApiEnabled: "yes",
  MqttEnabled: "no",
  MqttHost: "",
  MqttPort: "1883",
  MqttTls: "no",
  MqttTlsVerify: "yes",
  MqttUser: "",
  MqttPass: "",
  MqttTopic: "sample/subtitles",
  MqttClientId: "",
  MqttPublishPartials: "no",
  RemoteAudioHost: "",
  RemoteAudioUser: "",
  RemoteAudioPass: "",
  AudioInput: "",
};

function textResponse(body, status = 200) {
  return new Response(body, { status, headers: { "Content-Type": "text/plain" } });
}

window.fetch = async (input) => {
  const url = new URL(typeof input === "string" ? input : input.url, location.href);
  if (url.origin !== location.origin) return textResponse("Offline preview blocks external requests.", 403);
  if (url.pathname.endsWith("/config/settings")) {
    if (url.searchParams.get("action") === "list") return textResponse(JSON.stringify(sampleSettings));
    if (url.searchParams.get("action") === "update") {
      for (const [key, value] of url.searchParams) {
        if (key !== "action" && Object.hasOwn(sampleSettings, key)) sampleSettings[key] = value;
      }
      return textResponse("OK\n");
    }
  }
  if (url.pathname.endsWith("/axis-cgi/applications/control.cgi"))
    return textResponse("Offline preview: app restart is disabled.", 403);
  return textResponse("Offline preview: this endpoint is not available.", 404);
};
