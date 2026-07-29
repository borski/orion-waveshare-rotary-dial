/*
 * SCR_BRIGHTNESS — full-screen day/night/night-clock backlight percent
 * picker, reached from SCR_BRIGHTNESS_MENU's Day/Night/Night clock rows.
 * Replaces the old tap-to-edit-in-place on those rows (owner field
 * feedback: it was an unintuitive, one-off micro-pattern) — every other
 * "adjust a value with the knob" control in this UI is a full-screen face
 * with a big number (the temperature dial, scr_boost's duration picker), so
 * brightness now matches that vocabulary: same caption styling, same big
 * numeral font, same display-only rim arc, same detent/zoom-bump/range-stop
 * feel. `arg` packs which row opened it: 0 = day, 1 = night, 2 = night
 * clock (the standby/screensaver face at night — see dial_state.h's
 * bri_night_clock_pct comment).
 *
 * The night-clock row previews and commits the NIGHT table's STANDBY duty
 * (dial_power's DPWR_STANDBY), not ACTIVE like the other two rows — see
 * preview_current() below and dial_power_preview's header comment. That
 * duty is very dim by design (it's the whole point of the setting), so
 * don't be alarmed that its live preview barely lights the panel.
 *
 * The arc is drag-adjustable via a discrete handle, not the ring itself —
 * exactly the pattern scr_dial.c's setpoint handle established (see that
 * file's handle_event_cb comment for the full rationale): a separate small
 * object rides on the fill's leading edge, owns its own press/pressing/
 * release events, and does NOT bubble them, so a gesture starting on the
 * handle adjusts brightness and never becomes the tap-anywhere-exit or a
 * swipe. The arc itself stays CLICKABLE-cleared and display-only.
 *
 * There is deliberately NO timeout and NO second-tap-to-confirm — leaving
 * the screen IS the commit, from every exit path (tap-anywhere, swipe-down
 * back, or any future caller of ui_router_go while this is current). See
 * destroy()'s comment for the single choke point that guarantees this.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_power.h"
#include <math.h>

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180
#define CY 180
#define ARC_R 165

// All three rows (Day, Night, Night clock) move in 1% steps from both the
// knob and the drag handle — the owner asked for uniform granularity across
// all of them. Only the FLOOR differs, and that's a safety rail, not an
// inconsistency: the night clock may legitimately go to 0 (a dark bedroom;
// a touch or a knob detent still wakes the dial to the night ACTIVE duty to
// see by), but Day and Night govern the screen you're looking at WHILE
// using the dial — at 0 those would render the very picker you'd need to
// undo it invisible. So Day/Night keep a 10% floor; the clock runs 0-100.
#define BRI_MIN_PCT   0    // all three rows: 0 is dimmest-legible for Day/Night, off for the clock
#define BRI_MAX_PCT  100
#define BRI_STEP_PCT   1

// Knob acceleration (owner: "single digit accuracy... [but] super slow to
// move 100 clicks"). ui_router.c's dispatch_tick accumulates detents for
// ~50ms and hands the whole batch to one on_knob() call, so |detents| is
// already a usable speed proxy: a careful, deliberate click arrives alone
// (batch 1) and a brisk spin arrives as several. Only the per-detent
// multiplier is accelerated by batch size — floor/ceiling clamping and the
// range-stop haptic in on_knob() are untouched by it, so a single detent
// still moves exactly BRI_STEP_PCT (1%) and a fast spin crosses the whole
// range in roughly a second. Tune the curve here without touching on_knob().
#define BRI_ACCEL_1_MULT  BRI_STEP_PCT   // |batch| == 1 -> 1%/detent  (exact single-step control)
#define BRI_ACCEL_2_MULT  3              // |batch| == 2 -> 3%/detent
#define BRI_ACCEL_3_MULT  6              // |batch| >= 3 -> 6%/detent (full range in ~1s of brisk spinning)

static lv_obj_t *s_arc;
static lv_obj_t *s_handle;   // percent drag handle — the sole touch target (see file header)
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_num_box, *s_num_lbl;
static lv_obj_t *s_unit_lbl;

static bool s_night;   // true for BOTH night rows (Night and Night clock)
static bool s_clock;   // true only for the Night clock row (arg 2)
static int  s_pct;

// This visit's value range: 0-100 for the night clock, BRI_MIN_PCT-100
// otherwise. Fixed for the screen's whole lifetime (set once in create()),
// unlike scr_dial's arc range which can change mid-visit on a mode toggle —
// shared by position_handle()/value_from_point() so the drag math and the
// arc/knob clamps all agree on the same rails.
static int s_arc_min, s_arc_max;

// Drag state for the handle, mirroring scr_dial's s_dragging: true between
// PRESSED and RELEASED/PRESS_LOST so on_state's palette re-apply never
// fights an in-progress drag. s_limit_latched is a one-shot per drag so
// HAPTIC_STOP fires once on arriving/holding at a range end rather than on
// every ~continuous PRESSING callback.
static bool s_dragging;
static bool s_limit_latched;

// Which tier this row previews/commits: the night-clock row edits ONLY the
// standby (screensaver) duty; the other two rows edit ACTIVE, same as
// before this row existed. Shared by create()/on_knob() so the live preview
// always matches what destroy() is about to persist.
static void preview_current(void)
{
    dial_power_preview(s_night, s_clock ? DPWR_STANDBY : DPWR_ACTIVE, (uint8_t)s_pct);
}

/* ---- handle geometry (scr_dial.c's setpoint-handle math, ported) -------- */
// The arc sweeps 270° starting at 135° (lower-left), clockwise over the top
// to 45° (lower-right); the remaining 90° at the bottom is the gap. Both
// helpers share that mapping so the handle rides exactly on the fill's
// leading edge, same as scr_dial's ARC_R/CX/CY ring.

