# Dictation extensions

This branch (`dictate-extensions`) extends Vicinae's dictation with:

| Feature | What it does |
|---|---|
| [Bigger HUD](#1-bigger-hud) | Scales the dictation HUD up to 4× its normal size. |
| [Live transcript](#2-live-transcript-in-the-hud) | Shows the words in the HUD as you speak them. The final text is still produced by a full pass when you stop. |
| [Streaming servers](#streaming-realtime-api) | Streams audio to servers that implement the OpenAI Realtime API, so the live text arrives without re-sending audio. |
| [Other speech servers](#3-speech-servers) | Lets the OpenAI-compatible provider serve speech-to-text models: self-hosted (Speaches, whisper.cpp, LocalAI, vLLM…) on this or another machine, or a cloud endpoint. |
| [Wyoming servers](#wyoming-provider) | A provider for Home Assistant's Wyoming speech protocol, e.g. Parakeet running on an Intel NPU. |

None of this needs extra software. With the built-in local models, everything runs inside Vicinae. A speech
server is only needed if you want transcription to run elsewhere.

Push-to-talk was attempted on this branch and removed. The dictation shortcut works as in upstream Vicinae.

---

## Dictation settings

**Settings → Extensions → Dictation**, or `providers.dictation.preferences` in
`~/.config/vicinae/settings.json`. Settings marked *new* come from this branch; the others are upstream and
listed for completeness.

| Setting | `settings.json` key | Values | Default | Purpose |
|---|---|---|---|---|
| Transcription model | `model` | `none` or `provider:model`, e.g. `local:whisper-large-v3-turbo-q5_0` | `none` | Model that produces the final text when you stop recording. |
| Language | `language` | `auto` or a language code such as `en` | `auto` | Language you speak. Setting it explicitly is faster and avoids misdetection on short clips. Also applies to the live transcript. |
| Sound Effects | `sound` | `true` / `false` | `true` | Plays a sound when recording starts and stops. |
| Pause media | `pauseMedia` | `true` / `false` | `true` | Pauses media players while recording (or mutes output if they can't be paused) and resumes them afterwards. |
| Dictation Action | `dictationAction` | `paste`, `copy` | `paste` | What happens with the final text: paste it into the focused window, or copy it to the clipboard. |
| Live transcription *(new)* | `livePreview` | `off`, `same`, or `provider:model` | `off` | Shows the words in the HUD while you speak. `same` uses the transcription model; you can also pick a different model, such as a fast local one for the preview with a cloud model for the final text. See [§2](#2-live-transcript-in-the-hud). |
| HUD size *(new)* | `hudScale` | number from `1` to `4`, decimals allowed | `1` | Size of the dictation HUD relative to normal. Values outside 1–4 are clamped. See [§1](#1-bigger-hud). |

Example:

```json
"dictation": {
  "preferences": {
    "model": "local:whisper-large-v3-turbo-q5_0",
    "language": "en",
    "dictationAction": "copy",
    "livePreview": "same",
    "hudScale": 2
  }
}
```

---

## 1. Bigger HUD

`hudScale` multiplies every part of the dictation HUD: the pill, the buttons, the waveform, the timer, the
fonts and the live text panel. With `1` and live transcription off, the HUD looks exactly like upstream.

* The HUD never grows wider than the screen it is on, so large scales also fit portrait monitors.
* Short notices shown after dictation, such as "Copied to clipboard", keep their normal size.
* Changes take effect immediately, even during a recording; no restart is needed.

## 2. Live transcript in the HUD

When `livePreview` is not `off`, the HUD widens and gets a three-line text panel under the controls:

* **Full-color text** is settled and won't change.
* **Dimmed text** is the current phrase and may still be corrected as you keep talking.
* New words **fade in** over about half a second instead of popping up, and dimmed words brighten gently
  when they settle. Words the model only confirms again keep their state; corrected words fade in anew.
* *Listening…* is shown until the first words arrive, and *Transcribing…* while the final pass runs. The
  panel scrolls to keep the newest text visible.

**The live transcript is a preview only.** When you stop, the whole recording is transcribed again in one
pass by the transcription model, and that result is pasted or copied. The final text may differ slightly from
the preview, and it is never less accurate than without live transcription.

If the chosen preview model is unavailable (provider removed, model not downloaded), dictation works as if
live transcription were off: no panel is shown.

### How the preview is produced

The method depends on the preview model's provider and is chosen automatically.

#### Chunked (any transcription model)

Used for the built-in local models, Groq, Mistral, OpenAI's whisper models, and any OpenAI-compatible server
without streaming enabled.

1. Vicinae watches the audio level, learning the room's background noise, to find where you pause.
2. While you speak, it re-transcribes the current phrase about once a second as it grows (dimmed text).
3. When you pause for about 0.6 s, the phrase is settled (full-color text) and a new one begins. Phrases
   are also split at a pause once they reach 20 s, so requests stay small.
4. The end of the settled text is sent along with each new phrase, so the model keeps names and sentences
   consistent across phrases.
5. If three requests in a row fail, the preview gives up for that recording. The recording itself
   continues.

Cost and speed: on a GPU with the built-in `whisper-large-v3-turbo`, each pass takes 100–250 ms. **With cloud
models, every pass is a billed request**, so audio is sent several times. To preview for free and still
finalize in the cloud, pick a local model as the preview model.

#### Streaming (Realtime API)

Used when the provider supports the OpenAI Realtime WebSocket API:

* **OpenAI-compatible provider** with *Live transcription* set to a Realtime option (see
  [provider fields](#openai-compatible-provider-fields)).
* **OpenAI provider**, for models named `…transcribe…` (e.g. `gpt-4o-mini-transcribe`), when an API key is
  set. *This path is implemented but has not been tested against OpenAI.*

Audio is streamed continuously instead of being re-sent. The server decides where phrases end: Speaches sends
text once per phrase, while OpenAI sends it word by word.

If the stream fails before producing any text (connection refused, protocol mismatch), Vicinae switches that
recording to chunked mode and replays the audio captured so far, so nothing is lost. Errors reported by the
server on a working stream are only logged.

---

## 3. Speech servers

Speech models come from AI providers, set up in **Settings → AI → Providers**. Once a provider offers
transcription models, they appear in the *Transcription model* and *Live transcription* dropdowns.

| Where the speech runs | Provider to add |
|---|---|
| This machine, inside Vicinae | **Local** (upstream): Vicinae downloads and runs whisper models itself |
| Cloud | **OpenAI**, **Groq**, **Mistral** (upstream): enter an API key |
| A server you host, on this machine or another one on your network | **OpenAI-compatible** (extended on this branch) |
| A cloud or hosted service with an OpenAI-style API not listed above | **OpenAI-compatible** with the service's URL and API key |
| A Wyoming speech server (Home Assistant's protocol), such as Parakeet on an Intel NPU | **Wyoming** (new on this branch) |

### OpenAI-compatible provider fields

The OpenAI-compatible provider still serves chat models (LM Studio, vLLM, llama.cpp…). This branch adds
speech-to-text and streaming to it. You can add several, one per server.

| Field | `fields` key | Purpose |
|---|---|---|
| Name | `name` | Label shown for this server in model lists, e.g. "Speech box". |
| Server URL | `url` | Base URL of the API **including the version prefix**, e.g. `http://192.168.1.20:8000/v1`. Transcription requests go to `<url>/audio/transcriptions`, the model list is read from `<url>/models`. Use the machine's LAN address or hostname to reach another computer, and make sure the server listens on `0.0.0.0` rather than `localhost`. |
| API Key | `apiKey` | Sent as `Authorization: Bearer …`. Leave empty for servers without authentication. |
| Transcription models *(new)* | `transcriptionModels` | Comma- or newline-separated model IDs to offer for dictation. Optional when the server lists its models and their names contain one of: whisper, transcribe, parakeet, voxtral, canary, moonshine, sensevoice, wav2vec, asr, stt, speech-to-text; those are detected on their own. Needed when the server has no model list (whisper.cpp) or uses other names. Listed models are always treated as speech models; other models are offered for chat. |
| Live transcription *(new)* | `realtime` | How the live transcript is produced with this server's models. `off`: chunked mode, works with any server. `beta`: stream over the Realtime API's beta protocol (Speaches and most self-hosted servers). `ga`: stream over the current OpenAI protocol. Use `off` if unsure; a failed stream falls back to chunks anyway. |
| Realtime path *(new)* | `realtimePath` | Where the Realtime WebSocket is, if not at `<url>/realtime`. A relative path is appended to the server URL; one starting with `/` replaces the URL's path. `http`/`https` become `ws`/`wss`. |

Changing the URL, key or model list refreshes the models immediately.

Example:

```json
"providers": {
  "ai": {
    "preferences": {
      "providers": {
        "speech-box": {
          "type": "openai-compatible",
          "fields": {
            "name": "Speech box",
            "url": "http://192.168.1.20:8000/v1",
            "transcriptionModels": "Systran/faster-whisper-large-v3",
            "realtime": "beta"
          }
        }
      }
    }
  }
}
```

The model is then referred to as `speech-box:Systran/faster-whisper-large-v3` in the dictation settings.

### Speaches

[Speaches](https://speaches.ai) serves faster-whisper models with both the transcription and the Realtime
API.

```sh
docker run -d --name speaches -p 8000:8000 \
  -e LOOPBACK_HOST_URL=http://localhost:8000 \
  -v speaches-hf-cache:/home/ubuntu/.cache/huggingface \
  ghcr.io/speaches-ai/speaches:latest-cpu          # or :latest-cuda with --gpus all
curl -X POST http://localhost:8000/v1/models/Systran/faster-whisper-small.en   # download a model
```

Provider settings: URL `http://<host>:8000/v1`, *Live transcription* `Realtime API (beta, Speaches)`. The
models are detected by name.

`LOOPBACK_HOST_URL` is required for streaming. Without it, Speaches sends its internal transcription
calls to `None/v1/...`, every streamed phrase fails, and Vicinae falls back to chunked mode.

### whisper.cpp server

```sh
whisper-server -m ggml-large-v3-turbo.bin --host 0.0.0.0 --port 8080 \
  --inference-path /v1/audio/transcriptions
```

Provider settings: URL `http://<host>:8080/v1`, *Transcription models* set to any name (e.g. `whisper`), since
whisper.cpp has no model list and serves the one model it was started with. Leave *Live transcription* off;
whisper.cpp has no Realtime API.

### Wyoming provider

[Wyoming](https://github.com/OHF-Voice/wyoming) is Home Assistant's protocol for speech services: JSON
events over a plain TCP connection. The **Wyoming** provider asks the server which speech-to-text models it
has (`describe`) and offers them for dictation. Each transcription opens a connection, streams the audio
(16-bit PCM) and waits for the transcript. The live transcript uses the chunked mode.

| Field | `fields` key | Purpose |
|---|---|---|
| Name | `name` | Label shown for this server in model lists. |
| Server address | `url` | `tcp://host:port`, default `tcp://localhost:10300`. A bare `host:port` works too. |
| Longest audio per request (seconds) | `maxSegmentSeconds` | Longer recordings are split at the quietest moment near the limit and sent piece by piece; the texts are joined. Some servers cut audio past a fixed length (Parakeet on the NPU stops at its largest encoder bucket), so set this to that length. `0` sends recordings whole. Default `30`. |

If the server can't be reached, the provider retries in the background (15 s, doubling up to 5 min) and
the provider settings show the error. Requests time out after 3 minutes, enough for a server that compiles
an encoder on first use.

#### Parakeet on the Intel NPU

[wyoming-parakeet-on-intel-npu](https://github.com/cibernox/wyoming-parakeet-on-intel-npu) runs NVIDIA's
Parakeet TDT 0.6B v3 (25 European languages) through OpenVINO on the "AI Boost" NPU of Intel Core Ultra
CPUs (Meteor Lake, Arrow Lake, Lunar Lake). It leaves the GPU free and uses little power. Needs the
`intel_vpu` kernel driver (`/dev/accel/accel0`); the image ships the NPU userspace driver.

The easiest way is [`scripts/setup-parakeet-npu.sh`](scripts/setup-parakeet-npu.sh): it checks the NPU
driver, firmware and Docker, starts the container below, waits until it is ready and transcribes a test
phrase (`--status`, `--test`, `--recreate`, `--uninstall [--purge]`; settings via `PARAKEET_*` variables, see
its header). By hand:

```sh
docker run -d --name parakeet-npu --device /dev/accel/accel0 \
  -e LANGUAGE=en -e ENCODER_BUCKETS=5,10,20,30 -e ENCODER_LAZY_BUCKETS= \
  -p 127.0.0.1:10300:10300 -v parakeet-npu-data:/data --memory 12g \
  --restart unless-stopped ghcr.io/cibernox/wyoming-parakeet-on-intel-npu:latest
```

The encoder is compiled for fixed audio lengths ("buckets", in seconds); each request uses the smallest one
that fits, and audio longer than the largest is truncated by the server. Set the provider's
*Longest audio per request* to the largest bucket (30 above). The first start downloads ~3.2 GB and compiles
the buckets (2–3 min); the blobs are cached in the volume, so later starts take ~10 s. Keep all buckets eager:
a lazy bucket compiles during the first dictation that needs it (~25 s once, then ~1.3 s after each restart).

```json
"parakeet": {
  "type": "wyoming",
  "fields": { "name": "Parakeet NPU", "url": "tcp://127.0.0.1:10300", "maxSegmentSeconds": 30 }
}
```

The model is then `parakeet:nemo-parakeet-tdt-0.6b-v3`. Measured on a Core Ultra 7 265K: 3 s of speech in
~160 ms, 16 s in ~0.4 s, 52 s (two pieces) in ~1.1 s. Parakeet ignores the vocabulary and context prompts.

To watch the NPU work, run [`scripts/npu-top`](scripts/npu-top) (no root needed): live utilization with a
history graph, clock and allocated memory, from the `intel_vpu` sysfs counters. nvtop and Mission Center
don't show the NPU.

---

## Troubleshooting and debugging

| Symptom | Check |
|---|---|
| A server's models don't appear in the dictation dropdowns | The URL includes `/v1` (or the server's prefix); list the model under *Transcription models*. |
| Live panel stays on *Listening…* | The preview model is available; with a remote server, look for `Live transcription:` lines in the log. |
| Streaming never works and the preview arrives in chunks | That's the fallback. For Speaches, check `LOOPBACK_HOST_URL`; otherwise try the other Realtime protocol or the *Realtime path*. |
| Wyoming models don't appear | `nc -z <host> 10300`; the provider settings show the connection error. For the NPU image, `docker logs parakeet-npu` must list `NPU` among the OpenVINO devices. |
| Long Wyoming dictations lose their end | *Longest audio per request* is above the server's largest bucket; lower it. |
| Final text differs from the preview | Expected: the final pass re-transcribes the whole recording with full context. |

Debug aids:

* `VICINAE_HUD_CAPTURE_DIR=/some/dir` saves a PNG of the HUD every second while it is visible;
  `VICINAE_HUD_CAPTURE_MS=100` captures more often, e.g. to check animations.
* `VICINAE_DICTATION_DUMP_DIR=/some/dir` (upstream) saves each recording as WAV.
* The log (`~/.local/state/vicinae/vicinae.log`) has `Live transcription:` and `Dictation:` lines.
