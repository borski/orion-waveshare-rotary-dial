/*
 * SCR_UPDATE_PROMPT — the dismissible "update available" sheet
 * (docs/SPEC-update-prompt.md). Routed by main.c's nav_policy, not reached by
 * a deliberate swipe/tap the way every other sub-screen is: the worker raises
 * app_state_t.ota_prompt_due on a WAKE EDGE (DPWR_STANDBY/DIMMED ->
 * DPWR_ACTIVE) when every entry gate holds at that instant (status
 * AVAILABLE, not skipped, not deferred, not shown in the last 24h, outside
 * the sleep window, PH_READY, a real clock, and Auto-update Off — no idle
 * window; a wake edge IS the "a human is provably present" signal), and
 * nav_policy shows this screen exactly then — see nav_policy's own comment
 * for why it's scoped to SCR_DIAL specifically (never yanks the user out of
 * Settings/Menu/etc). Once raised the worker does NOT keep re-evaluating
 * that gate list — see main.c's "Update prompt: exit" comment for the three
 * separate conditions (display back to STANDBY / night / update no longer
 * available) that withdraw it instead; a touch is deliberately not one of
 * them, which is the whole point of the rework (the old continuously
 * re-evaluated idle-window gate withdrew the sheet the instant a touch
 * reset idle time, i.e. mid-reach).
 *
 * Input guard: the wake-consumes rule only swallows the FIRST touch of the
 * wake that raised this sheet (main.c's touch_filter/knob_step, upstream of
 * this screen entirely) — a second, reflex touch from someone who actually
 * meant to adjust temperature can land here within a couple hundred ms.
 * s_created_at_tick (stamped in create()) + OTA_PROMPT_GUARD_MS makes every
 * button handler below a no-op for the first 400ms this screen exists, so
 * that reflex touch can't land on "Update now".
 *
 * Visually this is this project's bottom-sheet idiom (slides up over the
 * dial face, dismissed only by its own buttons (a down swipe used to do it; removed after it fired while users tried to scroll the options)) — but sized to the full screen
 * rather than a partial sheet: the spec's copy (heading + version + a
 * two-line install-cost paragraph + two buttons meeting explicit >=88/>=72px
 * minimums + a third text row) does not fit a partial sheet's safe band on a
 * 360px round panel without risking the round bezel clipping the lower
 * rows (the same round-safe-zone budget scr_adjust_mode.c/scr_updating.c
 * already spend end to end). The "sheet, not a hard modal" requirement the
 * spec cares about is about DISMISSAL COST (one tap/swipe, never a multi-step
 * flow blocking the temperature control), not literal screen coverage — this
 * screen still slides in/out with that same sheet motion and every exit is
 * a single action, so that requirement holds either way.
 *
 * Every exit path (Update now / Later / Update options / the Later button) clears
 * ota_prompt_due itself, synchronously, before or alongside navigating away
 * — see dial_state_clear_ota_prompt_due()'s comment for why this can't be
 * left to the worker's own next idle tick (a race that would otherwise let
 * nav_policy re-show this screen for up to ~300ms after the user already
 * left it).
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_ota.h"
#include <time.h>

#define SCREEN_W       360
#define SHEET_H        360   // full screen — see header comment for why
#define SHEET_Y_OPEN     0
#define SHEET_Y_CLOSED 360

// "Later" walks the prompt 23h around the clock (not 24h) so a bad time of
// day doesn't pin it to the same moment forever — see the spec.
#define OTA_DEFER_SECONDS (23 * 3600)

static lv_obj_t *s_sheet;
static lv_obj_t *s_grab;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_version_lbl;
static lv_obj_t *s_body_lbl;
static lv_obj_t *s_btn_now, *s_btn_now_lbl;
static lv_obj_t *s_btn_later, *s_btn_later_lbl;
static lv_obj_t *s_row_options, *s_row_options_lbl;

static zone_idx_t s_zone = ZONE_A;   // dial face to return to on dismiss
static bool       s_apply_sent;      // latches "Update now" so a stray re-render doesn't reset the label

// Input guard (docs/SPEC-update-prompt.md's wake-edge rework): this sheet
// now only ever appears immediately after a wake, and the wake-consumes
// rule only swallows the FIRST touch of that wake end-to-end — main.c's
// touch_filter/knob_step, not this screen. Someone who woke the dial meaning
// to adjust temperature (not read a dialog) will often touch again right
// away, and without a guard that second touch can land on "Update now"
// before they've even registered what's on screen. lv_tick_get() stamped in
// create(); each button handler below returns early while
// lv_tick_elaps(s_created_at_tick) is still under this. Deliberately NOT
// applied to on_gesture's swipe-dismiss — a directional drag isn't the
// "reflex second tap" shape this guards against, and swallowing a genuine
// swipe-to-dismiss buys nothing.
#define OTA_PROMPT_GUARD_MS 400
static uint32_t s_created_at_tick;

/* ---- sheet slide anim (this project's bottom-sheet timings) ------------*/