// Place the handle centered ON the arc band, mirroring LVGL's own knob
// centerline math (band centerline = (inner span)/2 - indic_width/2), read
// from the live object so it stays correct regardless of the arc's padding.
static void position_handle(int pct)
{
    if (!s_handle || s_arc_max <= s_arc_min) return;
    lv_coord_t span = LV_MIN(lv_obj_get_width(s_arc), lv_obj_get_height(s_arc))
                      - lv_obj_get_style_pad_left(s_arc, LV_PART_MAIN)
                      - lv_obj_get_style_pad_right(s_arc, LV_PART_MAIN);
    float r = (span > 0 ? span : 2 * ARC_R) / 2.0f
              - lv_obj_get_style_arc_width(s_arc, LV_PART_INDICATOR) / 2.0f;
    float frac = (float)(pct - s_arc_min) / (float)(s_arc_max - s_arc_min);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    float ang = (135.0f + frac * 270.0f) * (float)M_PI / 180.0f;
    int dx = (int)lroundf(r * cosf(ang));
    int dy = (int)lroundf(r * sinf(ang));
    lv_obj_align(s_handle, LV_ALIGN_CENTER, dx, dy);
}

// Map a live touch point (screen coords) back to a percent along the sweep.
// Angles in the bottom gap fold to whichever rail is nearer, so a finger
// that slides off the bottom pins cleanly instead of jumping across the
// gap. lroundf already lands on a whole percent, which is this screen's
// full granularity (BRI_STEP_PCT == 1) — no separate quantize step needed.
static int value_from_point(lv_point_t p)
{
    float theta = atan2f((float)(p.y - CY), (float)(p.x - CX)) * 180.0f / (float)M_PI;
    if (theta < 0.0f) theta += 360.0f;              // 0..360, 0 at 3 o'clock, +cw
    float sweep = theta - 135.0f;                   // 0 at the arc's start
    if (sweep < 0.0f) sweep += 360.0f;
    if (sweep > 270.0f) sweep = (sweep > 315.0f) ? 0.0f : 270.0f;   // gap -> nearer rail
    float frac = sweep / 270.0f;
    return s_arc_min + (int)lroundf(frac * (float)(s_arc_max - s_arc_min));
}

/* ---- motion helpers (scr_boost.c's §6 vocabulary, verbatim) -------------*/

static void set_zoom_cb(void *obj, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (int16_t)v, 0); }
static void set_x_cb(void *obj, int32_t v)    { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_zoom_bump(lv_obj_t *obj)
{
    lv_anim_del(obj, set_zoom_cb);
    lv_obj_set_style_transform_zoom(obj, 256, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_zoom_cb);
    lv_anim_set_values(&a, 256, 266);
    lv_anim_set_time(&a, 45);
    lv_anim_set_playback_time(&a, 45);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

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

static void render_numeral(int pct)
{
    char t[8];
    snprintf(t, sizeof(t), "%d", pct);
    lv_label_set_text(s_num_lbl, t);
}

/* ---- palette -------------------------------------------------------------*/
// Re-applied from on_state (not just create()) so a night palette swap while
// the picker is open recolors it — screens never cache PAL() past a render.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    // Brightness has no heat/cool meaning of its own, so the live value uses
    // ink_primary as its accent — the exact token the inline row edit this
    // screen replaces used for the same purpose (scr_settings.c's old
    // apply_palette comment: "without reaching for a thermal/warning/
    // identity token that means something else everywhere else").
    lv_color_t accent = pal->ink_primary;

    lv_obj_t *scr = lv_obj_get_parent(s_arc);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    lv_obj_set_style_arc_color(s_arc, pal->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_num_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_unit_lbl, pal->ink_secondary, 0);

    // Handle: themed to the same accent as the arc fill, parked at the
    // current percent. Left alone while dragging — the finger owns s_pct
    // then, and a reposition here would fight it (mirrors scr_dial).
    lv_obj_set_style_bg_color(s_handle, accent, 0);
    if (!s_dragging) position_handle(s_pct);
}

/* ---- events ----------------------------------------------------------------*/

// Tap anywhere on the face exits — the arc and numeral box below both clear
// CLICKABLE so the tap reaches this handler on `scr` itself regardless of
// where on the dial it lands (same idiom scr_standby.c's tap-anywhere-wake
// uses). The handle does NOT bubble its own events (see its creation
// comment), so a tap that lands on it never reaches here — it adjusts
// instead of exiting. Commit happens in destroy(), not here — see its
// comment.
static void tap_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
}

