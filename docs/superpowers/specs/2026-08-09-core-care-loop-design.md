# Core care loop — first iteration

**Date:** 2026-08-09
**Status:** Agreed, not yet implemented

The first real gameplay slice: Tamagotchi-style pet care, made more demanding
than the current placeholder, with one system of its own that distinguishes
it from a basic virtual pet — personality-driven preferences that the player
discovers by experiment.

Everything here builds on what already exists (ADR 0015 needs and decay, ADR
0021 life stages, ADR 0023 personality traits). Nothing in this document
requires a new simulation framework.

---

## 1. The loop

Four care types: **feed, play, clean, sleep.**

Three of them map to needs that already exist — hunger, happiness, energy.
The fourth, clean, works differently and is described in section 4.

The player's job is to keep the creature fed, entertained, rested and clean,
learning as they go which *kind* of each it prefers.

## 2. Demand curve

Demand scales with life stage, expressed as time from full until the
creature needs attention:

| Stage | Needs attention | Critical |
|---|---|---|
| Egg | never (no decay) | — |
| Baby | ~30 min | ~90 min |
| Child | ~1 hour | ~3 hours |
| Teen | ~2 hours | ~6 hours |
| Adult | ~4 hours | ~12 hours |

A baby genuinely owns the player's attention; an adult is a companion they
check on. This replaces today's uniform, glacial rates (hunger takes four
real days to empty, which is why nothing appears to happen).

**Every figure here is config, in one table.** This is the single set of
numbers most likely to be wrong after a week of living with it, and changing
them must not mean touching logic.

## 3. Sleep

The creature sleeps on a schedule, against **real wall-clock time** from the
DS3231 — it knows it is 3am even if the device spent two days in a drawer.

It becomes drowsy at its own hour. The player settles it down, and decay
nearly stops until morning. Keeping it up has a cost.

This is what makes a 30-minute baby survivable overnight, and it is load
bearing rather than flavour: without it, the demand curve above is
unplayable.

Player-initiated naps at any time were rejected — they hollow out the demand
curve, because the optimal play becomes keeping the creature asleep whenever
the player is busy. Scheduled night sleep now; creature-requested daytime
naps are a natural later addition.

## 4. Mess

Two mechanics under one care type.

**Poops** are discrete objects. They appear on a timer (sooner after
eating), sit on screen until cleared, and accumulate. The mess *is* the
state — visible across the room, no bar to read.

**Dirtiness** is a continuous value that rises over time and faster while
poops are lying around. At thresholds it becomes visible: flies first, then
stink lines.

All three cleaning variations address both; they differ only in how well,
according to the creature's preference.

## 5. Personality and preference

Two layers, both already in the data model.

- **Base trait** — one of six, rolled once at birth, never changes.
- **Developed trait** — one of three, earned from how the creature has
  actually been treated (`kf_pet_dominant_care_trait()`).

**Preferences live on the base trait.** They are fixed and knowable, and they
transfer between creatures: learning what trait 3 likes serves every future
pet with that trait. Discovery is *learning the system*, so a player can be
right, share findings, and accumulate expertise.

**Three variations per care action**, twelve in total. For any given trait,
one is **liked**, one **neutral**, one **disliked**. That is a 6 × 4 table —
24 facts about the world, small enough to hold in your head and to find a
tuning mistake in.

Effects:

- **Liked** — restores noticeably more, happy reaction.
- **Neutral** — baseline restore.
- **Disliked** — restores little, visible objection, and repeated use pushes
  the developed traits. Mistreatment shapes who the creature becomes rather
  than merely being inefficient.

### Open question: what the developed trait does

Agreed as desirable, mechanism undecided. It must influence care somehow, but
we did not settle on how.

The leading candidate, recorded so it is not lost: the developed trait shifts
**how much** rather than **what** — a play-leaning creature gets more out of
play in general, while the base trait still owns which variation it prefers.
That keeps the discovery table clean and gives the earned trait real weight.

This is deliberately unresolved. Do not implement a mechanism for it in this
iteration without deciding it first.

## 6. Feedback

**The reaction leads.** Every care action produces a visible response — loved
it, fine, or objected — readable at a glance with no numbers. This is what
makes discovery possible and what makes the thing feel like a creature rather
than a form.

**The bars confirm.** Magnitude differs by preference, so a player watching
closely gets precision for free. The bars already exist and already move.

Deliberately opaque feedback was rejected: with 6 traits × 4 care types ×
3 variations, a player with no immediate signal is not solving a puzzle, they
are waiting days to find out they guessed wrong.

## 7. Sickness and death

**Sickness is a state, not a stat.** Triggered by sustained neglect: mess left
too long, dirtiness too high, or a need left critical past its threshold.
While sick, needs decay faster and happiness drains, so it compounds if
ignored.

**Curing it is care, not a separate button.** Clean the creature up and let it
rest and it recovers. A dedicated medicine action was rejected as a fifth
action that is only ever "press when red" — but it is the obvious fallback if
recovery-through-care proves too forgiving.

