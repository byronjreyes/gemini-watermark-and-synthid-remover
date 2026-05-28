"""
Convert reverse-SynthID spectral codebooks (.npz) to wmr binary format (.wcb).

Supports:
  - V3 format (format_version 2): float32 mag/phase, HxW prefix
  - V3 expanded (format_version 2): same format, more resolutions
  - V4 format (format_version 5): quantized uint8/int8 + scale, model|HxW prefix

The Python codebooks store R2C (real-to-complex) FFT data with shape
(rows, cols//2+1). Our C++ FFTW3 wrapper produces full complex output
(rows, cols). This converter expands the half-spectrum to full size using
conjugate symmetry: magnitude is mirrored, phase is negated.

Usage:
    python3 scripts/convert_npz_to_wcb.py <input.npz> <output.wcb>
"""

import struct
import sys
import numpy as np


MAGIC = b"WMRCB01"


def expand_r2c_to_full(half_array, full_cols):
    """Expand R2C half-spectrum (rows, cols//2+1) to full (rows, cols).

    Uses conjugate symmetry: F[r, c] = conj(F[(H-r)%H, (W-c)%W])
    For magnitude (real): mirror rows AND columns.
    For phase: same mirroring but negate the right half (handled by caller).
    """
    rows, half_cols = half_array.shape
    full = np.zeros((rows, full_cols), dtype=half_array.dtype)
    full[:, :half_cols] = half_array
    # Mirror columns to right side using conjugate symmetry.
    # R2C stores columns 0..half_cols-1. Column 0 (DC) is unique.
    # For even W: half_cols = W//2+1. Columns 1..half_cols-2 mirror to W-1..half_cols.
    #   Nyquist at half_cols-1 is unique (real-valued).
    # For odd W: half_cols = (W+1)//2. Columns 1..half_cols-1 mirror to W-1..half_cols.
    #   No pure Nyquist bin exists.
    right_size = full_cols - half_cols
    # Mirror columns 1..right_size (inclusive) from the left side
    right_cols = half_array[:, 1:right_size + 1][:, ::-1]  # reverse order
    row_indices = np.arange(rows)
    conj_rows = (rows - row_indices) % rows
    full[conj_rows, half_cols:] = right_cols
    return full


def expand_channel_r2c(half_mag, half_phase, half_cons, full_cols):
    """Expand R2C arrays for a single channel to full size."""
    full_mag = expand_r2c_to_full(half_mag, full_cols)
    full_phase = expand_r2c_to_full(half_phase, full_cols)
    # Negate phase for the mirrored part (conjugate symmetry)
    half_cols = half_phase.shape[1]
    full_phase[:, half_cols:] = -full_phase[:, half_cols:]
    full_cons = expand_r2c_to_full(half_cons, full_cols)
    return full_mag, full_phase, full_cons


def convert_dense(npz, prefix, w, h, sample_count):
    """Convert dense format arrays and expand to full spectrum."""
    mag = npz[f"{prefix}/mag"].astype(np.float32)   # (rows, half_cols, 3)
    phase = npz[f"{prefix}/phase"].astype(np.float32)
    cons = npz[f"{prefix}/cons"].astype(np.float32) / 255.0

    rows, half_cols, n_ch = mag.shape
    full_cols = w  # Full image width = full FFT width

    print(f"  R2C shape: ({rows}, {half_cols}) -> expanding to ({rows}, {full_cols})")

    mag_channels = []
    phase_channels = []
    cons_channels = []

    for ch in range(n_ch):
        fm, fp, fc = expand_channel_r2c(mag[:, :, ch], phase[:, :, ch], cons[:, :, ch], full_cols)
        mag_channels.append(fm)
        phase_channels.append(fp)
        cons_channels.append(fc)

    mag_full = np.stack(mag_channels, axis=-1)
    phase_full = np.stack(phase_channels, axis=-1)
    cons_full = np.stack(cons_channels, axis=-1)

    return w, h, sample_count, rows, full_cols, mag_full, phase_full, cons_full


def convert_sparse(npz, prefix, w, h, sample_count):
    """Convert sparse format to dense arrays and expand to full spectrum."""
    full_rows = h
    full_cols = w

    mag_channels = []
    phase_channels = []
    cons_channels = []

    for ch in range(3):
        idx = npz[f"{prefix}/idx_{ch}"]
        m = npz[f"{prefix}/mag_{ch}"].astype(np.float32)
        p = npz[f"{prefix}/phase_{ch}"].astype(np.float32)
        c = npz[f"{prefix}/cons_{ch}"].astype(np.float32) / 255.0

        # Sparse indices reference into R2C (rows, cols//2+1) space
        half_cols = full_cols // 2 + 1

        half_mag = np.zeros((full_rows, half_cols), dtype=np.float32)
        half_phase = np.zeros((full_rows, half_cols), dtype=np.float32)
        half_cons = np.zeros((full_rows, half_cols), dtype=np.float32)

        row_idx = idx // half_cols
        col_idx = idx % half_cols

        half_mag[row_idx, col_idx] = m
        half_phase[row_idx, col_idx] = p
        half_cons[row_idx, col_idx] = c

        fm, fp, fc = expand_channel_r2c(half_mag, half_phase, half_cons, full_cols)
        mag_channels.append(fm)
        phase_channels.append(fp)
        cons_channels.append(fc)

    mag = np.stack(mag_channels, axis=-1)
    phase = np.stack(phase_channels, axis=-1)
    cons = np.stack(cons_channels, axis=-1)

    return w, h, sample_count, full_rows, full_cols, mag, phase, cons


