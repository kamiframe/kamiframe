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

### Resolved, 2026-08-09: the bible's shape wins

Chris: "Just go with the character bible's branch shape. keep egg and baby
regardless."

So the tree becomes:

- **Five stages stay.** Egg and baby are kept regardless of the bible not
  naming them.
- **Four teen forms**, one per verb family: Cut, Hold, Mark, Go.
- **Adults per family are uneven** and will change as the roster fills: Cut 2,
  Hold 3, Mark 3, Go 1. The code carries a per-family count table, not one
  constant, so adding the creatures the bible's section 11 still wants is a
  data edit rather than a breaking change.
- **Hokorimaru is reached by never interacting at all**, which is a different
  condition from the care-quality average every other branch uses.

---

## Addendum, 2026-08-09: why sleep is not the next thing built

Sickness and death landed first, which makes sleep more urgent than section 3
suggests — a creature that needs attention every thirty minutes and can now
die of accumulated neglect, with no night, is incoherent. It was planned next
and deliberately stopped, because two questions in it are not technical.

**The hard part is offline, not live.** While the device is on, segments are
one frame long and a sleep window is trivial. Offline, a single segment can
span a fortnight and a dozen nights, and this file's standing rule is that no
loop may be bounded by elapsed time. So the seconds falling inside a daily
window have to be solved for analytically rather than stepped through — which
is tractable (whole days plus two partials), but it has to be got right the
first time, because a mistake there silently corrupts offline ageing, which is
the feature the entire product rests on.

**The part that needs Chris is the interaction.** Section 3 says the creature
becomes drowsy and *the player settles it down*, with a cost to keeping it up.
That is a real and good interaction while someone is watching. It has no
meaning at all across a fortnight in a drawer, where there is no player to
settle anything — so the offline rule has to be "it sleeps", and the live rule
has to be "it sleeps if you let it". Those are two different behaviours
sharing one state, and which one applies is a judgement about how the thing
should feel, not a technical fact.

Two smaller decisions ride along with it: whether being asleep should suspend
the neglect clock (it must, or the player is punished for the creature
sleeping — but then an unwoken creature is immune, which the automatic morning
wake has to be trusted to prevent), and what the creature's night is in local
terms, since the wall clock is UTC and a creature that sleeps at 22:00 GMT is
wrong for most of the people who will own one.

**One thing was settled and is worth keeping.** Core has no clock by design,
and `last_advanced` currently only moves when a save is loaded, so during live
play the pet has no idea what time it is. The fix is for `kf_pet_advance()` to
carry `last_advanced` forward by the same elapsed seconds that drive decay:
wall time then enters Core exactly once, at load, and time of day is available
everywhere without Core ever reading a clock. That is a good change regardless
of what sleep ends up looking like.

---

## Addendum, 2026-08-09 (evening): Chris's decisions

Five rulings, and what each one changed.

### A drawer is fatal

A creature left alone runs its needs down, calls out, and dies — exactly as
the original Tamagotchi did. No exemption for total neglect. This overturned
the earlier rule that spared never-touched creatures.

**Consequence, resolved here:** the dust form (Hokorimaru) was reached by
never interacting with a creature at all. That is now impossible — an
untouched creature dies during childhood, days before the branch point. Left
alone, the bible would have documented a character no player could ever
obtain.

So the condition became the nearest thing that survives: **kept alive and
nothing more.** A care average across the whole of CHILD below
`dust_care_average_mp` grows the creature into dust rather than one of the
four verb families. Same story — neglect made visible — by a route that does
not require the creature to be dead first.

`dust_care_average_mp` is **provisional at 20%** and is the number here most
in need of play rather than argument. It is squeezed between two hard walls:
too high and it swallows the worst verb-family band, so a badly raised
creature never gets a real form; too low and nothing alive can reach it,
because surviving already means keeping every need above the neglect
threshold most of the time, which drags the average up on its own. The window
between "barely raised" and "raised badly" is genuinely narrow.

### Death without a player holds on the last creature's scene

No auto-hatching. The device sits on the death scene until someone comes back
and starts a new egg. This is also what gives the deferred babysitter hand-off
a real job: the sanctioned way to avoid this, rather than a convenience.

**Shrine, not a gravestone — recommended.** A small roadside shrine reads as
remembrance and continuity, where a headstone reads as a plot in a Western
graveyard; the shrine also carries the visual language the rest of the project
already uses, and it gives the eventual "start a new egg" action somewhere
natural to live — you leave an offering rather than dig. Not yet built:
nothing draws any scene at all yet.

### Cleaning is two actions, not one

Split, because they were never really one thing:

**Bath** — washes the creature. Dirtiness to zero, always, whichever variation
was used. Being clean is a *need*, and a need met badly is still met; a
creature left dirty because it disliked the flannel would punish the player
for doing the right thing in the wrong style. What preference buys is a small
happiness bonus on top: a noticeable lift for the way it likes, barely
anything for a way it tolerates, and nothing for the way it hates. Nothing
negative, ever — there is deliberately no config value that could make it so.

