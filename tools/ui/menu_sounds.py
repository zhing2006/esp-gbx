# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Synthesize the menus' sound cues into assets/ui/common/sounds/.

Each cue is a 48 kHz, 16-bit mono WAV file built here from sine partials and
decay envelopes, so the sounds carry no third-party material. pack_assets.py
packs every WAV file in that folder. The menus play:

  move     the cursor moves or a value changes
  confirm  A or START does something
  back     B or MENU leaves a page
  denied   A or START on something that cannot act, such as a gray row
  dialog   a dialog with buttons opens

Every cue is scaled to the same loudness, well below full scale, since games
keep their own level and the volume setting sets both.
"""

import argparse
import wave
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

from uikit import ASSETS

RATE = 48000
OUTPUT = ASSETS / "common" / "sounds"

# The level every cue is scaled to: the K-weighted energy of its loudest
# 200 ms, in dB below full scale. A short cue spreads its energy over the
# whole window, as hearing sums a short sound's energy over about that long.
LOUDNESS = -21.0
LOUDNESS_WINDOW = RATE // 5
# The K-weighting filter of ITU-R BS.1770 at 48 kHz: a high shelf, then a
# high-pass.
K_WEIGHTING = (
    ((1.53512485958697, -2.69169618940638, 1.19839281085285), (1, -1.69065929318241, 0.73248077421585)),
    ((1, -2, 1), (1, -1.99004745483398, 0.99007225036621)),
)


def seconds(n):
    return np.arange(int(RATE * n)) / RATE


def note(freq, length, decay, partials=((1, 1.0), (2, 0.28), (3, 0.08)), attack=0.0015):
    """A struck tone: sine partials, higher ones dying away faster."""
    t = seconds(length)
    out = np.zeros_like(t)
    for ratio, level in partials:
        out += level * np.sin(2 * np.pi * freq * ratio * t) * np.exp(-t * ratio / decay)
    rise = np.clip(t / attack, 0, 1)
    return out * (0.5 - 0.5 * np.cos(np.pi * rise))


def place(parts, length):
    """Sums (start, samples) pairs into one clip of `length` seconds."""
    out = np.zeros(int(RATE * length))
    for start, samples in parts:
        at = int(RATE * start)
        n = min(len(samples), len(out) - at)
        out[at:at + n] += samples[:n]
    return out


def loudness(samples):
    """The K-weighted energy of the loudest LOUDNESS_WINDOW, in dB."""
    k = np.concatenate([samples, np.zeros(LOUDNESS_WINDOW)])
    for b, a in K_WEIGHTING:
        k = lfilter(b, a, k)
    window = np.convolve(k * k, np.ones(LOUDNESS_WINDOW))[:len(k)] / LOUDNESS_WINDOW
    return 10 * np.log10(window.max())


def finish(samples):
    """Fades the last 5 ms, scales the cue to LOUDNESS and pads with silence
    to whole 256-frame blocks, as the output is written."""
    fade = min(len(samples), int(RATE * 0.005))
    samples[-fade:] *= np.linspace(1, 0, fade)
    samples *= 10 ** ((LOUDNESS - loudness(samples)) / 20)
    assert np.max(np.abs(samples)) < 1, "the cue clips at LOUDNESS"
    pad = -len(samples) % 256
    return np.round(np.concatenate([samples, np.zeros(pad)]) * 32767).astype("<i2")


def cues():
    c5, e5, g5, c6, e6, g6 = 523.25, 659.26, 783.99, 1046.5, 1318.5, 1568.0
    return {
        # A soft, short tick; it plays on every step of a held key.
        "move": finish(note(2093.0, 0.03, 0.006, ((1, 1.0), (2.01, 0.2)))),
        "confirm": finish(place([(0, note(c6, 0.06, 0.03)), (0.05, note(g6, 0.16, 0.045))], 0.21)),
        "back": finish(place([(0, note(g5, 0.06, 0.03)), (0.05, note(c5, 0.15, 0.045))], 0.2)),
        # Two low, dull knocks: mostly the fundamental, quickly gone.
        "denied": finish(place([(0, note(196.0, 0.07, 0.02, ((1, 1.0), (3, 0.15)))),
                                (0.085, note(196.0, 0.07, 0.02, ((1, 1.0), (3, 0.15))))], 0.16)),
        # A rising arpeggio that rings on.
        "dialog": finish(place([(0, note(c5 * 2, 0.4, 0.08)), (0.06, note(e6, 0.34, 0.08)),
                                (0.12, note(g6, 0.3, 0.09))], 0.42)),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--output", type=Path, default=OUTPUT)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, samples in cues().items():
        with wave.open(str(args.output / f"{name}.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(samples.tobytes())
        print(f"{name}: {len(samples)} frames, {len(samples) / RATE * 1000:.0f} ms")


if __name__ == "__main__":
    main()
