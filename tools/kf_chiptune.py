#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Preview Kamiframe's chiptune sounds on a desktop, as .wav files.

The device does not play samples. It plays square waves, one note at a
time, through kf_audio_tone(hz, ms) -- so every sound in this file is a
short list of notes, and this script exists only so a human can HEAR one
without flashing a board. What it writes is a preview; what ships is the
note list.

Written to match the firmware's own constraints rather than to sound good
in isolation: integer square waves, no floating-point synthesis, no
samples, one voice at a time. If it plays here it is implementable there.

WHY SQUARE WAVES AND NOT A RECORDED VOICE. This started as a text-to-speech
clip pitched up and bit-crushed. It sounded like a chipmunk, because
pitch-shifting preserves every cue that says "human" -- harmonics,
formants, breath. Chiptune is not a pitched voice, it is a different
waveform. Chris, 2026-08-13: "Maybe I'm actually looking for 8-bit nintendo
midi representations of chirps." He was right, and it deleted an entire
pipeline: no PCM decoder, no ASSET_TYPE_AUDIO_CLIP, no resampling, no flash
cost per sound, and no third-party audio licence to worry about, because a
frequency belongs to nobody.

THE MOTIF. Every sound here is a transformation of one idea: the rising
major triad. Extended and resolved for rare events (hatching), fragmentary
for frequent ones (a want), inverted for bad news (dislike, death), minor
for the worst of it. That is what makes twelve sounds an identity rather
than a pile of effects -- the same trick Zelda and Pokemon use, and it
costs nothing here because they are all just notes.

DUTY CYCLE IS THE SECOND LEVER, and it is free. A 50% square is the fat
classic beep; 12.5% is the thin nasal NES pulse. Same oscillator, same
cost, different timbre -- so THE CREATURE'S OWN VOICE IS A THIN PULSE and
system fanfares are fat. That is what separates "the pet is talking" from
"the device is announcing something" without needing different notes.

Usage:
    python3 tools/kf_chiptune.py out_dir/          # every sound
    python3 tools/kf_chiptune.py out_dir/ want_food
    python3 tools/kf_chiptune.py --list
