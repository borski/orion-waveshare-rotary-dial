/*
 * SCR_ADJUST_MODE — "Schedule vs. Hold" choice screen. Replaces the old
 * single settings row that cycled the same pref on tap ("Next step" /
 * "Morning"): a preference whose consequence lands hours later, mid-sleep,
 * can't be explained by a value label alone, so this is a full CHOICE
 * screen with prose under the two options instead of a plain toggle.
 *
 * Three entry points, one packed `arg` (a uintptr_t, scr_brightness.c's own
 * packed-arg idiom): 0 = scr_settings.c's "Adjustment mode" row; 1 + zone
 * (so 1 or 2) = scr_dial.c's power-disc long-press or status-pill tap, for
 * that zone's dial face. s_origin below stores it as-is (not unpacked into
 * a bool + zone) so go_back() has one value to switch on. Back (the pill)
 * and swipe-right both funnel through go_back(), which returns to
 * SCR_SETTINGS for arg 0 or SCR_DIAL (that zone) for arg 1+zone — owner
 * feedback on the first cut: hardcoding SCR_SETTINGS sent a dial-face visit
 * to the wrong place entirely.
 *
 * Hand-laid, not dial_list (owner's call is explicit either way is fine):
 * with only two options this isn't a scrollable list, and the description
 * block below the options has to stay put and re-render in place as the
 * selection changes — a rotor's per-row scroll/zoom/fade machinery has
 * nothing to offer a two-item choice and would fight a fixed description
 * anchored beneath it. The two options sit side by side instead of stacked
 * so there's still headroom left for that description AND a Back pill
 * inside the round panel's safe band (see the y-offsets below).
 *
 * Knob semantics: "Schedule" is index 0 (left), "Hold" is index 1 (right).
 * A detent moves the CURRENT selection by one slot, clamped at both ends
 * (range-stop haptic + nudge past the last option) — dial_list's own "walk
 * one row per detent" vocabulary, just collapsed onto two slots instead of
 * a scrollable N. There's no separate "focused but not yet chosen" state
 * to track here: every reachable slot is already a complete, meaningful
 * choice (unlike a numeric picker being dialed toward a target), so a knob
 * turn commits exactly like a tap does — same persist, same HAPTIC_CONFIRM,
 * same description update.
 *
 * Keeps the underlying pref, NVS key, and write-path logic exactly as they
 * were (app_state_t.sched_follow / dial_state_set_sched_follow / main.c's
 * temp_write_phase()/sleep_phase_now()) — this file is a presentation
 * change only.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

#define CX 180
#define CY 180
#define ARC_R 165

static lv_obj_t *s_ring;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_pill_schedule, *s_pill_schedule_lbl;
static lv_obj_t *s_pill_hold, *s_pill_hold_lbl;
static lv_obj_t *s_desc_lbl;
static lv_obj_t *s_back, *s_back_lbl;

// Packed entry-point arg, verbatim (see the header comment for the
// encoding) — 0 = Settings, 1+zone = the dial face for that zone. Stored
// as-is, not unpacked into separate fields, since every reader (go_back())
// just switches on the raw value once.
static uintptr_t s_origin;

/* ---- motion helper (scr_brightness.c's range-stop nudge, ported
 * verbatim) --------------------------------------------------------------*/

