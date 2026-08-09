/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_screen.h"

#include "../pet/kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/pet.h"

#include "kf_lvgl_idempotent.h"

#include <lvgl.h>

#include <cstdint>

namespace {

constexpr const char *TAG = "pet-screen";

/* Row carries no cached "last value" fields: kf_lvgl_idempotent.h's setters
 * ask the widget what it currently shows instead. That header explains why
 * skipping unchanged updates is a correctness requirement on real hardware
 * rather than an optimisation. */
struct Row {
    lv_obj_t *bar = nullptr;
    lv_obj_t *value_label = nullptr;
};

struct Screen {
    Row hunger;
    Row happiness;
    Row energy;
    lv_obj_t *blob = nullptr;
    lv_obj_t *blob_label = nullptr;
    bool ready = false;

    /* Last blob inputs pushed into the widgets -- see update_blob(). -1
     * rather than a valid enum value so the first update always draws. */
    int last_stage = -1;
    int last_teen_form = -1;
    int last_adult_branch = -1;
};
Screen g;

/* LV_THEME_SIMPLE (kf_lvgl_display.cpp) is exactly what its name says --
 * it does not style LV_STATE_FOCUSED at all, so with the theme alone a
 * keypad-focused button looks identical to an unfocused one. The old proof
 * screen (kf_lvgl_proof_screen.cpp) never exposed this: its group had
 * exactly one member, so "the focused widget" was never ambiguous even
 * though it was never visibly marked either. Three buttons is the first
 * screen where that actually matters -- there is no way to tell which one
 * A/ENTER is about to activate without one. A style applied directly to
 * LV_STATE_FOCUSED, independent of the theme, is the standard LVGL fix; a
 * static lv_style_t is a widget-tree-lifetime constant, not something that
 * needs to be part of `Screen` or re-initialised per screen instance. */
lv_style_t g_focus_style;

void init_focus_style(void) {
    lv_style_init(&g_focus_style);
    lv_style_set_bg_color(&g_focus_style, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_border_color(&g_focus_style, lv_palette_darken(LV_PALETTE_BLUE, 2));
    lv_style_set_border_width(&g_focus_style, 2);
}

/* kf_pet_millipercent is 0..100000 (see kf/pet.h); LVGL's lv_bar wants an
 * integer range, and tenths of a percent (0..1000) gives a visibly smooth
 * bar without needing float anywhere in this file. */
constexpr int32_t kBarMax = 1000;

int32_t to_bar_value(kf_pet_millipercent mp) {
    return static_cast<int32_t>(mp / 100u);
}

/* One row: a name label above a bar, with a live percentage to its right.
 * `top_y` is this row's own top edge; everything else is placed relative
 * to it so the three calls below are just "the next row down". */
Row make_row(lv_obj_t *screen, const char *name, int16_t top_y) {
    Row row;

    lv_obj_t *name_label = lv_label_create(screen);
    lv_label_set_text(name_label, name);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 12, top_y);

    row.bar = lv_bar_create(screen);
    lv_obj_set_size(row.bar, 150, 12);
    lv_bar_set_range(row.bar, 0, kBarMax);
    lv_obj_align(row.bar, LV_ALIGN_TOP_LEFT, 12, top_y + 18);

    row.value_label = lv_label_create(screen);
    lv_obj_align(row.value_label, LV_ALIGN_TOP_LEFT, 168, top_y + 17);

    return row;
}

void update_row(const Row &row, kf_pet_millipercent mp) {
    kf_lvgl_set_bar(row.bar, to_bar_value(mp));

    /* e.g. "92.3%" -- one decimal digit, matching kf/app.cpp's own
     * append_fixed1() convention for the constraint HUD, just via a
     * printf-style label setter instead of hand-rolled digits: this file is
     * simulator-side presentation code, not Core, so kf/poison.h's ban on
     * libc string formatting does not apply here.
     *
     * Both setters are the idempotent wrappers, which is what keeps an idle
     * screen from redrawing itself forever -- see kf_lvgl_idempotent.h. The
     * text comparison also makes the precision question answer itself:
     * hunger drifting 92.34% -> 92.36% formats to the same six characters,
     * so nothing is written. */
    const uint32_t tenths = mp / 100u;
    kf_lvgl_set_label_fmt(row.value_label, "%u.%u%%",
                           static_cast<unsigned>(tenths / 10u),
                           static_cast<unsigned>(tenths % 10u));
}

/* Placeholder blob styling (ADR 0021, see kf_pet_screen.h's header
 * comment): a real design pass with Chris's designer replaces every one of
 * these tables later, but the SHAPE of the mapping -- size grows with
 * stage, colour picks up the branch once one exists -- is what proves the
 * mechanism is visibly working, not the exact palette choice below.
 *
 * Diameter per life-cycle stage: literally "grows up" as the pet ages,
 * the simplest visual anyone reading the screen understands with zero
 * explanation. Indexed by kf_pet_stage's own numeric order (egg=0 ..
 * adult=4), matching kf/pet.h's header comment that stage order is
 * deliberately life-cycle order. Capped at kBlobDiameterMax (see below) --
 * this screen is only 240 logical pixels wide and the first need row
 * starts at y=32, so the blob's whole footprint (including its caption
 * label) has to stay well clear of that regardless of which stage is
 * showing. */
constexpr int32_t kBlobDiameter[] = {8, 12, 16, 20, 24};
constexpr int32_t kBlobDiameterMax = 24; /* the largest value in the table above */

/* Colour before any branch has been decided: one fixed colour per stage,
 * for egg/baby/child (KF_PET_STAGE_EGG..KF_PET_STAGE_CHILD == indices
 * 0..2). Teen and Adult switch to the branch-indexed tables below instead,
 * since by then there IS a branch to show. */
constexpr lv_palette_t kPreBranchColor[3] = {
    LV_PALETTE_GREY,   /* egg */
    LV_PALETTE_YELLOW, /* baby */
    LV_PALETTE_GREEN,  /* child */
};

/* One colour per teen_form (KF_PET_TEEN_FORM_COUNT == 4: Cut, Hold, Mark,
 * Go, per the character bible's section 6), chosen once at the Child->Teen
 * transition and read from then on -- see kf/pet.h's kf_pet_state comment.
 * Placeholder colours standing in for real art, same status as every other
 * number in this file that isn't final. */
constexpr lv_palette_t kTeenColor[KF_PET_TEEN_FORM_COUNT] = {
    LV_PALETTE_BLUE,   /* Cut */
    LV_PALETTE_PURPLE, /* Hold */
    LV_PALETTE_CYAN,   /* Mark */
    LV_PALETTE_ORANGE, /* Go */
};

/* One colour per (teen_form, adult_branch) pair, sized
 * [KF_PET_TEEN_FORM_COUNT][KF_PET_ADULT_BRANCH_MAX]. Rows are RAGGED because
 * families are: Cut has 2 adults, Hold and Mark have 3, Go has 1 (character
 * bible section 11) -- kf_pet_adults_in_family() is the actual per-family
 * count, and adult_branch is always < that count, so a short family's unused
 * trailing slots here are unreachable by construction, not meaningful data.
 * They're filled with the family's own teen colour (kTeenColor above)
 * rather than left as some other placeholder, so an accidental read past
 * the real count still lands on a colour that visually "belongs" to the
 * right family.
 *
 * Column 0 of every row deliberately matches that family's teen colour --
 * adult_branch 0 keeps the teen's own hue, later columns are variants --
 * same rough-hue-family reasoning as before this table grew a Go row. */
constexpr lv_palette_t kAdultColor[KF_PET_TEEN_FORM_COUNT]
                                   [KF_PET_ADULT_BRANCH_MAX] = {
    /* Cut: 2 real adults, 1 unreachable pad slot. */
    {LV_PALETTE_BLUE, LV_PALETTE_LIGHT_BLUE, LV_PALETTE_BLUE},
    /* Hold: 3 real adults, no padding. */
    {LV_PALETTE_PURPLE, LV_PALETTE_DEEP_PURPLE, LV_PALETTE_INDIGO},
    /* Mark: 3 real adults, no padding. */
    {LV_PALETTE_CYAN, LV_PALETTE_TEAL, LV_PALETTE_LIGHT_BLUE},
    /* Go: 1 real adult, 2 unreachable pad slots. */
    {LV_PALETTE_ORANGE, LV_PALETTE_ORANGE, LV_PALETTE_ORANGE},
};

struct BlobStyle {
    int32_t diameter;
    lv_palette_t palette;
    const char *caption;
};

/* WHAT a stage/branch index means is not this file's business either (see
 * kf/pet.h's header comment) -- "egg", "baby", teen_form N, adult_branch N
 * are the only labels this can honestly show without inventing names that
 * are Chris's to pick later. */
BlobStyle blob_style(const kf_pet_state *state) {
    switch (state->stage) {
    case KF_PET_STAGE_EGG:
        return {kBlobDiameter[0], kPreBranchColor[0], "egg"};
    case KF_PET_STAGE_BABY:
        return {kBlobDiameter[1], kPreBranchColor[1], "baby"};
    case KF_PET_STAGE_CHILD:
        return {kBlobDiameter[2], kPreBranchColor[2], "child"};
    case KF_PET_STAGE_TEEN:
        return {kBlobDiameter[3], kTeenColor[state->teen_form], "teen"};
    case KF_PET_STAGE_ADULT:
    default:
        return {kBlobDiameter[4],
                kAdultColor[state->teen_form][state->adult_branch], "adult"};
    }
}

void update_blob(const kf_pet_state *state) {
    /* Same reasoning as update_row(): everything about the blob is a pure
     * function of these three fields, so if none of them moved, redrawing it
     * would dirty a large area of the panel to produce identical pixels. The
     * blob is the biggest object on the screen, so this is the single
     * largest contributor to a still frame's dirty area. */
    if (static_cast<int>(state->stage) == g.last_stage &&
        static_cast<int>(state->teen_form) == g.last_teen_form &&
        static_cast<int>(state->adult_branch) == g.last_adult_branch) {
        return;
    }
    g.last_stage = static_cast<int>(state->stage);
    g.last_teen_form = static_cast<int>(state->teen_form);
    g.last_adult_branch = static_cast<int>(state->adult_branch);

    const BlobStyle style = blob_style(state);
    lv_obj_set_size(g.blob, style.diameter, style.diameter);
    lv_obj_set_style_bg_color(g.blob, lv_palette_main(style.palette), 0);

    /* teen_form/adult_branch are opaque indices (kf/pet.h), so the caption
     * shows them as plain numbers once they mean anything -- "teen 1",
     * "adult 2-0" -- rather than inventing a name for either. */
    if (state->stage == KF_PET_STAGE_TEEN) {
        lv_label_set_text_fmt(g.blob_label, "%s %u", style.caption,
                               static_cast<unsigned>(state->teen_form));
    } else if (state->stage == KF_PET_STAGE_ADULT) {
        lv_label_set_text_fmt(g.blob_label, "%s %u-%u", style.caption,
                               static_cast<unsigned>(state->teen_form),
                               static_cast<unsigned>(state->adult_branch));
    } else {
        lv_label_set_text(g.blob_label, style.caption);
    }
}

void on_feed(lv_event_t *) { kf_pet_session_feed(); }
void on_play(lv_event_t *) { kf_pet_session_play(); }
void on_rest(lv_event_t *) { kf_pet_session_rest(); }

lv_obj_t *make_button(lv_obj_t *screen, const char *text, int16_t x,
                       lv_event_cb_t cb) {
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 70, 28);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, x, -8);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_style(button, &g_focus_style, LV_STATE_FOCUSED);
    return button;
}

} // namespace

