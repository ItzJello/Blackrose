#!/usr/bin/env bash
# Deploy: mark executable (chmod +x) or invoke with bash, e.g.
#   nohup bash /home/deploy/Blackrose/death_webhook.sh >>... 2>&1 &
set -euo pipefail

WEBHOOK_URL="${WEBHOOK_URL:-}"
# Default: Docker sidecar mount (/logs); else typical bare-metal deploy path.
WORLD_LOG="${WORLD_LOG:-}"
if [[ -z "$WORLD_LOG" ]]; then
  if [[ -d /logs ]]; then
    WORLD_LOG="/logs/Server.log"
  else
    WORLD_LOG="/home/deploy/Blackrose/env/dist/logs/Server.log"
  fi
fi

WEBHOOK_USERNAME="${WEBHOOK_USERNAME:-Blackrose}"
EMBED_COLOR="${EMBED_COLOR:-13740282}" # default: soft rose/red-ish (decimal)

sanitize_line() {
  # Trim Windows CRLF + surrounding whitespace/newlines.
  local s="$1"
  s="${s//$'\r'/}"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "$s"
}

WEBHOOK_URL="$(sanitize_line "$WEBHOOK_URL")"
# UTF-8 BOM before the value (common if the .env was saved from some editors).
WEBHOOK_URL="${WEBHOOK_URL#$'\xEF\xBB\xBF'}"
WORLD_LOG="$(sanitize_line "$WORLD_LOG")"
WEBHOOK_USERNAME="$(sanitize_line "$WEBHOOK_USERNAME")"
EMBED_COLOR="$(sanitize_line "$EMBED_COLOR")"
[[ -z "$EMBED_COLOR" ]] && EMBED_COLOR="13740282"
if [[ ! "$EMBED_COLOR" =~ ^[0-9]+$ ]]; then
  echo "ERROR: EMBED_COLOR must be a decimal integer (Discord embed color)." >&2
  exit 1
fi

if [[ -z "$WEBHOOK_URL" ]]; then
  echo "death_webhook: WEBHOOK_URL not set; idling (no Discord, no tail)." >&2
  exec tail -f /dev/null
fi

if [[ ! "$WEBHOOK_URL" =~ ^https://(canary\.)?discord\.com/api/webhooks/[0-9]+/[^[:space:]]+$ ]]; then
  echo "ERROR: WEBHOOK_URL looks malformed after sanitization." >&2
  echo "       It must look like: https://discord.com/api/webhooks/<id>/<token>" >&2
  echo "       Got (redacted): ${WEBHOOK_URL:0:40}... (len=${#WEBHOOK_URL})" >&2
  exit 1
fi

wait_log="${BRDEATH_WEBHOOK_LOG_WAIT_SEC:-180}"
while [[ ! -f "$WORLD_LOG" ]] || [[ ! -r "$WORLD_LOG" ]]; do
  if (( wait_log <= 0 )); then
    echo "ERROR: log file not ready after wait: $WORLD_LOG" >&2
    echo "Hint: check DOCKER_VOL_LOGS / worldserver is writing Server.log" >&2
    exit 1
  fi
  echo "death_webhook: waiting for log ($WORLD_LOG) ... (${wait_log}s left)" >&2
  sleep 2
  wait_log=$((wait_log - 2))
done

if ! command -v jq >/dev/null 2>&1; then
  echo "ERROR: jq is required (install jq)"
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "ERROR: curl is required"
  exit 1
fi

# Fail fast if libcurl rejects the URL (BOM, bad % encoding, stray bytes, etc.).
err="$(mktemp)"
set +e
probe_http="$(
  curl -g -sS -o /dev/null -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"content":"."}' -- "$WEBHOOK_URL" 2>"$err"
)"
probe_ec=$?
set -e
if (( probe_ec != 0 )); then
  echo "ERROR: curl cannot use WEBHOOK_URL (exit ${probe_ec})." >&2
  cat "$err" >&2 || true
  rm -f "$err"
  echo "Hints: UTF-8 BOM before https://, Windows CRLF inside the value," >&2
  echo "       smart quotes, or a stray % in the token (bad percent-encoding)." >&2
  echo "Inspect bytes: printf '%%s' \"\$WEBHOOK_URL\" | xxd | head" >&2
  exit 1