static void set_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_nudge(lv_obj_t *obj, int dir)
{
    lv_anim_del(obj, set_x_cb);
    lv_obj_set_x(obj, 4 * dir);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_x_cb);
    lv_anim_set_values(&a, 4 * dir, 0);
    lv_anim_set_time(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

/* ---- selection ------------------------------------------------------------*/

// The single choke point every input path (tap on either pill, knob detent)
// funnels through: no-ops on a redundant re-select of the already-current
// mode (no haptic, no NVS write) so tapping/turning onto what's already
// chosen doesn't buzz or wear the flash for nothing. Visual refresh happens
// on the next on_state (the generation bump dial_state_set_sched_follow
// makes below reaches the dispatcher within one ~50ms tick — same latency
// every other settings toggle in this UI already accepts).
static void select_mode(bool schedule)
{
    app_state_t st;
    dial_state_get(&st);
    if (st.sched_follow == schedule) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    dial_state_set_sched_follow(schedule);
}

static void schedule_tap_cb(lv_event_t *e) { (void)e; select_mode(true); }
static void hold_tap_cb(lv_event_t *e)     { (void)e; select_mode(false); }

// The one place that decodes s_origin (see the header comment) — both Back
// and swipe-right funnel through this instead of each hardcoding a
// destination, so the two exit paths can never disagree about where "back"
// means. arg 0 -> Settings, arrived at by a lateral menu swipe, so leaves
// the same way (MOVE_RIGHT); arg 1+zone -> the dial face for that zone,
// arrived at by a modal-style long-press/tap (LV_SCR_LOAD_ANIM_NONE both
// ways, matching scr_dial.c's own power_long_press_cb/pill_event_cb).
// Deliberately no haptic here — callers add their own (or don't), same
// split the destination itself used to have (a tapped Back pill confirms,
// a swipe doesn't, matching every other swipe-back in this UI).
static void go_back(void)
{
    if (s_origin == 0) {
        ui_router_go(SCR_SETTINGS, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    } else {
        zone_idx_t zone = (zone_idx_t)(s_origin - 1);
        ui_router_go(SCR_DIAL, (void *)(uintptr_t)zone, LV_SCR_LOAD_ANIM_NONE);
    }
}

static void back_event_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    go_back();
}

/* ---- palette + render ------------------------------------------------------*/

// One option pill's DEFAULT-state look: selected = filled (surface bg) +
// ink_primary border/text (the "accent" + "filled pill" cues from the same
// token vocabulary every other screen's selection/state uses); not selected
// = flat + track border + ink_secondary text. PRESSED-state feedback (a
// surface flash on tap) is set once in create() and layers under this.
static void style_pill(lv_obj_t *pill, lv_obj_t *lbl, const dial_palette_t *pal, bool selected)
{
    if (selected) {
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pill, pal->surface, 0);
        lv_obj_set_style_border_color(pill, pal->ink_primary, 0);
        lv_obj_set_style_text_color(lbl, pal->ink_primary, 0);
    } else {
        lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(pill, pal->track, 0);
        lv_obj_set_style_text_color(lbl, pal->ink_secondary, 0);
    }
}

static void render(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_ring);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_back, pal->surface, 0);
    lv_obj_set_style_border_color(s_back, pal->track, 0);
    lv_obj_set_style_text_color(s_back_lbl, pal->ink_secondary, 0);

    bool schedule = st->sched_follow;
    style_pill(s_pill_schedule, s_pill_schedule_lbl, pal, schedule);
    style_pill(s_pill_hold,     s_pill_hold_lbl,     pal, !schedule);

    // The entire point of this screen: prose that explains what the CURRENT
    // choice actually does hours from now, not just names it.
    lv_label_set_text(s_desc_lbl, schedule
        ? "Your change follows tonight's schedule. The next scheduled temperature still happens."
        : "Your change stays until morning. The rest of tonight's schedule is ignored.");
    lv_obj_set_style_text_color(s_desc_lbl, pal->ink_secondary, 0);
}

/* ---- vtable ----------------------------------------------------------------*/

