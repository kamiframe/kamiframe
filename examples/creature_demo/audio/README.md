# Demo creature audio — staging, not yet a pipeline

Raw source audio waiting on a pack pipeline that does not exist yet. **No
code reads anything in this directory.** It is here so the file is findable
when the work happens, not because anything works today.

## The raw `.wav` files here are deliberately NOT committed

`.gitignore` excludes `examples/creature_demo/audio/*.wav`, on purpose, and
that exclusion should not be removed without answering the licence question
below first.

`attention_voice_source.wav` came from a **purchased "Puzzle Audio Bundle"**
(`Dropbox/Game Dev Projects/Music & SFX/Puzzle Audio Bundle/WAV/FX/Voices/
Confusion Voice 4.wav`). This repository is public and Apache 2.0. Most
commercial audio-pack licences permit *using* a sound in a game while
prohibiting *redistribution of the raw asset files* — and pushing one to a
public repository is redistribution, regardless of intent.

**Unanswered, and it gates shipping this sound at all:**

1. Does the bundle's licence permit redistribution of the source `.wav`?
   (Usually no.)
2. Does it permit distributing a *processed, embedded* form — resampled,
   converted to 16-bit PCM and packed into a `.kfpack`? (Often yes, as
   "use in a game", but bundles vary and some require the sound be
   "integrated" rather than extractable.)
3. If (2) is yes but (1) is no, the pipeline must consume this file from
   **outside** the repo, the way `tools/kf_ingest_sprites.py` can already
   be pointed at any directory.

Kamiframe already keeps a hard line here — see `LICENSING.md`, where the
demo creature's artwork is copyright-reserved while its code is Apache 2.0,
and hardware lives in a separate CERN-OHL-W repository specifically so the
licences never blur. Audio should not be the thing that blurs them.

## What this file actually is

Measured, not assumed:

| | |
|---|---|
| Format | IEEE float (RIFF tag 3), **not** integer PCM |
| Channels | 2 (stereo) |
| Sample rate | 44,100 Hz |
| Bit depth | 32-bit |
| Duration | 0.332 s |
| On disk | 126,110 bytes |

Converted to the packer's reserved audio shape (16-bit signed PCM, mono,
uncompressed — see `tools/kf_pack_assets.py`'s format comment):

- at **22,050 Hz**: ~14.3 KB
- at **44,100 Hz**: ~28.6 KB

Either is negligible against `KF_FLASH_ASSET_BUDGET_BYTES` (12 MB). Length
is the friendly part: a third of a second streams comfortably; a multi-second
line would be a different engineering problem.

## What has to be built before it makes a sound

Four gaps, in dependency order:

1. **Packer support.** `ASSET_TYPE_AUDIO_CLIP = 1` is *reserved* in
   `tools/kf_pack_assets.py` with its intended `type_meta` already written
   down (sample_rate u32, channels u8, bits_per_sample u8), but nothing
   packs or reads it.
2. **A loader.** `hakoniwaos/src/assets.cpp` has no audio decoder.
3. **A HAL capability.** `kf/hal/audio.h` is deliberately tone-only —
   ADR 0055 rejected sampled playback because there was no way to author a
   clip. This file is what makes that reasoning expire.
4. **Conversion.** Stereo 32-bit float → mono 16-bit PCM, and a sample-rate
   decision. Core is integer-only, so any resampling happens **host-side at
   pack time**, never on device.

## An API decision that is not mine to make

`kf.beep()` and `kf.tone(hz, ms)` are the whole sound API today, and
third-party developers build against it. A creature voice could be a
general `kf.play("name")` for any packed clip — more useful to other
developers, more work — or a single built-in attention sound, which is
quicker and does not generalise. The SDK is the product, which argues for
the general one, but it is a scope call.
