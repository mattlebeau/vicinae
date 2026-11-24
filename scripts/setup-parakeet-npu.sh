#!/usr/bin/env bash
# setup-parakeet-npu.sh — run NVIDIA Parakeet TDT 0.6B v3 on the Intel NPU (ODIN)
#
# Uses the Docker image ghcr.io/cibernox/wyoming-parakeet-on-intel-npu, which
# runs Parakeet through OpenVINO on the Core Ultra "AI Boost" NPU and serves it
# over the Wyoming protocol (TCP). Vicinae's dictation talks to it through its
# "Wyoming" AI provider.
#
# Usage:
#   ./setup-parakeet-npu.sh              install / update and start (idempotent)
#   ./setup-parakeet-npu.sh --recreate   remove and recreate the container
#   ./setup-parakeet-npu.sh --status     show container state and NPU stats
#   ./setup-parakeet-npu.sh --test       run the smoke test only
#   ./setup-parakeet-npu.sh --uninstall  remove container (add --purge to delete the model volume)
#
# Environment overrides (defaults in brackets):
#   PARAKEET_IMAGE     [ghcr.io/cibernox/wyoming-parakeet-on-intel-npu:latest]
#   PARAKEET_NAME      [parakeet-npu]       container name
#   PARAKEET_BIND      [127.0.0.1]          host address to publish on (0.0.0.0 for LAN)
#   PARAKEET_PORT      [10300]              host port
#   PARAKEET_LANGUAGE  [en]                 default language
#   PARAKEET_DEVICE    [NPU]                NPU | GPU | CPU
#   PARAKEET_BUCKETS   [5,10,20,30]         eager encoder buckets (seconds), compiled (or loaded
#                                           from the cache) at start, so no dictation waits on it
#   PARAKEET_LAZY      []                   lazy buckets, compiled on first use (first use of a
#                                           new one takes ~25 s). Audio longer than the largest
#                                           bucket is truncated by the server; Vicinae splits
#                                           recordings to fit (provider "Longest audio" = 30).
#   PARAKEET_VOLUME    [parakeet-npu-data]  model + compiled-blob cache volume
#   PARAKEET_MEM       [12g]                container memory limit (NPU compiles peak at 6-10 GB)
set -euo pipefail

IMAGE="${PARAKEET_IMAGE:-ghcr.io/cibernox/wyoming-parakeet-on-intel-npu:latest}"
NAME="${PARAKEET_NAME:-parakeet-npu}"
BIND="${PARAKEET_BIND:-127.0.0.1}"
PORT="${PARAKEET_PORT:-10300}"
LANGUAGE="${PARAKEET_LANGUAGE:-en}"
DEVICE="${PARAKEET_DEVICE:-NPU}"
BUCKETS="${PARAKEET_BUCKETS:-5,10,20,30}"
LAZY="${PARAKEET_LAZY-}"
VOLUME="${PARAKEET_VOLUME:-parakeet-npu-data}"
MEM="${PARAKEET_MEM:-12g}"
NPU_DEV=/dev/accel/accel0
READY_TIMEOUT=900   # first start downloads ~3.2 GB and compiles buckets

