"""
astk_client — client for the Whisper engine side (Python 3.8+, stdlib only).

Protocol doc: docs/PROTOCOL.md

Usage example:
    from astk_client import AudioStackClient, MsgType

    c = AudioStackClient()          # default: /run/audiostack/engine.sock
    c.connect()                     # waits/retries until the daemon is up
    for f in c.frames():
        if f.type == MsgType.HELLO:
            meta = f.json()         # {"format":{"rate":16000,...}, ...}
        elif f.type == MsgType.AUDIO:
            pcm16 = f.payload       # s16le mono 16kHz bytes
        elif f.type == MsgType.SESSION_END:
            ...
    # send a transcript back:
    c.send_result("Hello.", kind="final", t_start_ms=1200, t_end_ms=4300)

If you use the shared-memory ring (low-latency path):
    from astk_client import ShmRingReader
    r = ShmRingReader("/audiostack_ring")
    r.seek_to_now()
    while True:
        data = r.read(3200)   # 100 ms worth (16k * 2B * 0.1)
"""
import mmap
import os
import socket
import struct
import time

HEADER = struct.Struct("<IBBHIQI")  # magic I, ver B, type B, flags H, seq I, t_us Q, len I = 24B
MAGIC = 0x4B535441
VERSION = 1
MAX_PAYLOAD = 1 << 20


class MsgType:
    HELLO = 0x01
    GOODBYE = 0x02
    PING = 0x03
    PONG = 0x04
    AUDIO = 0x10
    SESSION_BEGIN = 0x11
    SESSION_END = 0x12
    HEARTBEAT = 0x13
    CTRL = 0x14
    RESULT = 0x20
    ENGINE_STATUS = 0x21
    ERROR = 0x2F


class Flags:
    SILENCE = 1 << 0
    CACHED = 1 << 1
    FINAL = 1 << 2


class Frame:
    __slots__ = ("type", "flags", "seq", "t_us", "payload")

    def __init__(self, type_, flags, seq, t_us, payload):
        self.type = type_
        self.flags = flags
        self.seq = seq
        self.t_us = t_us
        self.payload = payload

    def json(self):
        import json
        return json.loads(self.payload.decode("utf-8")) if self.payload else {}

    def samples(self):
        """AUDIO payload as bytes (convert with numpy.frombuffer(..., np.int16))."""
        return self.payload