**Flush** — clears the poops. One way to do it, no variations, no opinion, no
effect on any need. It does not touch dirtiness either, but it does slow how
fast dirtiness climbs, since waiting poops accelerate it. A player who flushes
but never baths has slowed the problem without solving it.

This replaced a thoroughness mechanic where a disliked clean left mess behind.
That was a defensible reading of "they differ only in how well" and it was
wrong: it made meeting a need conditional on style.

### The clock is a device setting, not a per-creature one

Local time is set once in the device's global settings and every creature on
it shares that. This settles the timezone half of sleep — Core carries a UTC
offset from config, and nothing per-egg.

The other half of sleep is settled too, as of later the same day — see "Sleep,
settled" at the end of this addendum.

### Egg and baby have designs now

**Egg:** deliberately generic. Nothing about it hints at what is inside.

**Baby:** a small drifting spirit wisp — soft tapering body, no limbs, no
ground contact, trailing off at the bottom, with a vague ghostly face in manga
shorthand. Shared by every creature.

**Marumaru, at the CHILD stage, remains the first form that is actually its
own thing.** Both new designs are placeholders in the sense that the bible
does not describe either stage; neither is an attempt to invent bible content.

### Sleep, settled

The earlier addendum ("why sleep is not the next thing built") stopped because
the drowsy/settle interaction has no meaning offline. Chris's answer removes
the problem rather than solving it: **the creature falls asleep by itself.**
Settling it is optional decoration on top, not the mechanism sleep depends on.
That makes the live and offline rules the same rule, which is what unblocked
this.

**Night is 22:00 to 07:00 local**, using the device-wide UTC offset decided
above. Generic on purpose for now. The eventual intent is that the device
learns the real zone by itself — over BLE from a phone, or WiFi — so nobody
ever sets a clock; that is future work, not this build.

**Falling asleep is automatic.** At bedtime the creature gets drowsy, and if
nothing happens it plops down where it stands and sleeps. Offline, that is all
that happens, which is why offline needs no separate rule.

**Being put to bed is the optional interaction.** The drowsy state is the
signal that this is available: while drowsy, the creature can be brought to
bedding and tucked in. It is a nicety, not a duty — skipping it costs nothing
beyond the late-night deficit below.

**Waking up is entirely the creature's own.** In the morning it wakes, gets
itself out of bed, and puts the bedding away. The player is never required to
do anything to end sleep, which is what stops "asleep forever" from being a
reachable state.

**A late night costs deficits, paid the next day.** Staying up past bedtime
keeps draining, so the following day starts in a hole that needs extra care to
climb out of. This is what makes bedtime matter without punishing anyone who
simply is not holding the device at 22:00.

**Waking it deliberately is allowed and costs happiness.** The original
punished you for leaving the light on; this is the same idea, kept small.

**The neglect clock pauses while asleep, but the needs do not.** These are two
different things and the distinction is the point: `neglect_seconds` — the
accumulator that drives sickness and death — does not advance while the
creature sleeps, so nobody is punished for it sleeping. Hunger and the rest
keep decaying normally, so you wake up to a creature that wants something.
There is something to do in the morning, and it still cannot kill you overnight.

**Consequence, and Chris's answer to it.** Because nights do not accrue
neglect, sickness and death arrive on roughly fifteen hours a day rather than
twenty-four. Chris's decision: **compress it, so a waking day still costs a
full day's worth.**

Two corrections to how this was first framed, both established by reading the
code rather than reasoning about it:

*Hokorimaru is not affected and needs no compensation.* The dust branch is
selected from `care_integral_mp_seconds / stage_elapsed_seconds`
(`pet.cpp:328`) — an average of need levels across the whole child stage. The
needs keep decaying while the creature sleeps and the stage clock keeps
running, so that average never notices the neglect clock stopping. Only
sickness and death, the two things `neglect_seconds` drives, change at all.

*The compression belongs in the thresholds, not the accrual.* Cut
`sickness_onset_seconds` and `sickness_death_seconds` by the waking fraction;
do not multiply the rate at which `neglect_seconds` climbs. Two reasons. The
accumulator is part of the save format and is read straight out in the debug
timeline, so it should keep meaning literal seconds spent neglected and awake.
More importantly, neglect *recovers* as well as accrues — `pet.cpp:610`
subtracts time spent cared-for back off it — so scaling only the accrual would
make the creature harsher in a direction nobody asked for, while moving the
thresholds scales both directions at once.

Build it as a single named constant. The late-night deficit above and this
compression both land on the same waking day, and whether their combined
pressure feels right is a thing to feel rather than derive.

**Noted for later, not now: sleeping by actual darkness.** The target hardware
carries an ambient light sensor, so the creature could settle when it is really
dark — a cloth over it, a drawer, lights out. Chris wants this eventually. It
needs a simulator fallback, since a desktop has no such sensor, and it is a
larger build than the clock-driven version. Clock first.
