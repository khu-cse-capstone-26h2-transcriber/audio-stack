// ============================================================================
// dummy_backend.js — minimal stand-in for the web/app part (Node/Express +
// PostgreSQL + SSE). Implements, with node:http only (no deps), exactly the
// endpoints the audio stack (audiostackd) POSTs to. Replace with the real
// backend by keeping the same routes (or just point base_url at it).
//
// run: node tests/dummy_backend.js [port]     (default 3000)
// demo: open http://localhost:3000/ to see live transcripts over SSE
// ============================================================================
const http = require("http");

const PORT = Number(process.argv[2] || process.env.PORT || 3000);
const sessions = new Map(); // id -> {id, started_at, ended_at, transcripts:[]}
const sseClients = new Set();

function sendSSE(event, data) {
  const msg = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const res of sseClients) res.write(msg);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let b = "";
    req.on("data", (c) => { b += c; if (b.length > 1e6) req.destroy(); });
    req.on("end", () => { try { resolve(JSON.parse(b || "{}")); } catch (e) { reject(e); } });
    req.on("error", reject);
  });
}

function json(res, code, obj) {
  res.writeHead(code, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(obj));
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://localhost:${PORT}`);
  const p = url.pathname;

  try {
    // ---- ingest API called by the audio stack daemon ----------------------
    if (req.method === "POST" && p === "/api/sessions") {
      const b = await readBody(req);
      sessions.set(b.session_id, { id: b.session_id, started_at: b.started_at, ended_at: null, transcripts: [] });
      console.log(`[session start] ${b.session_id} device=${b.device}`);
      sendSSE("session", { type: "start", session_id: b.session_id, started_at: b.started_at });
      return json(res, 201, { ok: true, session_id: b.session_id });
    }

    const mT = p.match(/^\/api\/sessions\/([^/]+)\/transcripts$/);
    if (req.method === "POST" && mT) {
      const b = await readBody(req);
      const s = sessions.get(mT[1]);
      if (s) s.transcripts.push(b);
      console.log(`[transcript ] ${mT[1]} seq=${b.seq} ${JSON.stringify(b.transcript)}`);
      sendSSE("transcript", b); // -> consumed by the React liveConnection layer
      return json(res, 202, { ok: true });
    }

    const mE = p.match(/^\/api\/sessions\/([^/]+)\/end$/);
    if (req.method === "POST" && mE) {
      const b = await readBody(req);
      const s = sessions.get(mE[1]);
      if (s) s.ended_at = b.ended_at;
      console.log(`[session end] ${mE[1]}`);
      sendSSE("session", { type: "end", session_id: mE[1], ended_at: b.ended_at });
      return json(res, 200, { ok: true });
    }

    // ---- for the web app (React) ------------------------------------------
    if (req.method === "GET" && p === "/api/live") { // SSE
      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "keep-alive",
      });
      res.write(": connected\n\n");
      sseClients.add(res);
      req.on("close", () => sseClients.delete(res));
      return;
    }
    if (req.method === "GET" && p === "/api/sessions") {
      return json(res, 200, [...sessions.values()]);
    }
    if (req.method === "GET" && p === "/") {
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
      res.end(`<!doctype html><meta charset="utf-8"><title>audiostack demo</title>
<h3>Live transcripts (SSE demo)</h3><pre id="log"></pre>
<script>
const es = new EventSource("/api/live");
const log = document.getElementById("log");
es.addEventListener("transcript", e => {
  const d = JSON.parse(e.data);
  log.textContent += (d.transcript && d.transcript.text) || JSON.stringify(d) ;
  log.textContent += "\\n";
});
es.addEventListener("session", e => { log.textContent += "-- " + e.data + "\\n"; });
</script>`);
      return;
    }
    json(res, 404, { error: "not found" });
  } catch (e) {
    json(res, 400, { error: String(e) });
  }
});

server.listen(PORT, () => console.log(`dummy backend :${PORT} (POST /api/sessions[/{id}/transcripts|/end], GET /api/live, GET /)`));