void kf_pet_screen_init(void) {
    init_focus_style();

    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Pet");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Top-right corner, above the rows (which start at y=32) and clear of
     * the title (short text, centered): the biggest blob size
     * (kBlobDiameterMax=24px) plus its caption label still fits entirely
     * within y=[4,~48] there without overlapping anything else on the
     * screen -- see the caption's own comment below for exactly how that
     * is kept true regardless of which stage's diameter is showing.
     * lv_obj_create with a LV_RADIUS_CIRCLE style radius is the standard
     * LVGL way to draw a plain circle with no custom drawing code or image
     * asset -- exactly the "existing LVGL widget primitives, no new art
     * assets" this screen already uses for its bars and buttons. */
    g.blob = lv_obj_create(screen);
    lv_obj_set_style_radius(g.blob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g.blob, 0, 0);
    lv_obj_align(g.blob, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_remove_flag(g.blob, LV_OBJ_FLAG_SCROLLABLE);

    /* LV_ALIGN_TOP_RIGHT keeps the blob's TOP-right corner fixed as its
     * size changes per stage (update_blob() below calls lv_obj_set_size()
     * every update) -- only its bottom and left edges move. That makes the
     * worst case for the caption below it fully known at compile time from
     * kBlobDiameterMax alone, so the caption gets a FIXED position
     * computed once here, deliberately NOT lv_obj_align_to(..., blob, ...):
     * that was tried first and anchored to the blob's default ~100px LVGL
     * object size (this function runs before update_blob()'s first resize
     * has happened), landing the caption far below the blob instead of
     * under it -- confirmed via an actual Xvfb screenshot while building
     * this, not assumed. A fixed offset sidesteps needing the label's
     * position to track the blob's current size at all. */
    g.blob_label = lv_label_create(screen);
    lv_obj_align(g.blob_label, LV_ALIGN_TOP_RIGHT, -8, 4 + kBlobDiameterMax + 2);

    g.hunger = make_row(screen, "Hunger", 32);
    g.happiness = make_row(screen, "Happy", 84);
    g.energy = make_row(screen, "Energy", 136);

    make_button(screen, "Feed", 8, on_feed);
    make_button(screen, "Play", 85, on_play);
    make_button(screen, "Rest", 162, on_rest);

    /* No explicit lv_group_add_obj() here, deliberately -- an earlier
     * version of this file called it for all three buttons, matching the
     * old proof screen's pattern, and it was wrong. lv_button's own
     * lv_obj_class sets group_def = LV_OBJ_CLASS_GROUP_DEF_TRUE
     * (lv_button.c), which lv_obj_class_init() (lv_obj_class.c) reads to
     * auto-add every new button to whatever group is CURRENTLY DEFAULT the
     * moment it is created -- already true here, since kf_lvgl_input_init()
     * calls lv_group_set_default() before this function ever runs. Calling
     * lv_group_add_obj() again on an object already in that group does not
     * no-op: it internally calls lv_group_remove_obj() first (see
     * lv_group.c's own "be sure the object is removed from its current
     * group" comment), and removing the CURRENTLY FOCUSED object forces an
     * immediate refocus onto some other member before the object is
     * re-inserted at the list's tail. Three redundant add calls in a row
     * each independently perturbed focus and list order, and which button
     * ended up focused after all three was not "Feed" (the first one
     * created, the naive expectation) but whichever member the group's
     * internal remove/refocus/reinsert churn happened to land on --
     * confirmed directly via lv_obj_get_state() while diagnosing, not
     * guessed at. The buttons are already reachable the moment they're
     * created; nothing else needs to run. */
    if (lv_group_get_default() == nullptr) {
        KF_LOGW(TAG, "no default LVGL group -- the pet screen's buttons "
                     "will not be reachable with the keypad");
    }

    g.ready = true;
    kf_pet_screen_update();
}

void kf_pet_screen_update(void) {
    KF_ASSERT(g.ready,
              "kf_pet_screen_update called before kf_pet_screen_init");
    const kf_pet_state *state = kf_pet_session_state();
    update_row(g.hunger, state->hunger_mp);
    update_row(g.happiness, state->happiness_mp);
    update_row(g.energy, state->energy_mp);
    update_blob(state);
}
