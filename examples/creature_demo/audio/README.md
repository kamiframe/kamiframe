# Demo creature audio

**The creature's sounds are square waves, not recordings.** There are no
audio assets here to pack, load or license. Every sound is a short list of
notes played through `kf_audio_tone(hz, ms)` — the definitions live in
`tools/kf_chiptune.py`'s `SOUNDS` table.

To hear them without flashing a board:

```
python3 tools/kf_chiptune.py /tmp/preview     # writes a .wav per sound
python3 tools/kf_chiptune.py --list           # just the note specs
```

The `.wav` files that script writes are **previews for human ears**. They
are not shipped and not packed; the device synthesises from the notes.

## How this got here, because the dead end is worth recording

This started as text-to-speech: generate a voice, pitch it up, bit-crush
it, pack it as PCM. That produced a chipmunk. Pitch-shifting preserves
every cue that says *human* — harmonics, formants, breath — so a sped-up
voice is a small person, not a synthetic creature. Chiptune is a different
*waveform*, not a faster one.

Chris, 2026-08-13: *"Maybe I'm actually looking for 8-bit nintendo midi
representations of chirps."* That was the correct instinct, and abandoning
samples deleted an entire pipeline that had not been built yet:

- no `ASSET_TYPE_AUDIO_CLIP` packer support (still reserved, still unused)
- no PCM decoder in `hakoniwaos/src/assets.cpp`
- no sample-rate conversion, and no float maths in an integer-only Core
- no flash cost per sound (a 0.3s clip was ~14 KB; a note list is bytes)
- **no third-party audio licence to reason about at all**

That last one mattered. The first source was a purchased Humble Bundle
asset pack, which permits *use* in a game but not redistribution of the raw
files — a genuine problem for a public Apache-2.0 repository where forking
is the point. See `LICENSING.md` for the line this project already draws
around the demo creature's artwork. **A frequency belongs to nobody**, so
that whole question disappeared.

`attention_voice_source.wav` may still be sitting in this directory
untracked (`.gitignore` excludes `*.wav` here). It is the abandoned TTS
source, kept only in case the sampled route is ever revisited. Nothing
reads it.

## The design

Every sound is a transformation of **one motif: the rising major triad**.
Extended and resolved for rare events, fragmentary for frequent ones,
inverted for bad news, minor for the worst of it. Twelve sounds that share
a shape read as one creature; twelve unrelated jingles read as a sound
pack.

**Duty cycle is the second lever and it is free.** 50% is the fat classic
beep, 12.5% the thin nasal NES pulse — same oscillator, same cost. So the
**creature's own voice is a thin pulse** and system fanfares are fat, which
separates "the pet is talking" from "the device is announcing something"
without changing a single note.

**The five wants each get their own call.** `kf_pet_want` has five values,
and giving each a distinct sound lets a player learn what the creature
needs by ear, without looking at the screen — something the original
Tamagotchi never did.

One rejected idea is worth keeping, because it explains the shape of all
five. The first want sound was *repeat-then-leap* — the same note twice,
then a jump up. That is a **summons**: the shape of a doorbell. Wanting is
a **question** — rising, unresolved, faintly plaintive. `want_flush`
deliberately ends on the second rather than the tonic, so it never
resolves; the tension is the point.

## Not built yet

The notes exist; the playback path does not.

1. **`kf.melody("E6:55 -:35 B6:85")`** — a Lua binding taking a note-name
   string. Note names, not raw Hz, because the SDK is the product and
   `{{1318,60},{1568,60}}` is not something a jQuery developer writes.
2. **Duty-cycle support** in `kf/hal/audio.h`, which is currently tone-only
   with a fixed 50% square.
3. **The queue depth.** `ports/esp32/hal/esp_audio.cpp` uses a depth-1
   queue with `xQueueOverwrite`, so two `kf_audio_tone()` calls in quick
   succession *replace* each other. **A multi-note phrase currently plays
   only its last note on hardware.** Nothing in this file works on a real
   board until that is fixed.
