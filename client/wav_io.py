"""
wav_io — load any PCM WAV and normalize to the ASTK audio format
(16 kHz / mono / S16_LE). Pure stdlib (no numpy), shared by the tools.
"""
import array
import wave

TARGET_RATE = 16000


class WavError(Exception):
    pass


def read_wav_mono16(path):
    """Return s16le mono 16 kHz bytes. Raises WavError with a conversion hint."""
    try:
        w = wave.open(path, "rb")
    except (wave.Error, EOFError) as e:
        raise WavError(
            f"{path}: cannot read as PCM WAV ({e}) — convert first: "
            f"ffmpeg -i <input> -ar 16000 -ac 1 -c:a pcm_s16le out.wav")
    with w:
        nch, sw, rate, n = (w.getnchannels(), w.getsampwidth(),
                            w.getframerate(), w.getnframes())
        raw = w.readframes(n)

    if sw == 2:
        a = array.array("h")
        a.frombytes(raw)
        f = [v * (1.0 / 32768.0) for v in a]
    elif sw == 1:
        f = [(b - 128) * (1.0 / 128.0) for b in raw]
    elif sw == 4:
        a = array.array("i")
        a.frombytes(raw)
        f = [v * (1.0 / 2147483648.0) for v in a]
    else:
        raise WavError(f"{path}: unsupported sample width {sw}")

    if nch > 1:
        f = [sum(f[i * nch:(i + 1) * nch]) / nch for i in range(len(f) // nch)]

    if rate != TARGET_RATE:  # linear-interpolation resample (test-grade)
        step = rate / TARGET_RATE
        out = []
        pos = 0.0
        while int(pos) + 1 < len(f):
            i = int(pos)
            fr = pos - i
            out.append(f[i] * (1.0 - fr) + f[i + 1] * fr)
            pos += step
        f = out

    out16 = array.array("h", (max(-32767, min(32767, int(round(v * 32767)))) for v in f))
    return out16.tobytes()
