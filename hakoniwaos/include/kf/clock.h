/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Civil time, in Core, integers only.
 *
 * This module turns a wall-clock second count into "what hour is it" and
 * back, plus one analytic query: how many seconds of a span fall inside a
 * daily clock-time window such as 22:00-07:00. It exists because two
 * different places need that same answer and must never disagree about it --
 * the Settings screen's 12-hour clock display (Task 4 of docs/superpowers/
 * plans/2026-08-13-screens-clock-sleep.md) and sleep's night-window
 * accounting (that plan's Task 6). Putting the arithmetic here once, in
 * Core, is what keeps the clock on screen and the hour the pet falls asleep
 * from drifting apart from each other.
 *
 * NO TIMEZONE, NO UTC OFFSET, ON PURPOSE -- settled by Chris, recorded in the
 * plan under "Timezone: settled by Chris". The device has no timezone
 * database, no network, and no location, so "local time" can only honestly
 * mean one thing: whatever the owner dials into the Settings screen by hand.
 * The RTC (kf/hal/time.h's kf_time_wall()) is therefore defined to hold
 * LOCAL time directly, not UTC -- the epoch second this module is handed
 * already IS the wall clock's own reckoning, with nothing to add or
 * subtract. That is why every function below takes and returns a plain
 * `epoch_seconds` with no offset parameter anywhere: a field nothing sets is
 * wrong the moment something reads it, and once a field like that ships in
 * the save format it has to be carried forever. There was an earlier draft
 * of this header with a stored UTC-offset accessor; it was replaced before
 * being implemented, once this decision landed, rather than shipped and
 * removed later.
 *
 * This module is consequently completely STATELESS -- no file-static
 * variable, nothing to initialise, nothing to persist. Every function is a
 * pure conversion over its arguments. That statelessness is also what keeps
 * these conversions safe across a wall-clock jump: nothing here remembers
 * what time it last returned, so it does not matter whether the clock was
 * just hand-edited on the Settings screen, corrected by a future internet
 * time sync, or has never moved at all -- each call is independent and
 * correct for whatever epoch_seconds it is given. Do not build anything on
 * top of this module that assumes wall time advances one second per real
 * second, or that it never moves backward or jumps forward: it already can,
 * from the Settings screen alone, and a sync will make it more common, not
 * less.
 *
 * WHY NOT libc's localtime(). It ships on both the host and ESP-IDF, would
 * compile cleanly on both, and would look correct. It is still wrong for
 * this job, for three reasons that all bite later rather than immediately:
 *
 *   - Core's rule (CLAUDE.md) is that it talks to the HAL and nothing else.
 *     localtime() reaches around that into libc's own global state.
 *   - It depends on the process-global `TZ` variable, which neither the
 *     desktop build nor the device sets, so the same input could legally
 *     produce two different answers depending on what else in the process
 *     touched TZ first -- exactly the kind of nondeterminism this project's
 *     headless checks exist to rule out. (Moot for timezone conversion once
 *     there is no timezone to convert, but the TZ-dependence and the locale
 *     machinery below are reasons enough on their own.)
 *   - It drags in locale machinery on a build that is budgeted in kilobytes
 *     (kf/budget.h) and must stay heap-free (enforced by
 *     tools/check_no_heap.py) and float-free (a design rule that script
 *     does not check -- see kf/scene.h's own comment on this for the one
 *     place it slipped).
 *
 * The integer form used here -- days-since-epoch to a civil (year, month,
 * day) triple and back -- is about twenty lines, is well-documented (cited
 * in clock.cpp), and needs none of the above.
 *
 * Kept TOTAL AND HONEST on purpose: this module never claims to know
 * anything about timezones, it only ever converts the wall clock's own
 * epoch second to and from a calendar date. That is precisely what makes a
 * future internet time sync (the board has WiFi; wanted, not built) a
 * bounded job later -- it only has to CALL kf_time_set_wall() with a new
 * value, exactly like the Settings screen already does. It never has to
 * reinterpret what an already-stored timestamp meant, because this module
 * never attached timezone meaning to one in the first place.
 *
 * Valid C.
 */

#ifndef KF_CLOCK_H
#define KF_CLOCK_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A civil (calendar) date and time -- whatever the wall clock itself says,
 * with no conversion applied. Never used to represent anything but that. */
typedef struct {
    int32_t year;   /* e.g. 2026. May be negative for dates before year 0;
                      * this device will never see one, but the conversion
                      * functions below are correct for it anyway, which is
                      * the algorithm's own property, not a special case. */
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t hour;   /* 0..23 */
    uint8_t minute; /* 0..59 */
    uint8_t second; /* 0..59 */
} kf_civil;

/* epoch_seconds -- the wall clock's own second count, kf_time_wall()'s
 * epoch_seconds field -- as a calendar date and time. */
void kf_civil_from_epoch(int64_t epoch_seconds, kf_civil *out);

/* The inverse: a civil date/time -> the wall-clock epoch second it names.
 * Does not validate that *in* is a real calendar date (e.g. day 31 of a
 * 30-day month) -- callers that build a kf_civil by hand, such as the
 * Settings screen's editor, are responsible for keeping every field in its
 * documented range. */
int64_t kf_epoch_from_civil(const kf_civil *in);

/* How many seconds of the half-open span [from, to) -- both wall-clock
 * epoch seconds -- fall inside the daily clock-time window
 * [start_hour:00, end_hour:00).
 *
 * A window whose end_hour is <= start_hour wraps midnight -- e.g.
 * start_hour=22, end_hour=7 means 22:00 through 07:00 the following morning.
 * start_hour == end_hour is treated as an empty window (0 seconds), matching
 * the modular arithmetic below rather than as a special-cased full day.
 *
 * Solved analytically as whole windows plus two partial ones, never by
 * stepping through the span -- Task 6's offline sleep accounting can be
 * asked about a fortnight in one call. The trick: shift time so the window
 * always starts at 0 within a "window-day" of length 86400 seconds
 * beginning at start_hour. That turns the wrapping and non-wrapping cases
 * into the same formula -- see the .cpp for the derivation. */
int64_t kf_clock_seconds_in_daily_window(int64_t from, int64_t to,
                                          uint8_t start_hour,
                                          uint8_t end_hour);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_CLOCK_H */
