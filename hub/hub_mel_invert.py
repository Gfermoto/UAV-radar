#!/usr/bin/env python3
"""Griffin-Lim: uint8 Hub Mel → WAV (наш LUT, не librosa defaults)."""
from __future__ import annotations

import io
import json
import math
import os
import threading
import wave
from pathlib import Path
from typing import Any

import numpy as np

from generate_mel_lut import (
    FFT_SIZE,
    MEL_FMAX,
    MEL_FMIN,
    MEL_NUM_BANDS,
    NUM_BINS,
    SAMPLE_RATE,
    WINDOW_LENGTH,
    generate_filterbank,
    generate_hann_window,
)

HOP = 160
LOG_EPS = 1e-10
FRAMES_MAX = 401
_LOCK = threading.Lock()
_HANN: np.ndarray | None = None
_FB: np.ndarray | None = None
_FB_PINV: np.ndarray | None = None


class MelInvertError(ValueError):
    """Инверсия невозможна (dims / bands / minmax)."""


def invert_iters() -> int:
    try:
        n = int(os.environ.get("MEL_INVERT_ITERS", "32") or 32)
    except ValueError:
        n = 32
    return max(1, min(n, 64))


def invert_fast() -> bool:
    v = (os.environ.get("MEL_INVERT_FAST", "1") or "1").strip().lower()
    return v not in ("0", "false", "no", "off")


def invert_rev() -> str:
    """Тег кэша WAV: смена → пересчёт (старый classic не отдаём как FGLA)."""
    return "fgla32" if invert_fast() else "gl32"


def _lut() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    global _HANN, _FB, _FB_PINV
    if _HANN is None or _FB is None or _FB_PINV is None:
        hann = np.asarray(generate_hann_window(WINDOW_LENGTH, FFT_SIZE), dtype=np.float64)
        fb = np.asarray(
            generate_filterbank(
                MEL_NUM_BANDS, FFT_SIZE, NUM_BINS, SAMPLE_RATE, MEL_FMIN, MEL_FMAX
            ),
            dtype=np.float64,
        )
        _HANN = hann
        _FB = fb
        _FB_PINV = np.linalg.pinv(fb)
    return _HANN, _FB, _FB_PINV


def n_samples_for_frames(n_frames: int) -> int:
    if n_frames < 1:
        return 0
    return WINDOW_LENGTH + HOP * (n_frames - 1)


def energy_to_power(energy: np.ndarray, method: str = "pinv") -> np.ndarray:
    """energy (bands, frames) → power (bins, frames) ≥ 0.

    Hub default = pinv+clip. NNLS даёт ||fb@H−E||≈0, но listen 2026-08-25:
    хуже на слух (разреженные пики в 257 bins при 64 уравнениях). Не включать в Hub.
    """
    _, fb, fb_pinv = _lut()
    if method == "pinv":
        return np.maximum(fb_pinv @ energy, 0.0)
    if method == "nnls":
        return _nnls_power(fb, fb_pinv, energy)
    raise MelInvertError("bad_power_method")


def _nnls_power(fb: np.ndarray, fb_pinv: np.ndarray, energy: np.ndarray) -> np.ndarray:
    try:
        from scipy.optimize import nnls
    except ImportError:
        return _mu_nnls(fb, fb_pinv, energy)
    frames = int(energy.shape[1])
    out = np.empty((fb.shape[1], frames), dtype=np.float64)
    for t in range(frames):
        out[:, t], _r = nnls(fb, energy[:, t])
    return out


def _mu_nnls(
    fb: np.ndarray, fb_pinv: np.ndarray, energy: np.ndarray, *, n_iter: int = 80
) -> np.ndarray:
    """Non-negative LS via multiplicative updates (no scipy)."""
    h = np.maximum(fb_pinv @ energy, 1e-18)
    wt = fb.T
    wtw = wt @ fb
    for _ in range(n_iter):
        h *= (wt @ energy) / (wtw @ h + 1e-18)
    return h