// Percent handle drag — ported from scr_dial.c's handle_event_cb (see its
// comment for the full swipe-vs-drag rationale). PRESSING reads the live
// touch point and maps it straight to a percent (already whole-number
// granular, see value_from_point); RELEASED/PRESS_LOST just end the drag —
// this screen's only commit point is destroy(), so nothing is posted here.
static void handle_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        s_limit_latched = false;
        return;
    }
    if (!s_dragging) return;   // a press that didn't start a drag

    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int q = value_from_point(p);
        if (q != s_pct) {
            // Live feedback: fill, numeral, and handle all follow the
            // finger, same as the knob path — that live preview is the
            // whole point of this screen.
            s_pct = q;
            lv_arc_set_value(s_arc, q);
            render_numeral(q);
            position_handle(q);
            preview_current();
            dial_haptics_play(HAPTIC_TICK);   // fires on value change, not every PRESSING
            s_limit_latched = false;
        } else if ((q == s_arc_min || q == s_arc_max) && !s_limit_latched) {
            // Arrived at / still held against a range end — one stop cue,
            // not a buzz on every ~continuous PRESSING callback.
            dial_haptics_play_soft(HAPTIC_STOP);
            s_limit_latched = true;
        }
        return;
    }

    // LV_EVENT_RELEASED / LV_EVENT_PRESS_LOST — end of the drag. No commit
    // here; destroy() is the single choke point (see its comment).
    s_dragging = false;
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    uintptr_t packed = (uintptr_t)arg;   // 0 = day, 1 = night, 2 = night clock
    s_night = packed != 0;
    s_clock = packed == 2;
    s_pct = s_clock ? dial_state_get_bri_night_clock_pct()
           : s_night ? dial_state_get_bri_night_pct()
                     : dial_state_get_bri_day_pct();
    s_arc_min = BRI_MIN_PCT;
    s_arc_max = BRI_MAX_PCT;
    s_dragging = false;
    s_limit_latched = false;

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);

    // Chassis ring — same geometry as scr_boost's, display-only (no drag):
    // CLICKABLE stays cleared so both a tap (exit) and a swipe-down
    // (on_gesture, also exit) pass through to `scr` instead of this ring.
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, BRI_MIN_PCT, BRI_MAX_PCT);
    lv_arc_set_value(s_arc, s_pct);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    // Kill the default theme's knob dot — a knob is a drag handle, and this
    // arc is display-only (percent rides the encoder, per on_knob). Same
    // suppression every other non-draggable ring in this UI applies.
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    // Percent handle — the sole touch target for brightness, ported from
    // scr_dial.c's setpoint handle (see that file's create() and the header
    // comment above for the full rationale). A thumb on the ring at the
    // current percent, created after the arc so it draws on top: press and
    // drag it around the ring to set brightness. ext_click_area gives a
    // ~56px effective grab without a bulky visual. Deliberately NOT
    // EVENT_BUBBLE-ing (the lv_obj default), so a press or gesture that
    // starts on it stays here instead of reaching `scr`'s tap-to-exit or
    // becoming a swipe. Positioned/colored per render in apply_palette().
    s_handle = lv_obj_create(scr);
    lv_obj_set_size(s_handle, 28, 28);
    lv_obj_set_style_radius(s_handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_handle, 3, 0);
    lv_obj_set_style_border_color(s_handle, pal->bg, 0);   // bg ring -> reads as a thumb ON the track
    lv_obj_clear_flag(s_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_handle, 14);   // ~56px effective touch target
    lv_obj_add_event_cb(s_handle, handle_event_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_handle, handle_event_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_handle, handle_event_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_handle, handle_event_cb, LV_EVENT_PRESS_LOST, NULL);

    // Caption.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, s_clock ? "NIGHT CLOCK"
                                  : s_night ? "NIGHT BRIGHTNESS"
                                            : "DAY BRIGHTNESS");
    // 84, not scr_boost's 64: this caption is nearly twice as wide as
    // "BOOST HEAT", and the arc's inner edge (r=149) leaves only ~159px of
    // chord at y=54 (a 16px-font line's top edge there) — "NIGHT BRIGHTNESS"
    // measures ~155px, so its corners nearly touched the stroke. 20px lower
    // the chord opens to ~209px, restoring a comfortable margin on both
    // sides without crowding the numeral below. "NIGHT CLOCK" is shorter
    // still (~115px), so the same offset clears the stroke with room to
    // spare — no per-row offset needed.
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 84 - CY);

    // Percent numeral — fixed anchor box, same slot as scr_boost's duration.
    s_num_box = lv_obj_create(scr);
    lv_obj_set_size(s_num_box, 210, 92);
    lv_obj_set_style_bg_opa(s_num_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_num_box, 0, 0);
    lv_obj_clear_flag(s_num_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_num_box, LV_ALIGN_CENTER, 0, 150 - CY);

    s_num_lbl = lv_label_create(s_num_box);
    lv_obj_set_style_text_font(s_num_lbl, &dial_font_num_88, 0);
    lv_obj_set_style_transform_pivot_x(s_num_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_num_lbl, LV_PCT(50), 0);
    lv_obj_center(s_num_lbl);
    render_numeral(s_pct);

    // Unit, below the numeral.
    s_unit_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_unit_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_unit_lbl, "%");
    lv_obj_align(s_unit_lbl, LV_ALIGN_CENTER, 0, 214 - CY);

    apply_palette();

    // Live preview from the moment the screen opens, not just on the first
    // knob turn — the backlight should already be showing the row's current
    // value the instant this face is on screen (task: "on entry AND on
    // every change... that's the point").
    preview_current();
}

