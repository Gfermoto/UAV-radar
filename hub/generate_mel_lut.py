#!/usr/bin/env python3
"""
Генератор compile-time MEL LUT (Look-Up Table).

Предвычисляет:
  1. Окно Ханна (512 float) — Hann window, zero-pad 400→512
  2. MEL-фильтрбанк (64×257 float) — треугольные фильтры по HTK-формуле
  3. Таблица перевода FFT-бина в частоту (для отладки/верификации)

Результат: src/mel_lut.h — C-заголовок с const-массивами.

Параметры согласованы с Config.h и MelSpectrogram.h:
  - Частота дискретизации: 16 кГц
  - FFT: 512 точек, пиковый бин = 257
  - MEL: 64 полосы, HTK-формула, 125–7500 Гц
  - Окно: Ханна, 400 точек окна + 112 zero-pad

Использование:
  python tools/generate_mel_lut.py

  # Результат: src/mel_lut.h (~70 КБ flash)
"""

import argparse
import math
import os


# ---- Параметры (по умолчанию 16 кГц) ----
SAMPLE_RATE = 16000
FFT_SIZE = 512
NUM_BINS = FFT_SIZE // 2 + 1  # 257
WINDOW_LENGTH = 400  # 25 мс @ 16 кГц
MEL_NUM_BANDS = 64
MEL_FMIN = 125
MEL_FMAX = 7500

OUTPUT_NAME = "mel_lut.h"


def parse_args():
    global OUTPUT_NAME
    parser = argparse.ArgumentParser(description="Generate the 16 kHz firmware MEL LUT")
    parser.add_argument("--output", default=OUTPUT_NAME)
    args = parser.parse_args()
    OUTPUT_NAME = args.output


def hz_to_mel(hz: float) -> float:
    """HTK-формула: mel = 2595 * log10(1 + hz / 700)."""
    return 2595.0 * math.log10(1.0 + hz / 700.0)


def mel_to_hz(mel: float) -> float:
    """Обратная HTK: hz = 700 * (10^(mel / 2595) - 1)."""
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def generate_hann_window(window_len: int, fft_size: int) -> list[float]:
    """
    Окно Ханна: w[i] = 0.5 * (1 - cos(2π·i/(N-1))) для i < window_len.
    Остальные fft_size - window_len — нули (zero-pad).
    """
    window = []
    for i in range(fft_size):
        if i < window_len:
            w = 0.5 * (1.0 - math.cos(2.0 * math.pi * i / (window_len - 1)))
        else:
            w = 0.0  # zero-pad
        window.append(w)
    return window


def generate_filterbank(
    num_bands: int,
    fft_size: int,
    num_bins: int,
    sample_rate: int,
    fmin: float,
    fmax: float,
) -> list[list[float]]:
    """
    Генерация треугольных MEL-фильтров.

    Идентична MelSpectrogram::generateFilterbank() в C++.
    Каждый фильтр: пик 1.0 в центре MEL-полосы, линейный спад к соседним центрам.

    Returns:
        filterbank[num_bands][num_bins] — неразреженная матрица (все веса float).
    """
    low_mel = hz_to_mel(fmin)
    high_mel = hz_to_mel(fmax)
    mel_step = (high_mel - low_mel) / (num_bands + 1)

    # Инициализация нулями
    filterbank = [[0.0] * num_bins for _ in range(num_bands)]

    for b in range(num_bands):
        center_mel = low_mel + (b + 1) * mel_step
        center_hz = mel_to_hz(center_mel)
        left_hz = mel_to_hz(center_mel - mel_step)
        right_hz = mel_to_hz(center_mel + mel_step)

        # FFT-бин = round(f * N / Fs)
        left_bin = round(left_hz * fft_size / sample_rate)
        center_bin = round(center_hz * fft_size / sample_rate)
        right_bin = round(right_hz * fft_size / sample_rate)

        # Ограничение границ
        left_bin = max(0, left_bin)
        right_bin = min(num_bins - 1, right_bin)

        for i in range(left_bin, right_bin + 1):
            if i <= center_bin:
                den = float(center_bin - left_bin)
                filterbank[b][i] = ((i - left_bin) / den) if den > 0.0 else 1.0
            else:
                den = float(right_bin - center_bin)
                filterbank[b][i] = ((right_bin - i) / den) if den > 0.0 else 1.0

    return filterbank


def generate_bin_frequencies(fft_size: int, sample_rate: int, num_bins: int) -> list[float]:
    """Частота каждого FFT-бина (Гц)."""
    return [i * sample_rate / fft_size for i in range(num_bins)]


def format_float_array(name: str, data: list[float], per_line: int = 8) -> str:
    """Форматирование float-массива в C-строку."""
    lines = []
    lines.append(f"static const float {name}[] = {{")
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        formatted = ", ".join(f"{v:.8f}f" for v in chunk)
        if i + per_line < len(data):
            formatted += ","
        lines.append(f"    {formatted}")
    lines.append("};")
    return "\n".join(lines)