**Death is real, and heavily telegraphed.** It follows only sustained critical
sickness, with escalating distress before it. Every death should feel
deserved rather than unlucky — at a 30-minute baby demand, a pet that dies
during a meeting teaches people to stop carrying it.

Permanent death with no warning was rejected for that reason. No death at all
was rejected because the stakes are the point; and the softer consequence
comes free regardless, since evolution already branches off care history, so
a neglected creature grows into something visibly different.

## 8. Not in this iteration

- **Babysitter hand-off** (as in Tamagotchi Uni / Paradise). Wanted, deferred.
  Death-with-warning gives it a clear job later: the sanctioned way to hand
  over responsibility, not a convenience.
- **Unlock progression.** Variations will be earned through play eventually;
  all twelve are available now so the interactions can be tuned.
- **Variations four and five.** Three is enough to prove the shape; five is
  where this grows once the loop is fun.
- **How the developed trait influences care** (section 5).
- **Anything beyond care** — no mini-games, toys, or social features.
- **Audio.** The hardware is planned but unwired, and there is no audio HAL.
  Sound effects exist and will be packed like sprites when it lands.

## 9. What this touches

- `kf/pet.h`, `pet.cpp` — per-stage decay config; mess state (poop count,
  dirtiness); sickness state; a variation argument on the care actions.
- The Lua API — care actions taking a variation, and exposing mess, sickness
  and traits to scripts.
- The pet screen — mess objects, reactions, sickness indication.
- Assets — creature expressions (happy, neutral, objecting, sick, sleeping),
  poop, flies, stink lines. Sprites to be generated; the asset pipeline to
  carry them is being built in parallel.

**Creature sprite size: 48x48 as the base**, a fifth of the screen's width
and about a sixth of its height — roughly the proportion Tamagotchi Uni
gives its creature. A multiple of 8, which suits the blitter. Per-stage
override allowed so an adult can carry more presence than a baby.

At 48x48 a frame is 4.5KB, so the 10MB asset budget holds thousands of
frames. Size is a design choice here, not a constraint.

This forces a layout pass on the pet screen. Three stat bars and their
labels currently take most of the vertical space; with real art the
creature should be what the eye goes to, which likely means the needs
become compact icons or a single row rather than three full-width bars.

## 10. Decisions and what was rejected

| Decision | Rejected alternative | Why |
|---|---|---|
| Preferences on the base trait | Per-creature random roll | Knowledge transfers; the player can be right and can share findings |
| | Both layers at once | Cannot tell a mislearned type from an unusual individual |
| Reaction leads, bars confirm | Numbers only | Cold, and hard to read at a glance on a 2" screen |
| | Deliberately opaque | Days of waiting to discover a wrong guess |
| Baby needs care every 20–40 min | Every 1–2 hours | Wanted closer to the original's intensity |
| Scheduled sleep | Player-initiated naps | An off switch for the entire demand curve |
| Poops as objects **and** a dirtiness value | One cleanliness bar | A fourth bar is another number; mess should look like mess |
| Three variations to start | Five | Five times the art and tuning before the loop is proven fun |
| Death after telegraphed sickness | Sudden permanent death | Deaths must feel deserved |
| | No death | Stakes are the point |
| Cure by care | A medicine action | A fifth action that is only ever "press when red" |

---

## Addendum, 2026-08-09: how the character bible maps onto the code's stages

`14-character-bible-v1.md` names three stages (baby, juvenile, adult) while
the simulation has five. Chris resolved the mismatch: the bible was
describing the stages where characters are *distinct*, not the whole life.

| Code stage | Bible | Designs needed |
|---|---|---|
| Egg | not in the bible; always first | 1 |
| Baby | not in the bible; "barely a thing", needy, featureless | 1, shared |
| Child | the bible's **Baby** — Marumaru, the uncut blank | 1, shared |
| Teen | the bible's **Juvenile** — one per verb family | 4 |
| Adult | the bible's **Adult** | 10 confirmed |

So the code keeps all five stages and the bible's three map onto the last
three. Nothing in the simulation's stage machinery changes.

### What does NOT yet reconcile: the branch shape

| | Bible | Code today |
|---|---|---|
| Teen forms | 4 verb families (Cut, Hold, Mark, Go) | `KF_PET_TEEN_FORM_COUNT` = 3 |
| Adults per family | uneven — Cut 2, Hold 3, Mark 3, Go 1 | `KF_PET_ADULT_BRANCH_COUNT` = 2, uniform |
| Total adults | 10, plus Hokorimaru | 6 |

Three consequences for whoever implements this:

1. **Teen forms must become 4**, not 3.
2. **Adults per family are uneven and will change.** The bible's own section
   11 says Go and Cut still need more creatures to balance at three each. So
   the code should carry a per-family count table rather than one constant —
   otherwise every roster addition is a breaking change to the tree.
3. **Hokorimaru is not a normal branch.** It is what a creature becomes when
   it is *never interacted with at all* — a separate condition from the
   care-quality average that drives the other branches, and explicitly
   flagged in the bible's section 8 as needing its own mechanism.

Changing the tree means bumping the save format version, since `teen_form`
and `adult_branch` are stored.