def convert_v4_profile(npz, prefix):
    """Convert a single V4 (format 5) quantized profile.

    V4 uses uint8 mag + float scale, int8 phase + float scale, uint8 cons.
    Prefix format: "model|HxW" (rows x cols)
    """
    # Parse dimensions from prefix: "model|HxW"
    dims = prefix.split("|")[1]  # "HxW"
    h, w = int(dims.split("x")[0]), int(dims.split("x")[1])

    # Dequantize
    mag_raw = npz[f"{prefix}/mag"]  # uint8
    phase_raw = npz[f"{prefix}/phase"]  # int8
    cons_raw = npz[f"{prefix}/cons"]  # uint8

    mag_scale = float(npz[f"{prefix}/mag__scale"])
    phase_scale = float(npz[f"{prefix}/phase__scale"])

    mag = mag_raw.astype(np.float32) * mag_scale
    phase = phase_raw.astype(np.float32) * phase_scale
    cons = cons_raw.astype(np.float32) / 255.0

    rows, half_cols, n_ch = mag.shape
    full_cols = w

    n_refs = int(npz.get(f"{prefix}/n_content_refs", 0))
    # Count total reference images
    ref_vals = npz.get(f"{prefix}/ref_counts_vals")
    sample_count = int(np.sum(ref_vals)) if ref_vals is not None else n_refs

    print(f"  R2C shape: ({rows}, {half_cols}) -> expanding to ({rows}, {full_cols})")
    print(f"  mag scale={mag_scale:.4f}, phase scale={phase_scale:.4f}")
    print(f"  Total references: {sample_count}")

    mag_channels = []
    phase_channels = []
    cons_channels = []

    for ch in range(n_ch):
        fm, fp, fc = expand_channel_r2c(mag[:, :, ch], phase[:, :, ch], cons[:, :, ch], full_cols)
        mag_channels.append(fm)
        phase_channels.append(fp)
        cons_channels.append(fc)

    mag_full = np.stack(mag_channels, axis=-1)
    phase_full = np.stack(phase_channels, axis=-1)
    cons_full = np.stack(cons_channels, axis=-1)

    return w, h, sample_count, rows, full_cols, mag_full, phase_full, cons_full


def detect_format(npz):
    """Detect codebook format version and return (version, is_v4)."""
    fv = int(npz["format_version"])
    if fv >= 5:
        return fv, True
    return fv, False


def get_v3_profiles(npz):
    """Get list of (prefix, w, h) for V3 format codebooks."""
    resolutions = npz["resolutions"]
    profiles = []
    for res_idx in range(len(resolutions)):
        h, w = int(resolutions[res_idx][0]), int(resolutions[res_idx][1])
        prefix = f"{h}x{w}"
        profiles.append((prefix, w, h))
    return profiles


def get_v4_profiles(npz):
    """Get list of prefixes for V4 format codebooks."""
    prefixes = set()
    for k in npz.files:
        if '/' in k and k != "format_version":
            prefix = k.rsplit('/', 1)[0]
            prefixes.add(prefix)
    return sorted(prefixes)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.npz> <output.wcb>")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    npz = np.load(input_path, allow_pickle=True)

    format_version, is_v4 = detect_format(npz)
    print(f"Format version: {format_version} ({'V4 quantized' if is_v4 else 'V3 float32'})")

    profiles = []

    if is_v4:
        v4_prefixes = get_v4_profiles(npz)
        print(f"Profiles: {len(v4_prefixes)}")

        for prefix in v4_prefixes:
            dims = prefix.split("|")[1]
            print(f"\nProcessing {prefix}...")
            profile = convert_v4_profile(npz, prefix)
            profiles.append(profile)
            _, _, _, rows, cols, mag, phase, cons = profile
            print(f"  Output: {rows}x{cols}x3, mag [{float(mag.min()):.2f}, {float(mag.max()):.2f}]")
    else:
        v3_profiles = get_v3_profiles(npz)
        print(f"Profiles: {len(v3_profiles)}")

        for prefix, w, h in v3_profiles:
            print(f"\nProcessing {prefix}...")

            n_black = int(npz.get(f"{prefix}/n_black_refs", 0))
            n_white = int(npz.get(f"{prefix}/n_white_refs", 0))
            n_random = int(npz.get(f"{prefix}/n_random_refs", 0))
            sample_count = n_black + n_white + n_random
            is_sparse = bool(int(npz.get(f"{prefix}/sparse", 0)))

            print(f"  Samples: {n_black} black + {n_white} white + {n_random} random = {sample_count}")
            print(f"  Format: {'sparse' if is_sparse else 'dense'}")

            if is_sparse:
                profile = convert_sparse(npz, prefix, w, h, sample_count)
            else:
                profile = convert_dense(npz, prefix, w, h, sample_count)

            profiles.append(profile)
            _, _, _, rows, cols, mag, phase, cons = profile
            print(f"  Output: {rows}x{cols}x3, mag range [{float(mag.min()):.2f}, {float(mag.max()):.2f}]")

    # Write .wcb file
    with open(output_path, "wb") as f:
        f.write(MAGIC)

        count = len(profiles)
        f.write(struct.pack("<I", count))

        for w, h, sample_count, rows, cols, mag, phase, cons in profiles:
            f.write(struct.pack("<I", w))
            f.write(struct.pack("<I", h))
            f.write(struct.pack("<i", sample_count))

            for ch in range(3):
                f.write(struct.pack("<I", rows))
                f.write(struct.pack("<I", cols))

                f.write(mag[:, :, ch].astype(np.float32).tobytes())
                f.write(phase[:, :, ch].astype(np.float32).tobytes())
                f.write(cons[:, :, ch].astype(np.float32).tobytes())

    import os
    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nWrote {output_path} ({size_mb:.1f} MB, {count} profiles)")


if __name__ == "__main__":
    main()