static lv_obj_t *make_pill(lv_obj_t *scr, const char *txt, lv_coord_t x, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *pill = dial_btn_create(scr);
    lv_obj_set_size(pill, 145, 72);   // >=72px touch target (this project's round-screen DLS)
    lv_obj_set_style_radius(pill, 16, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    // Pressed-state flash is selector-based (fixed at create time) and
    // layers under whatever style_pill's DEFAULT-state selected/not-selected
    // look currently is — same split scr_menu.c/scr_settings.c's rows use.
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_align(pill, LV_ALIGN_CENTER, x, 150 - CY);
    lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    if (lbl_out) *lbl_out = lbl;
    return pill;
}

static void create(lv_obj_t *scr, void *arg)
{
    s_origin = (uintptr_t)arg;   // 0 = Settings, 1+zone = the dial face (see header comment)
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Chassis hairline ring — same geometry as every other hand-laid face
    // (scr_menu.c/scr_standby.c): "a chassis that persists".
    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 135);
    lv_arc_set_bg_angles(s_ring, 0, 270);
    lv_obj_set_style_arc_width(s_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "ADJUSTMENT MODE");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    // Two options, side by side (not stacked) — see the header comment for
    // why: it leaves the vertical room a stacked pair would eat into the
    // description block below needs. -80/+80 keeps a visible gap between
    // the two 145-wide pills while staying inside the chord at y=150.
    s_pill_schedule = make_pill(scr, "Schedule", -80, schedule_tap_cb, &s_pill_schedule_lbl);
    s_pill_hold     = make_pill(scr, "Hold",      80, hold_tap_cb,     &s_pill_hold_lbl);

    // Description — fixed anchor, width capped so wrapped lines never reach
    // the bezel at this y-band; height is content-sized (LV_SIZE_CONTENT is
    // the label default) and centered on this slot, so the shorter Hold
    // text doesn't inherit Schedule's line count worth of empty space.
    s_desc_lbl = lv_label_create(scr);
    lv_obj_set_width(s_desc_lbl, 280);
    lv_label_set_long_mode(s_desc_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_desc_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_desc_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_desc_lbl, LV_ALIGN_CENTER, 0, 235 - CY);

    // Back pill: same 140x72 slot and pill look this project's other
    // hand-laid sub-screens use — the gesture alone wasn't discoverable
    // enough on its own (owner feedback on an earlier screen), hence a
    // visible affordance here too.
    s_back = dial_btn_create(scr);
    lv_obj_set_size(s_back, 140, 72);
    lv_obj_set_style_radius(s_back, 36, 0);
    lv_obj_set_style_border_width(s_back, 1, 0);
    lv_obj_align(s_back, LV_ALIGN_CENTER, 0, 310 - CY);
    lv_obj_add_event_cb(s_back, back_event_cb, LV_EVENT_CLICKED, NULL);

    s_back_lbl = lv_label_create(s_back);
    lv_obj_set_style_text_font(s_back_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_back_lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(s_back_lbl);
}

static void destroy(void)
{
    if (s_pill_schedule) lv_anim_del(s_pill_schedule, set_x_cb);
    if (s_pill_hold)     lv_anim_del(s_pill_hold, set_x_cb);

    s_ring = NULL;
    s_title_lbl = NULL;
    s_pill_schedule = s_pill_schedule_lbl = NULL;
    s_pill_hold = s_pill_hold_lbl = NULL;
    s_desc_lbl = NULL;
    s_back = s_back_lbl = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    render(st);
}

// Binary rotor: a detent moves the current selection one slot toward
// Schedule (CCW) or Hold (CW), clamped at both ends. Every reachable slot
// is a complete, meaningful choice, so the knob commits directly — no
// separate "focused but not chosen" cursor to track (see header comment).
static bool on_knob(int detents)
{
    if (!s_pill_schedule || detents == 0) return false;

    app_state_t st;
    dial_state_get(&st);
    int idx = st.sched_follow ? 0 : 1;         // 0 = Schedule, 1 = Hold
    int nidx = idx + (detents > 0 ? 1 : -1);
    if (nidx < 0 || nidx > 1) {
        dial_haptics_play_soft(HAPTIC_STOP);
        anim_nudge(nidx < 0 ? s_pill_schedule : s_pill_hold, detents > 0 ? 1 : -1);
        return true;
    }

    select_mode(nidx == 0);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    go_back();
    return true;
}

const ui_screen_t scr_adjust_mode = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