"""

import argparse
import math
import struct
import sys
import wave

RATE = 22050          # matches the device's I2S rate (ports/esp32/hal/esp_audio.cpp)
AMPLITUDE = 9000      # headroom; the device clips hard above this

# Duty cycles. Named, because "0.125" at a call site says nothing.
THIN = 0.125          # the creature's own voice
MID = 0.25
FAT = 0.5             # fanfares and system sounds

NOTES = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5,
         "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}


def note_hz(name):
    """'C6' / 'F#5' -> Hz. A4 = 440, MIDI numbering. '-' is a rest."""
    name = name.strip()
    if not name or name in ("-", "r", "R"):
        return 0
    octave = int(name[-1])
    return 440.0 * (2.0 ** (((octave + 1) * 12 + NOTES[name[:-1].upper()] - 69) / 12.0))


def square(hz, ms, duty=FAT):
    """One square-wave note, the way a PWM peripheral would make it."""
    n = int(RATE * ms / 1000)
    if hz <= 0:
        return [0] * n
    period = RATE / hz
    ramp = int(RATE * 0.002)
    out = []
    for i in range(n):
        v = AMPLITUDE if (i % period) / period < duty else -AMPLITUDE
        # A 2ms release, purely to stop the click a hard cut makes on a
        # small speaker. Integer-only, so the device can do the same.
        tail = n - i
        out.append(v * tail // ramp if tail < ramp else v)
    return out


def render(spec, duty=FAT, gap_ms=8):
    """spec: "E6:55 -:35 B6:85" -- note:ms, '-' rests, bare note = 70ms."""
    out = []
    for tok in spec.split():
        name, _, ms = tok.partition(":")
        out += square(note_hz(name), int(ms) if ms else 70, duty)
        out += [0] * int(RATE * gap_ms / 1000)
    return out


# ---------------------------------------------------------------------------
# THIS TABLE AND examples/creature_demo/creature.lua's OWN SOUNDS TABLE MUST
# BE CHANGED TOGETHER. This file is the preview tool -- it renders a .wav a
# human can audition before flashing anything, and does not run on the
# device. creature.lua's copy (note specs, duty tier, note names -- the same
# grammar kf.melody() parses) is the shipping copy that actually plays.
# Changing a note here without mirroring it there previews a sound the
# device does not make; changing it there without mirroring it here makes
# this script's preview a lie.
#
# The approved set. Chris picked these by ear on 2026-08-13, from 12 initial
# candidates plus 8 follow-ups for "wants" specifically.
#
# THE WANTS ARE FIVE DIFFERENT SOUNDS, NOT ONE. The pet has five distinct
# wants (kf_pet_want, hakoniwaos/include/kf/pet.h) and giving each its own
# call means a player learns what it needs by ear, without looking at the
# screen. His call: "you can use them like you said where it might use a
# different want or happy sound depending on what type of care you are
# doing to it."
#
# The first attempt at a want sound was rejected, and the reason is worth
# keeping: repeat-then-leap (same note twice, then a jump up) is a SUMMONS.
# It is the shape of a doorbell. Wanting is a QUESTION -- rising,
# unresolved, a little plaintive. Every want below is built that way.
# ---------------------------------------------------------------------------
SOUNDS = {
    # -- Wants: one per kf_pet_want ----------------------------------------
    "want_food":      (THIN, "C7:32 C7:32 C7:32 F7:75"),   # insistent; hunger nags
    "want_play":      (THIN, "G6:55 C7:55 E7:90"),         # bright, upbeat
    "want_rest":      (THIN, "E6:70 -:30 B6:120"),         # slow, wide, tired
    "want_bath":      (THIN, "C7:50 A6:50 D7:95"),         # dips first: mildly put out
    "want_flush":     (THIN, "E6:55 A6:55 D7:100"),        # ends UNRESOLVED, on the second

    # Escalation, when a want goes ignored. Two rungs: asked again, then
    # a whine. Deliberately the same creature, not a new alarm.
    "want_again":     (THIN, "A6:50 C7:65 -:60 C7:50 E7:85"),
    "want_whine":     (THIN, "B6:40 A#6:40 B6:40 A#6:40 D7:85"),

    # -- Care responses ----------------------------------------------------
    # One phrase, transposed per action, so "you did something" is instantly
    # recognisable while still telling you WHICH. Cheaper and more coherent
    # than five unrelated jingles.
    "care_feed":      (MID, "C7:50 E7:70"),
    "care_play":      (MID, "D7:50 F#7:70"),
    "care_rest":      (MID, "E7:50 G#7:70"),
    "care_bath":      (MID, "F6:50 A6:70"),
    "care_flush":     (MID, "G6:50 B6:70"),
    # The motif inverted: falls, and lands flat.
    "care_disliked":  (MID, "E6:60 C6:90"),

    # -- Daily -------------------------------------------------------------
    "wake":           (MID,  "C6:70 E6:70 G6:110"),
    "sleep":          (THIN, "G6:90 E6:90 C6:150"),

    # -- Rare: the motif fully extended -----------------------------------
    "hatch":          (FAT, "C6:60 E6:60 G6:60 C7:60 E7:60 G7:60 C8:220"),
    "evolve":         (FAT, "G6:55 C7:55 E7:55 G7:55 E7:55 G7:55 C8:240"),
    # Inverted, slowed, and minor -- same shape, wrong colour.
    "death":          (MID, "G6:150 D#6:150 C6:150 G5:400"),

    # -- Character ---------------------------------------------------------
    "idle_warble":    (THIN, "E7:35 B6:35 E7:35 B6:35 E7:60"),
    "confused":       (THIN, "C7:30 G6:30 D7:30 A6:30 E7:55"),
    "menu_blip":      (THIN, "G6:30"),
}


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", s) for s in samples))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("out_dir", nargs="?", help="directory to write .wav previews into")
    p.add_argument("names", nargs="*", help="specific sounds (default: all)")
    p.add_argument("--list", action="store_true", help="print the note specs and exit")
    args = p.parse_args()

    if args.list or not args.out_dir:
        for name, (duty, spec) in SOUNDS.items():
            print(f"{name:16s} duty={duty:<6} {spec}")
        return 0

    wanted = args.names or list(SOUNDS)
    for name in wanted:
        if name not in SOUNDS:
            print(f"unknown sound '{name}' -- try --list", file=sys.stderr)
            return 1
        duty, spec = SOUNDS[name]
        write_wav(f"{args.out_dir}/{name}.wav", render(spec, duty))
        print(f"  {name:16s} {spec}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