c_ok=$'\e[32m'; c_warn=$'\e[33m'; c_err=$'\e[31m'; c_off=$'\e[0m'
say()  { printf '%s==>%s %s\n' "$c_ok" "$c_off" "$*"; }
warn() { printf '%swarning:%s %s\n' "$c_warn" "$c_off" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$c_err" "$c_off" "$*" >&2; exit 1; }

container_exists()  { docker container inspect "$NAME" >/dev/null 2>&1; }
container_running() { [[ "$(docker container inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null)" == true ]]; }

# --------------------------------------------------------------------------
# Wyoming smoke test: synthesize speech, send it, print the transcript.
# --------------------------------------------------------------------------
wyoming_py() {
    python3 -I - "$@" <<'PY'
import json, socket, sys, wave

host, port, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3]

def send(sock, etype, data=None, payload=b""):
    header = {"type": etype}
    body = json.dumps(data).encode() if data else b""
    if body:
        header["data_length"] = len(body)
    if payload:
        header["payload_length"] = len(payload)
    sock.sendall(json.dumps(header).encode() + b"\n" + body + payload)

def recv(f):
    line = f.readline()
    if not line:
        return None
    header = json.loads(line)
    data = header.get("data") or {}
    if header.get("data_length"):
        data.update(json.loads(f.read(header["data_length"])))
    if header.get("payload_length"):
        f.read(header["payload_length"])
    return header["type"], data

sock = socket.create_connection((host, port), timeout=120)
f = sock.makefile("rb")
if mode == "describe":
    send(sock, "describe")
    while (ev := recv(f)) and ev[0] != "info":
        pass
    for prog in (ev[1].get("asr") or []):
        for m in prog.get("models") or []:
            print(f"{prog['name']}: {m['name']} ({len(m.get('languages') or [])} languages)")
    sys.exit(0)

w = wave.open(sys.argv[4], "rb")
rate, width, ch = w.getframerate(), w.getsampwidth(), w.getnchannels()
pcm = w.readframes(w.getnframes())
fmt = {"rate": rate, "width": width, "channels": ch}
send(sock, "transcribe", {"language": sys.argv[5]})
send(sock, "audio-start", fmt)
step = rate * width * ch // 10
for i in range(0, len(pcm), step):
    send(sock, "audio-chunk", fmt, pcm[i:i + step])
send(sock, "audio-stop")
while (ev := recv(f)) and ev[0] != "transcript":
    pass
print(ev[1].get("text", "") if ev else "")
PY
}

smoke_test() {
    say "Smoke test against $BIND:$PORT"
    local host="$BIND"; [[ "$host" == 0.0.0.0 ]] && host=127.0.0.1
    wyoming_py "$host" "$PORT" describe || die "Wyoming describe failed"
    if ! command -v espeak-ng >/dev/null; then
        warn "espeak-ng not installed; skipping transcription test"
        return 0
    fi
    SMOKE_TMP=$(mktemp -d)
    trap 'rm -rf "$SMOKE_TMP"' EXIT
    local tmp=$SMOKE_TMP
    local phrase="The quick brown fox jumps over the lazy dog."
    espeak-ng -s 150 -w "$tmp/test.wav" "$phrase"
    local t0 t1 text
    t0=$(date +%s%N)
    text=$(wyoming_py "$host" "$PORT" transcribe "$tmp/test.wav" "$LANGUAGE")
    t1=$(date +%s%N)
    printf '  said:  %s\n  heard: %s\n  time:  %d ms\n' "$phrase" "$text" $(( (t1 - t0) / 1000000 ))
    [[ "${text,,}" == *"lazy dog"* ]] || die "transcript does not match"
    say "Transcription works"
}

status() {
    if container_exists; then
        docker ps -a --filter "name=^${NAME}$" --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
    else
        echo "container $NAME does not exist"
    fi
    local d=/sys/class/accel/accel0/device
    if [[ -r $d/npu_busy_time_us ]]; then
        printf 'NPU busy time: %s us, frequency %s/%s MHz, memory used %s bytes\n' \
            "$(<"$d/npu_busy_time_us")" "$(<"$d/npu_current_frequency_mhz")" \
            "$(<"$d/npu_max_frequency_mhz")" "$(<"$d/npu_memory_utilization")"
    fi
}