def format_2d_array(name: str, data: list[list[float]], per_line: int = 8) -> str:
    """Форматирование двумерного float-массива в C-строку (row-major)."""
    rows = len(data)
    cols = len(data[0])
    total = rows * cols
    flat = [data[r][c] for r in range(rows) for c in range(cols)]

    lines = []
    lines.append(f"/** {rows}×{cols} MEL filterbank, row-major, {total} floats ({total*4/1024:.1f} KB). */")
    lines.append(f"static const float {name}[] = {{")
    for i in range(0, len(flat), per_line):
        chunk = flat[i:i + per_line]
        formatted = ", ".join(f"{v:.8f}f" for v in chunk)
        if i + per_line < len(flat):
            formatted += ","
        lines.append(f"    {formatted}")
    lines.append("};")
    return "\n".join(lines)


def main():
    parse_args()
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    output_path = os.path.join(repo_root, "src", OUTPUT_NAME)

    # Генерация данных
    hann = generate_hann_window(WINDOW_LENGTH, FFT_SIZE)
    filterbank = generate_filterbank(
        MEL_NUM_BANDS, FFT_SIZE, NUM_BINS, SAMPLE_RATE, MEL_FMIN, MEL_FMAX
    )
    bin_freqs = generate_bin_frequencies(FFT_SIZE, SAMPLE_RATE, NUM_BINS)

    # Верификация
    print(f"Окно Ханна:    {len(hann)} float ({len(hann)*4/1024:.1f} KB)")
    print(f"Фильтрбанк:    {MEL_NUM_BANDS}×{NUM_BINS} = {MEL_NUM_BANDS*NUM_BINS} float ({MEL_NUM_BANDS*NUM_BINS*4/1024:.1f} KB)")
    print(f"  Band 0  peak: bin {hann_to_peak_bin(filterbank[0]):3d}, {bin_freqs[hann_to_peak_bin(filterbank[0])]:.1f} Hz")
    print(f"  Band 31 peak: bin {hann_to_peak_bin(filterbank[31]):3d}, {bin_freqs[hann_to_peak_bin(filterbank[31])]:.1f} Hz")
    print(f"  Band 63 peak: bin {hann_to_peak_bin(filterbank[63]):3d}, {bin_freqs[hann_to_peak_bin(filterbank[63])]:.1f} Hz")
    print()

    # Генерация заголовка
    header = f"""\
/**
 * @file    mel_lut.h
 * @brief   Compile-time MEL Look-Up Table — предвычисленные окно Ханна и MEL-фильтрбанк.
 *
 * Сгенерировано: tools/generate_mel_lut.py
 * НЕ РЕДАКТИРОВАТЬ ВРУЧНУЮ — перегенерировать скриптом.
 *
 * Параметры:
 *   Fs = {SAMPLE_RATE} Гц, FFT = {FFT_SIZE}, бинов = {NUM_BINS}
 *   MEL = {MEL_NUM_BANDS} полос, {MEL_FMIN}–{MEL_FMAX} Гц, HTK-формула
 *   Окно Ханна: {WINDOW_LENGTH} точек + {FFT_SIZE-WINDOW_LENGTH} zero-pad
 *
 * Память (flash, не RAM):
 *   MEL_HANN_WINDOW:   {len(hann)} × 4 = {len(hann)*4/1024:.1f} KB
 *   MEL_FILTERBANK:    {MEL_NUM_BANDS}×{NUM_BINS} × 4 = {MEL_NUM_BANDS*NUM_BINS*4/1024:.1f} KB
 *   Итого:             {(len(hann) + MEL_NUM_BANDS*NUM_BINS)*4/1024:.1f} KB
 *
 * Экономия:
 *   PSRAM: ~68 KB (окно + фильтрбанк больше не выделяются в PSRAM)
 *   Startup: ~2 сек (генерация фильтрбанка исключена из begin())
 *
 * @see MelSpectrogram.h
 * @see tools/generate_mel_lut.py
 */

#ifndef MEL_LUT_H
#define MEL_LUT_H

#include <Arduino.h>
#include <cstdint>

// ---- Размерности (из Config.h) ----

#define MEL_LUT_NUM_BANDS  {MEL_NUM_BANDS}
#define MEL_LUT_FFT_SIZE   {FFT_SIZE}
#define MEL_LUT_NUM_BINS   {NUM_BINS}

// ---- Окно Ханна (512 float) ----

{format_float_array("MEL_HANN_WINDOW", hann)}

// ---- MEL-фильтрбанк ({MEL_NUM_BANDS}×{NUM_BINS} float, row-major) ----

{format_2d_array("MEL_FILTERBANK", filterbank)}

/**
 * @brief Быстрый доступ к фильтрбанку: band[b][bin].
 *        Индексация: MEL_FILTERBANK_AT(b, bin) = MEL_FILTERBANK[b * MEL_LUT_NUM_BINS + bin]
 */
#define MEL_FILTERBANK_AT(b, bin) MEL_FILTERBANK[(b) * MEL_LUT_NUM_BINS + (bin)]

#endif // MEL_LUT_H
"""

    with open(output_path, "w") as f:
        f.write(header)

    print(f"OK → {output_path}")
    print(f"  Размер: {os.path.getsize(output_path) / 1024:.1f} KB")


def hann_to_peak_bin(filter_row: list[float]) -> int:
    """Индекс максимального значения в строке фильтрбанка."""
    return max(range(len(filter_row)), key=lambda i: filter_row[i])


if __name__ == "__main__":
    main()
