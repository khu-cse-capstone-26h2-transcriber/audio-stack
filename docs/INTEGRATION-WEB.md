# Web/app integration guide — Node/Express + PostgreSQL + SSE ↔ audio stack

Mapping to the web/app decisions:

| web/app decision | audio stack counterpart |
|-----------|----------------|
| SSE push of partial/final text | daemon POSTs RESULT frames → backend relays via its broadcaster |
| transcript payload format TBD | `transcript.type = "partial" \| "final"` already distinguishes them (PROTOCOL.md §RESULT) |
| backend location TBD (on-board vs remote) | change only `[backend] base_url` / `--backend-url` (same role as STT_SERVICE_URL) |
| record trigger TBD (physical button vs web button) | web button → backend → `session rotate` on ctrl.sock; GPIO button → same call. Session model unchanged either way |
| single user, no login | daemon only issues session ids; no auth (loopback / LAN assumed) |

## 1. Routes the daemon calls (stub: tests/dummy_backend.js)

```js
// add these three routes to the backend
app.post("/api/sessions", (req, res) => {
  // req.body: { session_id, started_at, device }
  db.insertSession(req.body);            // PostgreSQL
  broadcaster.session("start", req.body); // → SSE
  res.status(201).json({ ok: true });
});

app.post("/api/sessions/:id/transcripts", (req, res) => {
  // req.body: { session_id, seq, received_at, audio_t_us,
  //             transcript: { type: "partial"|"final", text, lang, t_start_ms, t_end_ms, conf } }
  if (req.body.transcript.type === "final") db.insertTranscript(req.body); // history
  broadcaster.transcript(req.body);      // → SSE (render partials live too)
  res.status(202).json({ ok: true });
});

app.post("/api/sessions/:id/end", (req, res) => {
  db.endSession(req.params.id, req.body.ended_at);
  broadcaster.session("end", req.body);
  res.json({ ok: true });
});
```

SSE payload example (the existing `liveConnection` layer consumes it unchanged):

```
event: transcript
data: {"session_id":"sess-20261012-153001-4f2a","seq":7,"received_at":"2026-10-12T15:31:22Z","audio_t_us":123456789,"transcript":{"type":"final","text":"Let's start the meeting.","lang":"ko","t_start_ms":200,"t_end_ms":1900}}
```

Reconnect banner / EventSource policy: no changes needed.

## 2. Record start/stop trigger

```js
// backend: web "start recording" button → rotate the daemon session
const net = require("net");
function audiostackCtl(cmd) {
  return new Promise((resolve, reject) => {
    const s = net.connect("/run/audiostack/ctrl.sock"); // on-board placement
    s.on("connect", () => s.write(cmd + "\n"));
    let out = "";
    s.on("data", d => out += d);
    s.on("end", () => resolve(out));
    s.on("error", reject);
  });
}
app.post("/api/record/start", async (req, res) => res.json(JSON.parse(await audiostackCtl("session rotate"))));
```

If the backend runs on a separate server, either expose ctrl.sock over TCP
(e.g. `socat TCP-LISTEN:9101,reuseaddr UNIX-CONNECT:/run/audiostack/ctrl.sock`)
or simply let the daemon record continuously (auto session) and have the web
app only browse history.

## 3. Local integration test (30 seconds)

```bash
node tests/dummy_backend.js 3000 &      # Express stand-in (plain node:http, same API)
./build/audiostackd --device plughw:1,0 &
python3 tests/dummy_engine.py &         # engine stand-in (energy VAD + fake transcripts)
# talk — the browser at http://localhost:3000/ shows "[dummy transcript N]" arriving over SSE
```

Swaps later: `dummy_engine.py` → real Whisper engine (same RESULT format);
`dummy_backend.js` → real Express backend (same routes). Daemon/protocol unchanged.

## 4. PostgreSQL schema proposal (phase ① session CRUD)

```sql
CREATE TABLE sessions (
  id TEXT PRIMARY KEY,             -- daemon-issued "sess-YYYYMMDD-HHMMSS-xxxx"
  started_at TIMESTAMPTZ NOT NULL,
  ended_at   TIMESTAMPTZ,
  device TEXT
);
CREATE TABLE transcripts (
  session_id TEXT REFERENCES sessions(id),
  seq INT,
  type TEXT CHECK (type IN ('partial','final')),
  text TEXT,
  lang TEXT,
  t_start_ms INT, t_end_ms INT,
  audio_t_us BIGINT,               -- raw data for latency measurement
  received_at TIMESTAMPTZ DEFAULT now(),
  PRIMARY KEY (session_id, seq)
);
```