class AudioStackClient:
    def __init__(self, socket_path=None):
        self.path = socket_path or os.environ.get("ASTK_ENGINE_SOCKET",
                                                  "/run/audiostack/engine.sock")
        self._sock = None
        self._buf = b""
        self._seq = 0

    # ------------------------------------------------------------ connect ----
    def connect(self, wait=True, timeout=0.0):
        """wait=True: retry until the daemon appears. TimeoutError after `timeout` s."""
        deadline = time.monotonic() + timeout if timeout else None
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.path)
                self._sock = s
                self._buf = b""
                return
            except (FileNotFoundError, ConnectionRefusedError):
                if not wait or (deadline and time.monotonic() >= deadline):
                    raise
                time.sleep(0.5)

    def close(self):
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    # -------------------------------------------------------- frame I/O ----
    def _recv_exact(self, n, timeout=None):
        old = self._sock.gettimeout()
        self._sock.settimeout(timeout)
        try:
            data = b""
            while len(data) < n:
                chunk = self._sock.recv(n - len(data))
                if not chunk:
                    raise ConnectionError("engine.sock closed by daemon")
                data += chunk
            return data
        finally:
            self._sock.settimeout(old)

    def recv_frame(self, timeout=None):
        hdr = self._recv_exact(HEADER.size, timeout)
        magic, ver, type_, flags, seq, t_us, plen = HEADER.unpack(hdr)
        if magic != MAGIC or ver != VERSION:
            raise ValueError("protocol mismatch (magic=0x%x ver=%d)" % (magic, ver))
        if plen > MAX_PAYLOAD:
            raise ValueError("payload too large: %d" % plen)
        payload = self._recv_exact(plen, timeout) if plen else b""
        f = Frame(type_, flags, seq, t_us, payload)
        if type_ == MsgType.PING:      # automatic heartbeat reply
            self.send_frame(MsgType.PONG)
        return f

    def frames(self, timeout=None):
        """Infinite iterator yielding AUDIO/HELLO/... frames."""
        while True:
            try:
                yield self.recv_frame(timeout)
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                return

    def send_frame(self, type_, payload=b"", flags=0):
        hdr = HEADER.pack(MAGIC, VERSION, type_, flags, self._seq,
                          int(time.monotonic() * 1e6), len(payload))
        self._sock.sendall(hdr + payload)
        self._seq += 1

    # ----------------------------------------------------- engine -> daemon --
    def send_result(self, text, kind="final", t_start_ms=None, t_end_ms=None,
                    lang="ko", conf=None, session_id=None, extra=None):
        """Send a transcript. kind: 'partial' (provisional) | 'final' (confirmed).

        The daemon passes this JSON through as the `transcript` field of the
        backend POST payload.
        """
        import json
        obj = {"type": kind, "text": text}
        if lang is not None:
            obj["lang"] = lang
        if t_start_ms is not None:
            obj["t_start_ms"] = int(t_start_ms)
        if t_end_ms is not None:
            obj["t_end_ms"] = int(t_end_ms)
        if conf is not None:
            obj["conf"] = float(conf)
        if session_id:
            obj["session_id"] = session_id
        if extra:
            obj.update(extra)
        self.send_frame(MsgType.RESULT, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def send_status(self, **kw):
        import json
        self.send_frame(MsgType.ENGINE_STATUS, json.dumps(kw, ensure_ascii=False).encode("utf-8"))


# ------------------------------------------------- shm ring reader (optional) --
SHM_HEADER = struct.Struct("<IIIIIIQQQ")  # magic,ver,rate,ch,enc,capacity,write_pos,epoch,seq
# C layout: u32*6 = 24 bytes, then write_pos(u64) @24 — no padding (8-alignment holds)
SHM_HDR_SIZE = 256


class ShmRingReader:
    """Reads the daemon's 30-second ring (16k mono s16le) with near-zero copies."""

    def __init__(self, name="/audiostack_ring"):
        self.name = name
        path = "/dev/shm" + (name if name.startswith("/") else "/" + name)
        self._f = open(path, "rb")
        self._mm = mmap.mmap(self._f.fileno(), 0, prot=mmap.PROT_READ)
        (magic, ver, self.rate, self.channels, self.encoding, self.capacity,
         _wpos, self.epoch_us, _seq) = SHM_HEADER.unpack(self._mm[:SHM_HEADER.size])
        if magic != 0x52545341:
            raise ValueError("shm magic mismatch — the daemon must run with transport=shm|both")
        self._rpos = None

    def _wpos(self):
        return struct.unpack("<Q", self._mm[24:32])[0]  # write_pos @ offset 24

    def seek_to_now(self):
        self._rpos = self._wpos()

    def available(self):
        w = self._wpos()
        r = w if self._rpos is None else self._rpos
        return max(0, w - r)

    def read(self, n):
        w = self._wpos()
        if self._rpos is None:
            self._rpos = w  # default: start from now
        if w - self._rpos > self.capacity:  # overwritten (miss)
            self._rpos = w - self.capacity
        avail = w - self._rpos
        n = min(n, avail)
        if n <= 0:
            return b""
        off = self._rpos % self.capacity
        data = self._mm[SHM_HDR_SIZE + off: SHM_HDR_SIZE + off + min(n, self.capacity - off)]
        if n > len(data):
            data += self._mm[SHM_HDR_SIZE: SHM_HDR_SIZE + (n - len(data))]
        self._rpos += n
        return bytes(data)

    def close(self):
        self._mm.close()
        self._f.close()