def _init_stft(mag: np.ndarray, rng: np.random.Generator, phase_init: str) -> np.ndarray:
    if phase_init == "prev":
        spec = np.empty(mag.shape, dtype=np.complex128)
        spec[:, 0] = mag[:, 0] * np.exp(2j * math.pi * rng.random(mag.shape[0]))
        for t in range(1, mag.shape[1]):
            spec[:, t] = mag[:, t] * np.exp(1j * np.angle(spec[:, t - 1]))
        return spec
    return mag * np.exp(2j * math.pi * rng.random(mag.shape))


def griffin_lim(
    mag: np.ndarray,
    hann: np.ndarray,
    n_iter: int,
    rng: np.random.Generator,
    *,
    fast: bool = False,
    alpha: float = 0.99,
    phase_init: str = "random",
) -> np.ndarray:
    """mag: (bins, frames) |X|; окно/hop как прошивка (center=False)."""
    n_frames = int(mag.shape[1])
    n = n_samples_for_frames(n_frames)
    spec = _init_stft(mag, rng, phase_init)
    w = hann[:WINDOW_LENGTH]
    wsq_eps = 1e-2 * float(np.max(w * w))

    def _istft(cur: np.ndarray) -> np.ndarray:
        acc = np.zeros(n, dtype=np.float64)
        wsum = np.zeros(n, dtype=np.float64)
        for t in range(n_frames):
            buf = np.fft.irfft(cur[:, t], n=FFT_SIZE)
            start = t * HOP
            acc[start : start + WINDOW_LENGTH] += buf[:WINDOW_LENGTH] * w
            wsum[start : start + WINDOW_LENGTH] += w * w
        y = np.zeros(n, dtype=np.float64)
        ok = wsum >= wsq_eps
        y[ok] = acc[ok] / wsum[ok]
        return y

    t_prev: np.ndarray | None = None
    for _ in range(n_iter):
        y = _istft(spec)
        projected = np.empty_like(spec)
        for t in range(n_frames):
            start = t * HOP
            buf = np.zeros(FFT_SIZE, dtype=np.float64)
            buf[:WINDOW_LENGTH] = y[start : start + WINDOW_LENGTH] * w
            rebuilt = np.fft.rfft(buf, n=FFT_SIZE)
            projected[:, t] = mag[:, t] * rebuilt / (np.abs(rebuilt) + 1e-12)
        if fast and t_prev is not None:
            mom = projected + float(alpha) * (projected - t_prev)
            spec = mag * mom / (np.abs(mom) + 1e-12)
        else:
            spec = projected
        t_prev = projected
    return _istft(spec)


def invert_logmel(
    log_mel: np.ndarray,
    n_iter: int | None = None,
    seed: int = 2,
    *,
    power_method: str = "pinv",
    fast: bool | None = None,
    phase_init: str = "random",
) -> np.ndarray:
    """log_mel: (64, frames) ln(E+ε) → PCM float. Hub default = pinv + Fast GL."""
    if log_mel.ndim != 2 or log_mel.shape[0] != MEL_NUM_BANDS:
        raise MelInvertError("unsupported_bands")
    n_frames = int(log_mel.shape[1])
    if n_frames < 2 or n_frames > FRAMES_MAX:
        raise MelInvertError("bad_frames")
    hann, _fb, _pinv = _lut()
    energy = np.maximum(np.exp(log_mel) - LOG_EPS, 0.0)
    power = energy_to_power(energy, power_method)
    mag = np.sqrt(power * float(FFT_SIZE))
    use_fast = invert_fast() if fast is None else bool(fast)
    return griffin_lim(
        mag,
        hann,
        n_iter if n_iter is not None else invert_iters(),
        np.random.default_rng(seed),
        fast=use_fast,
        phase_init=phase_init,
    )