static void set_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_sheet(int32_t from, int32_t to, uint32_t time_ms,
                        lv_anim_path_cb_t path, lv_anim_ready_cb_t ready_cb)
{
    lv_anim_del(s_sheet, set_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, set_y_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, path);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static void close_ready_cb(lv_anim_t *a)
{
    (void)a;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
}

// Every dismiss path funnels through here: clear the live flag FIRST (see
// the header comment on why this can't wait for the worker), then slide
// down and hand off to SCR_DIAL.
static void dismiss(void)
{
    dial_state_clear_ota_prompt_due();
    anim_sheet(lv_obj_get_y(s_sheet), SHEET_Y_CLOSED, 150, lv_anim_path_ease_in, close_ready_cb);
}

/* ---- row actions ---------------------------------------------------------*/

// "Update now": exactly the same confirmed-install path SCR_UPDATE's
// "Check for updates" row uses (CMD_OTA_APPLY) — nav_policy's OTA takeover
// check (ahead of everything else) forces SCR_UPDATING the moment the
// worker's progress callback commits OTA_DOWNLOADING, so this screen
// doesn't navigate itself; it just goes inert while that happens, same
// idiom as scr_update.c's row_ota_cb / s_ota_apply_sent.
static void btn_now_cb(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_created_at_tick) < OTA_PROMPT_GUARD_MS) return;
    if (s_apply_sent) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    dial_state_clear_ota_prompt_due();
    s_apply_sent = true;
    if (s_btn_now_lbl) lv_label_set_text(s_btn_now_lbl, "Starting install...");
    if (s_btn_later)   lv_obj_add_state(s_btn_later, LV_STATE_DISABLED);
    if (s_row_options) lv_obj_add_state(s_row_options, LV_STATE_DISABLED);
    app_cmd_t cmd = { .kind = CMD_OTA_APPLY };
    dial_cmd_post(&cmd);
}

static void btn_later_cb(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_created_at_tick) < OTA_PROMPT_GUARD_MS) return;
    dial_haptics_play(HAPTIC_TICK);
    dial_state_set_ota_defer((uint32_t)time(NULL) + OTA_DEFER_SECONDS);
    dismiss();
}

static void row_options_cb(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_created_at_tick) < OTA_PROMPT_GUARD_MS) return;
    dial_haptics_play(HAPTIC_TICK);
    dial_state_clear_ota_prompt_due();
    ui_router_go(SCR_UPDATE, NULL, LV_SCR_LOAD_ANIM_NONE);
}

/* ---- palette ---------------------------------------------------------------*/