// The ONE commit path for every exit: tap_cb, on_gesture, or any other
// future caller of ui_router_go while this screen is current all funnel
// through here, unconditionally — ui_router.c:82 calls the OUTGOING
// screen's destroy() before the incoming screen's create() runs, so this
// fires exactly once per visit no matter which exit fired. That's why there
// is no separate "confirm" step anywhere above: leaving IS committing.
static void destroy(void)
{
    if (s_num_lbl) lv_anim_del(s_num_lbl, NULL);
    if (s_num_box) lv_anim_del(s_num_box, NULL);
    if (s_arc)     lv_anim_del(s_arc, NULL);

    dial_power_preview_end();
    if (s_clock)      dial_state_set_bri_night_clock_pct((uint8_t)s_pct);
    else if (s_night) dial_state_set_bri_night_pct((uint8_t)s_pct);
    else              dial_state_set_bri_day_pct((uint8_t)s_pct);
    dial_power_brightness_changed();

    s_arc = s_handle = s_title_lbl = s_num_box = s_num_lbl = s_unit_lbl = NULL;
    s_dragging = false;
    s_limit_latched = false;
}

static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_arc) return;
    apply_palette();
}

static bool on_knob(int detents)
{
    if (!s_arc) return false;
    // Accelerated by batch size (see the BRI_ACCEL_* comment up top) — only
    // the multiplier changes; clamping and the range-stop haptic below are
    // exactly what they were pre-acceleration.
    int mag  = (detents < 0) ? -detents : detents;
    int mult = (mag <= 1) ? BRI_ACCEL_1_MULT : (mag == 2) ? BRI_ACCEL_2_MULT : BRI_ACCEL_3_MULT;
    int np = s_pct + detents * mult;
    if (np < s_arc_min) np = s_arc_min;
    if (np > s_arc_max) np = s_arc_max;
    if (np == s_pct) {                                // at the range stop
        dial_haptics_play_soft(HAPTIC_STOP);
        int dir = detents > 0 ? 1 : -1;
        anim_nudge(s_num_box, dir);
        anim_nudge(s_arc, dir);
        return true;
    }

    s_pct = np;
    lv_arc_set_value(s_arc, np);
    render_numeral(np);
    position_handle(np);
    anim_zoom_bump(s_num_lbl);
    preview_current();   // live: the backlight follows the knob
    dial_haptics_play(HAPTIC_TICK);   // one pulse per on_knob() call, never per synthesized percent
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    // Never navigate while the handle is being dragged. It doesn't bubble
    // gestures so this shouldn't fire mid-drag, but a swipe that grazes it
    // must not both move brightness and exit the screen (mirrors scr_dial).
    if (s_dragging) return false;
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    return true;
}

const ui_screen_t scr_brightness = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