def pcm_to_wav_bytes(pcm_f: np.ndarray) -> bytes:
    """Норма по p99.5 тела клипа (не пик клика на краях Hann)."""
    hop = HOP
    if pcm_f.size > 2 * hop:
        body = pcm_f[hop:-hop]
    else:
        body = pcm_f
    ref = float(np.percentile(np.abs(body), 99.5)) if body.size else 0.0
    if ref < 1e-6:
        i16 = np.zeros(pcm_f.shape[0], dtype=np.int16)
    else:
        scale = 0.35 / ref
        i16 = np.clip(np.rint(pcm_f * scale * 32767.0), -32768, 32767).astype(np.int16)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(i16.tobytes())
    return buf.getvalue()


def uint8_logmel_to_wav(
    data: bytes,
    bands: int,
    frames: int,
    dmin: float,
    dmax: float,
    *,
    n_iter: int | None = None,
    power_method: str = "pinv",
    fast: bool | None = None,
    phase_init: str = "random",
) -> bytes:
    if bands != MEL_NUM_BANDS:
        raise MelInvertError("unsupported_bands")
    if frames < 2 or frames > FRAMES_MAX:
        raise MelInvertError("bad_frames")
    if not math.isfinite(dmin) or not math.isfinite(dmax):
        raise MelInvertError("missing_minmax")
    need = bands * frames
    if len(data) < need:
        raise MelInvertError("short_payload")
    u8 = np.frombuffer(data[:need], dtype=np.uint8).reshape(frames, bands)
    span = float(dmax) - float(dmin)
    log_mel = (float(dmin) + u8.astype(np.float64) * (span / 255.0)).T
    return pcm_to_wav_bytes(
        invert_logmel(
            log_mel,
            n_iter=n_iter,
            seed=2,
            power_method=power_method,
            fast=fast,
            phase_init=phase_init,
        )
    )


def wav_path_for_bin(bin_path: Path) -> Path:
    return bin_path.with_suffix(".wav")


def wav_rev_path(wav_path: Path) -> Path:
    return Path(str(wav_path) + ".rev")


def _wav_cache_hit(wav_path: Path, bin_path: Path) -> bool:
    try:
        if not wav_path.is_file() or not bin_path.is_file():
            return False
        if wav_path.stat().st_size <= 44:
            return False
        if wav_path.stat().st_mtime < bin_path.stat().st_mtime:
            return False
        return wav_rev_path(wav_path).read_text(encoding="utf-8").strip() == invert_rev()
    except OSError:
        return False


def ensure_mel_wav(bin_path: Path, meta: dict[str, Any] | None = None, *, n_iter: int | None = None) -> bytes:
    """Ленивый кэш: mel_*.bin → mel_*.wav. Пересчёт если bin новее или сменился invert_rev."""
    bin_path = Path(bin_path)
    wav_path = wav_path_for_bin(bin_path)
    if _wav_cache_hit(wav_path, bin_path):
        return wav_path.read_bytes()
    if not bin_path.is_file():
        raise FileNotFoundError(bin_path.name)
    if meta is None:
        meta_p = Path(str(bin_path) + ".meta.json")
        if not meta_p.is_file():
            raise MelInvertError("missing_meta")
        try:
            meta = json.loads(meta_p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise MelInvertError("bad_meta") from exc
    bands = int(meta.get("num_bands") or 0)
    frames = int(meta.get("num_frames") or 0)
    dmin = meta.get("data_min")
    dmax = meta.get("data_max")
    data = bin_path.read_bytes()
    with _LOCK:
        if _wav_cache_hit(wav_path, bin_path):
            return wav_path.read_bytes()
        blob = uint8_logmel_to_wav(
            data, bands, frames, float(dmin), float(dmax), n_iter=n_iter
        )
        tmp = wav_path.with_suffix(wav_path.suffix + ".tmp")
        tmp.write_bytes(blob)
        tmp.replace(wav_path)
        wav_rev_path(wav_path).write_text(invert_rev() + "\n", encoding="utf-8")
    return blob
