/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * kf/clock.h's implementation. No file-static state -- there is no offset
 * to hold (see kf/clock.h's header comment), so every function here is a
 * pure conversion over its own arguments.
 */

#include "kf/clock.h"

#include "kf/poison.h"

namespace {

/* Seconds in a day. Named rather than repeated so a reviewer sees the same
 * constant everywhere it matters. */
constexpr int64_t kSecondsPerDay = 86400;

/* Floor division: a/b rounded toward negative infinity, unlike C's `/`,
 * which truncates toward zero. This is the exact hazard the design note in
 * kf/clock.h and the plan warn about: with plain `/`, -3600 / 86400 is 0
 * (truncated toward zero) when it must be -1 (floored) for a negative epoch
 * -- one second before 1970-01-01T00:00:00 belongs to the day BEFORE day 0,
 * not to day 0 itself. Every day/remainder split in this file goes through
 * this function so that hazard cannot resurface in a second place by
 * someone reaching for `/` and `%` directly. `b` is always a positive
 * compile-time-known constant (86400) at every call site in this file. */
int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

/* Civil (Gregorian) date <-> days-since-1970-01-01, using Howard Hinnant's
 * "days_from_civil" / "civil_from_days" algorithm --
 * http://howardhinnant.github.io/date_algorithms.html -- reproduced here in
 * plain C++ rather than pulled in as a dependency: it is roughly twenty
 * lines each way, integer-only, branch-free, and correct proleptically (it
 * does not special-case any particular year range, which is what lets
 * kf_civil.year legally be negative without this code needing a second
 * path). The algorithm's own derivation re-bases the calendar so a "year"
 * runs March-to-February -- that is what `y -= m <= 2` and the `mp`
 * (month-index, 0 = March) juggling below are doing; the two-line comments
 * are Hinnant's own, kept because reconstructing the reasoning from the
 * arithmetic alone is exactly the kind of "half-remembered" implementation
 * this module's header explicitly warns against being. */

/* Preconditions: m in [1, 12], d in [1, last_day_of_month(y, m)]. */
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;                        /* [0, 399] */
    const int64_t doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;        /* [0, 365] */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* Inverse of days_from_civil. z is days since 1970-01-01. */
void civil_from_days(int64_t z, int64_t *y_out, int64_t *m_out,
                      int64_t *d_out) {
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;                          /* [0, 146096] */
    const int64_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;      /* [0, 399] */
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    /* [0, 365] */
    const int64_t mp = (5 * doy + 2) / 153;                         /* [0, 11] */
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                 /* [1, 31] */
    const int64_t m = mp + (mp < 10 ? 3 : -9);                      /* [1, 12] */
    *y_out = y + (m <= 2 ? 1 : 0);
    *m_out = m;
    *d_out = d;
}

/* The cumulative count of window-seconds elapsed by time `t` (any second
 * count on the same axis `t` -- see the two call sites), for a window that
 * always begins at the origin of every `period`-second cycle and runs for
 * `duration` seconds. F is monotonic non-decreasing, so the seconds inside
 * the window over a span [from, to) is exactly
 * window_cumulative(to, ...) - window_cumulative(from, ...) --
 * kf_clock_seconds_in_daily_window()'s entire body is that one subtraction,
 * once the caller has been reduced to this shape. That reduction is the
 * "whole windows plus two partials" the header promises: `cycles` below is
 * the whole-window count, and `portion` is the single partial window `t`
 * currently sits inside (0 for a `t` before any window has started this
 * cycle, `duration` for one at or past the window's end this cycle -- so
 * there are really only ever two partial windows total across a whole call,
 * one contributed by `from` and one by `to`, not one per day the span
 * touches). */
int64_t window_cumulative(int64_t t, int64_t period, int64_t duration) {
    const int64_t cycles = floor_div(t, period);
    const int64_t rem = t - cycles * period; /* in [0, period) */
    const int64_t portion = (rem < duration) ? rem : duration;
    return cycles * duration + portion;
}

} // namespace

void kf_civil_from_epoch(int64_t epoch_seconds, kf_civil *out) {
    if (out == nullptr) {
        return;
    }
    const int64_t days = floor_div(epoch_seconds, kSecondsPerDay);
    const int64_t secs_of_day = epoch_seconds - days * kSecondsPerDay; /* [0, 86400) */

    int64_t y = 0;
    int64_t m = 0;
    int64_t d = 0;
    civil_from_days(days, &y, &m, &d);

    out->year = static_cast<int32_t>(y);
    out->month = static_cast<uint8_t>(m);
    out->day = static_cast<uint8_t>(d);
    out->hour = static_cast<uint8_t>(secs_of_day / 3600);
    out->minute = static_cast<uint8_t>((secs_of_day % 3600) / 60);
    out->second = static_cast<uint8_t>(secs_of_day % 60);
}

int64_t kf_epoch_from_civil(const kf_civil *in) {
    if (in == nullptr) {
        return 0;
    }
    const int64_t days = days_from_civil(in->year, in->month, in->day);
    return days * kSecondsPerDay + static_cast<int64_t>(in->hour) * 3600 +
           static_cast<int64_t>(in->minute) * 60 +
           static_cast<int64_t>(in->second);
}

int64_t kf_clock_seconds_in_daily_window(int64_t from, int64_t to,
                                          uint8_t start_hour,
                                          uint8_t end_hour) {
    /* Duration of the window in seconds, folded into [0, 24h] with modular
     * arithmetic that treats a wrap (end_hour <= start_hour) and a plain
     * same-day window (start_hour < end_hour) identically -- there is no
     * separate branch for the wrapping case anywhere in this function,
     * which is deliberate: a loop-based implementation typically grows one,
     * and that branch is exactly where the two disagree at the edges. For
     * example start_hour=22, end_hour=7: (7 - 22 + 24) % 24 = 9h, the
     * night's true length. start_hour == end_hour folds to 0 (an empty
     * window), per kf/clock.h's documented choice. */
    const int64_t duration_seconds =
        ((static_cast<int64_t>(end_hour) - static_cast<int64_t>(start_hour)) %
             24 +
         24) %
        24 * 3600;

    /* Shift the whole axis so the window starts at 0 within a 24h cycle
     * that itself begins at start_hour every day. That is the "whole
     * windows plus two partials" trick: window_cumulative() only ever has
     * to reason about a window at the very start of its cycle, so the
     * wrap-midnight case never needs code of its own -- shifting by
     * start_hour is what turns "22:00 through 07:00" into "0:00 through
     * 09:00 of a day that itself starts at 22:00", which is a plain,
     * non-wrapping window. */
    const int64_t shift = static_cast<int64_t>(start_hour) * 3600;

    return window_cumulative(to - shift, kSecondsPerDay, duration_seconds) -
           window_cumulative(from - shift, kSecondsPerDay, duration_seconds);
}