static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(s_sheet, pal->surface, 0);
    lv_obj_set_style_bg_color(s_grab, pal->ink_secondary, 0);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_version_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_body_lbl, pal->ink_secondary, 0);

    // Primary CTA — filled surface + ink_primary border/text, this project's
    // "emphasized" look (same tokens scr_adjust_mode.c's SELECTED pill uses).
    lv_obj_set_style_bg_color(s_btn_now, pal->surface, 0);
    lv_obj_set_style_border_color(s_btn_now, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_btn_now_lbl, pal->ink_primary, 0);

    // Secondary — the neutral/"Back" look (track border, secondary ink).
    lv_obj_set_style_bg_color(s_btn_later, pal->surface, 0);
    lv_obj_set_style_border_color(s_btn_later, pal->track, 0);
    lv_obj_set_style_text_color(s_btn_later_lbl, pal->ink_secondary, 0);

    lv_obj_set_style_text_color(s_row_options_lbl, pal->ink_secondary, 0);
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    s_apply_sent = false;
    s_created_at_tick = lv_tick_get();   // input guard — see its own comment above
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_sheet = lv_obj_create(scr);
    lv_obj_set_size(s_sheet, SCREEN_W, SHEET_H);
    lv_obj_set_pos(s_sheet, 0, SHEET_Y_CLOSED);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    // Grab bar — same "this is a sheet, not just another screen" cue every
    // bottom sheet in this project opens with.
    s_grab = lv_obj_create(s_sheet);
    lv_obj_set_size(s_grab, 40, 4);
    lv_obj_set_style_radius(s_grab, 2, 0);
    lv_obj_set_style_border_width(s_grab, 0, 0);
    lv_obj_clear_flag(s_grab, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_grab, LV_ALIGN_TOP_MID, 0, 10);

    s_title_lbl = lv_label_create(s_sheet);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_title_lbl, "Update available");
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 24);

    s_version_lbl = lv_label_create(s_sheet);
    lv_obj_set_style_text_font(s_version_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_version_lbl, "");   // on_state fills this in
    lv_obj_align(s_version_lbl, LV_ALIGN_TOP_MID, 0, 50);

    // The install-cost line is not optional (spec) — someone tapping
    // "Update now" at bedtime must know their bed goes unadjustable for two
    // minutes BEFORE they tap, not find out after.
    s_body_lbl = lv_label_create(s_sheet);
    lv_obj_set_width(s_body_lbl, 300);
    lv_label_set_long_mode(s_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_body_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_body_lbl, "Takes about 2 minutes. The dial\nwon't respond while it installs.");
    lv_obj_align(s_body_lbl, LV_ALIGN_TOP_MID, 0, 74);

    // Primary — >=88px touch target (spec), full-width-ish so it reads as
    // the default action.
    s_btn_now = dial_btn_create(s_sheet);
    lv_obj_set_size(s_btn_now, 300, 88);
    lv_obj_set_style_radius(s_btn_now, 20, 0);
    lv_obj_set_style_border_width(s_btn_now, 2, 0);
    lv_obj_align(s_btn_now, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_add_event_cb(s_btn_now, btn_now_cb, LV_EVENT_CLICKED, NULL);
    s_btn_now_lbl = lv_label_create(s_btn_now);
    lv_obj_set_style_text_font(s_btn_now_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_btn_now_lbl, "Update now");
    lv_obj_center(s_btn_now_lbl);

    // Secondary — >=72px touch target (spec).
    s_btn_later = dial_btn_create(s_sheet);
    lv_obj_set_size(s_btn_later, 240, 72);
    lv_obj_set_style_radius(s_btn_later, 20, 0);
    lv_obj_set_style_border_width(s_btn_later, 1, 0);
    lv_obj_align(s_btn_later, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_add_event_cb(s_btn_later, btn_later_cb, LV_EVENT_CLICKED, NULL);
    s_btn_later_lbl = lv_label_create(s_btn_later);
    lv_obj_set_style_text_font(s_btn_later_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_btn_later_lbl, "Later");
    lv_obj_center(s_btn_later_lbl);

    // "Update options" — a text row, not a button: this is the hand-off to
    // SCR_UPDATE, where "stop asking" (Skip this version / Auto-update)
    // actually lives, deliberately lighter-weight than the two real choices
    // above (spec: "the sheet itself stays to three choices").
    // Kept narrow (180, not the buttons' 240-300) and as high as the layout
    // allows: at this y-band the round bezel's chord is already tight, and a
    // short two-word label has room to breathe there where a full-width row
    // would clip at its ends.
    s_row_options = lv_obj_create(s_sheet);
    lv_obj_set_size(s_row_options, 180, 40);
    lv_obj_set_style_bg_opa(s_row_options, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row_options, 0, 0);
    lv_obj_clear_flag(s_row_options, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_row_options, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_add_event_cb(s_row_options, row_options_cb, LV_EVENT_CLICKED, NULL);
    s_row_options_lbl = lv_label_create(s_row_options);
    lv_obj_set_style_text_font(s_row_options_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_row_options_lbl, "Update options");
    lv_obj_center(s_row_options_lbl);

    apply_palette();
    anim_sheet(SHEET_Y_CLOSED, SHEET_Y_OPEN, 180, lv_anim_path_ease_out, NULL);
}

static void destroy(void)
{
    if (s_sheet) lv_anim_del(s_sheet, NULL);
    s_sheet = s_grab = NULL;
    s_title_lbl = s_version_lbl = s_body_lbl = NULL;
    s_btn_now = s_btn_now_lbl = s_btn_later = s_btn_later_lbl = NULL;
    s_row_options = s_row_options_lbl = NULL;
    s_apply_sent = false;
}

static void on_state(const app_state_t *st)
{
    if (!s_sheet) return;
    apply_palette();

    // Version text and the button/row styling below are harmless to
    // re-render unconditionally — unlike scr_update.c's status row, nothing
    // here derives its TEXT from ota.status, so there's no "Starting
    // install..." to accidentally clobber on a routine re-render; s_apply_sent
    // only ever gates the one-time transition in btn_now_cb above.
    lv_label_set_text(s_version_lbl, st->ota.latest);
}

// Nothing on this sheet is knob-adjustable — the whole point is a fast
// tap/swipe decision, not a rotor to dial through.
static bool on_knob(int detents) { (void)detents; return false; }

static bool on_gesture(lv_dir_t dir)
{
    // Swipe-to-dismiss is deliberately GONE (owner, 2026-08-04: "I tried to
    // scroll through the options and it closed on me"). Three stacked options
    // look like a list, so a downward drag is what a user naturally tries in
    // order to read them — and it was being taken as "Later", silently
    // deferring the prompt for 23 hours. The sheet already offers an explicit
    // Later button, so the gesture bought nothing and cost the one
    // interaction people actually attempt. The knob is likewise inert here
    // (on_knob returns false): with no encoder push there is nothing for a
    // focus ring to activate, so moving focus would be a dead end.
    (void)dir;
    return false;
}

const ui_screen_t scr_update_prompt = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