fi
rm -f "$err"

echo "death_webhook: ready (only NEW lines after this moment are sent)." >&2
echo "death_webhook: tailing log: $WORLD_LOG" >&2
if [[ -n "${BRDEATH_WEBHOOK_DEBUG:-}" ]]; then
  echo "death_webhook: BRDEATH_WEBHOOK_DEBUG is set (parse/skip traces on)." >&2
fi

post_discord() {
  local payload="$1"
  local attempt=1
  local max_attempts=4
  local sleep_s=1

  while (( attempt <= max_attempts )); do
    local http
    http="$(
      curl -g -sS \
        -H "Content-Type: application/json" \
        -X POST \
        --data-binary "$payload" \
        -w "%{http_code}" \
        -o /tmp/blackrose_discord_resp.json \
        -- "$WEBHOOK_URL" || true
    )"

    if [[ "$http" == "2"* ]]; then
      return 0
    fi

    # Discord rate limits / transient errors
    if [[ "$http" == "429" || "$http" == "5"* ]]; then
      echo "WARN: Discord POST failed (http=$http), retrying in ${sleep_s}s (attempt ${attempt}/${max_attempts})" >&2
      sleep "$sleep_s"
      sleep_s=$((sleep_s * 2))
      attempt=$((attempt + 1))
      continue
    fi

    echo "ERROR: Discord POST failed (http=$http)" >&2
    if [[ -f /tmp/blackrose_discord_resp.json ]]; then
      head -c 2000 /tmp/blackrose_discord_resp.json >&2 || true
      echo >&2
    fi
    return 1
  done

  echo "ERROR: exhausted retries posting to Discord" >&2
  return 1
}

tail -Fn0 "$WORLD_LOG" | while IFS= read -r line || [[ -n "$line" ]]; do
  [[ "$line" == *"BRDEATH|"* ]] || continue

  data="${line#*BRDEATH|}"
  # map field: DBC name + id, e.g. "Eastern Kingdoms (0)" (legacy lines may be numeric id only)
  IFS='|' read -r player level guild killer mapinfo x y z lastWords <<<"$data"

  # Basic validation (server sends 9 pipe fields; guild may be empty if format changes)
  if [[ -z "${player:-}" || -z "${level:-}" || -z "${killer:-}" || -z "${mapinfo:-}" ]]; then
    if [[ -n "${BRDEATH_WEBHOOK_DEBUG:-}" ]]; then
      echo "death_webhook: skip BRDEATH (missing player/level/killer/map): ${data:0:120}" >&2
    fi
    continue
  fi
  [[ -n "${guild:-}" ]] || guild="(none)"

  # Discord field values max out at 1024 chars; keep it safe.
  lastWords="${lastWords:-...}"
  if ((${#lastWords} > 900)); then
    lastWords="${lastWords:0:900}…"
  fi

  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

  if ! payload="$(
    jq -n \
      --arg username "$WEBHOOK_USERNAME" \
      --arg player "$player" \
      --arg level "$level" \
      --arg guild "$guild" \
      --arg killer "$killer" \
      --arg mapinfo "$mapinfo" \
      --arg x "$x" \
      --arg y "$y" \
      --arg z "$z" \
      --arg words "$lastWords" \
      --arg ts "$ts" \
      --argjson color "$EMBED_COLOR" \
      '{
        username: $username,
        embeds: [{
          title: ("💀 " + $player + " has fallen"),
          description: (
            "**Level** " + $level + " • **Guild** " + $guild + "\n"
            + "**Slain by** " + $killer + "\n"
            + "**Map** `" + $mapinfo + "` • **Position** `" + $x + ", " + $y + ", " + $z + "`"
          ),
          color: $color,
          fields: [
            {name: "Last words", value: $words, inline: false}
          ],
          footer: {text: "• Blackrose Death Feed •"},
          timestamp: $ts
        }]
      }'
  )"; then
    echo "death_webhook: jq failed (payload); check field values / EMBED_COLOR." >&2
    continue
  fi

  post_discord "$payload" || true
done