preflight() {
    say "Preflight checks"
    command -v docker >/dev/null || die "docker is not installed"
    docker info >/dev/null 2>&1 || die "cannot talk to the docker daemon (is it running, and are you in the docker group?)"
    if [[ "$DEVICE" == NPU ]]; then
        [[ -e $NPU_DEV ]] || die "$NPU_DEV missing. Load the driver: sudo modprobe intel_vpu"
        lsmod | grep -q '^intel_vpu' || warn "intel_vpu module not listed by lsmod"
        compgen -G '/lib/firmware/intel/vpu/vpu_*' >/dev/null || warn "no NPU firmware under /lib/firmware/intel/vpu (package linux-firmware-intel)"
        [[ -r $NPU_DEV && -w $NPU_DEV ]] || warn "$NPU_DEV is not read/writable by $(id -un); the container may not open it"
        printf '  NPU: %s\n' "$(lspci | grep -i 'npu' | head -1 | cut -d' ' -f2- || true)"
    fi
    command -v python3 >/dev/null || die "python3 is required for the smoke test"
}

wait_ready() {
    say "Waiting for the server (first start downloads ~3.2 GB and compiles NPU blobs; can take several minutes)"
    local start=$SECONDS last=""
    while (( SECONDS - start < READY_TIMEOUT )); do
        container_running || { docker logs --tail 40 "$NAME" >&2; die "container stopped"; }
        if docker logs "$NAME" 2>&1 | grep -q 'Ready. Listening'; then
            say "Server ready after $(( SECONDS - start )) s"
            return 0
        fi
        local line; line=$(docker logs --tail 1 "$NAME" 2>&1 | cut -c1-150)
        [[ -n "$line" && "$line" != "$last" ]] && { printf '  %s\n' "$line"; last=$line; }
        sleep 3
    done
    die "server not ready after $READY_TIMEOUT s; see: docker logs $NAME"
}

install() {
    preflight
    say "Pulling $IMAGE"
    docker pull "$IMAGE"
    if container_exists; then
        local current; current=$(docker container inspect -f '{{.Image}}' "$NAME")
        local latest;  latest=$(docker image inspect -f '{{.Id}}' "$IMAGE")
        if [[ $RECREATE == 1 || "$current" != "$latest" ]]; then
            say "Replacing existing container $NAME"
            docker rm -f "$NAME" >/dev/null
        else
            say "Container $NAME is up to date"
            container_running || docker start "$NAME" >/dev/null
        fi
    fi
    if ! container_exists; then
        say "Creating container $NAME (device $DEVICE, buckets eager=$BUCKETS lazy=$LAZY, $BIND:$PORT)"
        local devargs=()
        [[ "$DEVICE" == NPU ]] && devargs+=(--device "$NPU_DEV:$NPU_DEV")
        [[ "$DEVICE" == GPU ]] && devargs+=(--device /dev/dri:/dev/dri)
        docker run -d \
            --name "$NAME" \
            "${devargs[@]}" \
            -e LANGUAGE="$LANGUAGE" \
            -e DEVICE="$DEVICE" \
            -e ENCODER_BUCKETS="$BUCKETS" \
            -e ENCODER_LAZY_BUCKETS="$LAZY" \
            -e TZ="$(timedatectl show -p Timezone --value 2>/dev/null || echo Etc/UTC)" \
            -p "$BIND:$PORT:10300" \
            -v "$VOLUME:/data" \
            --memory "$MEM" \
            --restart unless-stopped \
            "$IMAGE" >/dev/null
    fi
    wait_ready
    smoke_test
    cat <<EOF

Parakeet is running on the $DEVICE at tcp://$BIND:$PORT (restarts with Docker).
Vicinae: Settings → AI → add a "Wyoming" provider with host $BIND, port $PORT,
then pick "<provider>:nemo-parakeet-tdt-0.6b-v3" as the Dictation model.
Logs: docker logs -f $NAME
EOF
}

RECREATE=0
case "${1:-}" in
    "")          install ;;
    --recreate)  RECREATE=1; install ;;
    --status)    status ;;
    --test)      smoke_test ;;
    --uninstall)
        docker rm -f "$NAME" 2>/dev/null && say "Removed container $NAME" || true
        if [[ "${2:-}" == --purge ]]; then
            docker volume rm "$VOLUME" && say "Removed volume $VOLUME"
        fi ;;
    -h|--help)   sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//' ;;
    *)           die "unknown option $1 (see --help)" ;;
esac
