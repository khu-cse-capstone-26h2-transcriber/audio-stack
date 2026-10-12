# ASTK IPC protocol v1 — audio stack ↔ engine ↔ backend

This is the concrete specification of survey options **(2) UNIX Domain Socket**
and **(1) POSIX shared-memory ring**. The engine and the web backend only need
this document to integrate.

## 0. Data flow

```
mic (ALSA) → audiostackd → [AUDIO frames] → Whisper engine
                ↑                  [RESULT frame (JSON)] ↓
                └── HTTP POST (JSON) ──→ Node/Express ──SSE──→ React
```

- Fixed audio format: **16,000 Hz / mono / S16_LE** (the Whisper standard input)
- Pre-processing (50 Hz high-pass, resampling, channel extraction) happens in
  the daemon — the engine receives inference-ready PCM.

## 1. Engine IPC (AF_UNIX SOCK_STREAM)

Default path: `/run/audiostack/engine.sock` (config `[engine] socket`).
The daemon is the server, the engine is the client. The engine may reconnect
any number of times (one concurrent client; a new connection replaces the old
one). On reconnect the daemon replays the last `cache_seconds` (default 10 s)
of audio with the `kFlagCached` flag, then continues live.

### Frame header (24 bytes, little-endian)

| off | size | field        | notes |
|----|------|--------------|-------|
| 0  | 4    | magic        | `'A','S','T','K'` = 0x4B535441 |
| 4  | 1    | version      | 1 |
| 5  | 1    | type         | see table below |
| 6  | 2    | flags        | bit0 silence, bit1 cached, bit2 final |
| 8  | 4    | seq          | sender counter |
| 12 | 8    | t_us         | CLOCK_MONOTONIC µs (capture instant for AUDIO) |
| 20 | 4    | payload_len  | ≤ 1 MiB |

### Message types

| type | dir | payload |
|------|-----|---------|
| 0x01 HELLO | D→E | JSON: format/session/device metadata (once, right after connect) |
| 0x10 AUDIO | D→E | PCM s16le mono 16k, default 20 ms (320 samples = 640 B) |
| 0x11 SESSION_BEGIN | D→E | `{"session_id":"sess-..."}` |
| 0x12 SESSION_END | D→E | `{"session_id":"sess-..."}` — recording session switch/end |
| 0x13 HEARTBEAT | D→E | JSON: `{"level_db":-32.1,"session_id":...,"backlog":n}` (every 2 s) |
| 0x14 CTRL | D→E | JSON control events (`{"event":"xrun"}` etc.) |
| 0x20 RESULT | E→D | JSON transcript (schema below) |
| 0x21 ENGINE_STATUS | E→D | JSON: `{"model":"whisper-small-ko","loaded":true,...}` |
| 0x03/0x04 PING/PONG | both | daemon PING → client library auto-PONG |
| 0x02 GOODBYE / 0x2F ERROR | both | JSON reason |

### AUDIO flags

- `silence(0x01)`: below the daemon energy gate (`silence_gate_db`) — useful
  for the "skip inference while silent" strategy.
- `cached(0x02)`: chunk replayed from the daemon cache after (re)connect.
- `final(0x04)`: last chunk of the session.

### RESULT payload schema (engine → daemon)

```json
{
  "type": "final",            // "partial" (provisional) | "final" (confirmed)
  "text": "transcribed text",
  "lang": "ko",
  "t_start_ms": 1200,          // segment-relative times (optional)
  "t_end_ms": 4300,            // (optional)
  "conf": 0.93                 // (optional)
}
```

Only `text` is required. The daemon does **not** parse this JSON — it forwards
it verbatim to the backend. Plain (non-JSON) text is wrapped as
`{"type":"final","text":"..."}`.

## 2. Shared-memory ring (optional, `[engine] transport = shm|both`)

`shm_open("/audiostack_ring")` — 256-byte header + power-of-two ring data.
Default capacity 30 s (16k·mono·s16 = 960,000 B → 1 MiB). Header layout:

| off | type | field |
|-----|------|-------|
| 0 | u32 | magic 0x52545341 ("ASTR") |
| 4 | u32 | version = 1 |
| 8 | u32 | rate (16000) |
| 12 | u32 | channels (1) |
| 16 | u32 | encoding (0 = S16LE) |
| 20 | u32 | capacity (ring bytes, power of two) |
| 24 | u64 | write_pos (total bytes written, monotonic) |
| 32 | u64 | epoch_us (CLOCK_MONOTONIC at write_pos=0) |
| 40 | u64 | seq |

- The producer (daemon) overwrites old data. Each reader tracks its own
  `read_pos` in-process.
- If `read_pos < write_pos - capacity` data was missed → resync to
  `write_pos - capacity`.
- Position → time: `t_us = epoch_us + (write_pos/32000)*1e6` (32,000 B/s)
- Python sample: `ShmRingReader` in `client/astk_client.py`

## 3. Daemon → backend HTTP

The daemon POSTs JSON (Content-Type: `application/json`, Connection: close) to
`[backend] base_url` (default `http://127.0.0.1:3000`):

| path | body |
|------|------|
| `/api/sessions` | `{"session_id":"sess-20261012-153001-4f2a","started_at":"2026-10-12T15:30:01Z","device":"plughw:1,0"}` |
| `/api/sessions/{session}/transcripts` | `{"session_id":...,"seq":N,"received_at":ISO,"audio_t_us":µs,"transcript":{RESULT payload}}` |
| `/api/sessions/{session}/end` | `{"session_id":...,"ended_at":ISO}` |

- 2xx expected. If the backend is down, the daemon keeps up to `queue_max`
  (512) items and retries with `retry_seconds` backoff; beyond that it drops
  and counts (visible via `astk-ctl status`).
- Express route examples: `docs/INTEGRATION-WEB.md`

## 4. Control socket (humans/scripts/Node)

`/run/audiostack/ctrl.sock` — one text command per connection, reply then EOF:

```
status | stats | level | session rotate | shutdown | help
```

```bash
build/astk-ctl status
echo 'session rotate' | socat - UNIX-CONNECT:/run/audiostack/ctrl.sock
```

To trigger recording start/stop from the web app or a physical button, send
`session rotate` to this socket (the daemon issues the session id; engine
RESULTs carry it implicitly through the transcript envelope).
