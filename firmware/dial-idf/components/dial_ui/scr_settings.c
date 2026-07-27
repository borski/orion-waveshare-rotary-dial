/*
 * SCR_SETTINGS — full-screen scrollable settings list. Reached from the menu
 * face (scr_menu.c's "Settings" row); swipe right returns there. Wi-Fi and
 * software-update controls live on their own menu sub-screens (scr_wifi.c /
 * scr_about.c), so this list is only the preference + account rows.
 *
 * Rows >=72px tall: label (Mont 24) left, value (Mont 16) right-aligned,
 * living in a rotor list (dial_list.h — snap-centered focus row, edge rows
 * zoomed/faded for the round panel; knob walks one row per detent, a finger
 * drag free-scrolls then snaps). Tap activates a row. The two destructive
 * rows (re-link/factory reset) use a tap-twice-within-3s confirm pattern
 * instead of firing immediately. The two brightness rows (Day/Night) use a
 * tap-to-edit pattern instead: a tap steals the knob from the list to drive
 * that row's percent live (with a real dial_power_preview() fade on every
 * detent), until a second tap, 5s of inactivity, or leaving the screen
 * commits it — see the "brightness edit mode" section below.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_display.h"
#include "dial_power.h"

#define CY 180
#define ROW_H          76
#define CONFIRM_WINDOW_MS 3000
#define BRI_EDIT_TIMEOUT_MS 5000
#define BRI_STEP_PCT   10

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_scale, *s_val_units, *s_val_haptics, *s_val_rotation;
static lv_obj_t *s_row_bri_day, *s_val_bri_day;
static lv_obj_t *s_row_bri_night, *s_val_bri_night;

typedef enum { CONFIRM_RELINK = 0, CONFIRM_FACTORY, CONFIRM_COUNT } confirm_id_t;
static lv_obj_t   *s_val_confirm[CONFIRM_COUNT];
static confirm_id_t s_armed = CONFIRM_COUNT;   // CONFIRM_COUNT = "none armed"
static uint32_t     s_armed_at_ms;
static lv_timer_t   *s_confirm_timer;          // also drives the brightness edit timeout below

// Day/night brightness tap-to-edit. Only one row (or neither) is ever being
// edited; while it is, the knob drives s_bri_pct instead of the rotor list
// (see on_knob), and the row's value label is drawn accented (see
// apply_palette). Entered/exited exclusively through bri_edit_enter/exit so
// the preview + persistence + list-routing invariants can't drift apart.
typedef enum { BRI_EDIT_NONE = -1, BRI_EDIT_DAY = 0, BRI_EDIT_NIGHT = 1 } bri_edit_t;
static bri_edit_t s_bri_edit = BRI_EDIT_NONE;
static uint8_t    s_bri_pct;
static uint32_t   s_bri_edit_at_ms;

// Forward decl: brightness edit mode (below) re-renders the affected row's
// accent the instant it's entered/exited, well before this screen's own
// palette section is defined further down the file.
static void apply_palette(lv_obj_t *scr);

/* ---- row factory --------------------------------------------------------*/

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t cb, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    // 36px side insets, not 20: rows near the top/bottom of the list sit
    // where the round panel's chord is narrower, and 20 put label/value
    // ends outside the visible circle (owner-reported clipping).
    lv_obj_set_style_pad_hor(row, 36, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, label_txt);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_label_set_text(val, "");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    if (value_out) *value_out = val;

    return row;
}

/* ---- confirm-row helper (re-link / Wi-Fi reset / factory reset) --------- */

static void confirm_set_label(confirm_id_t id, const char *txt)
{
    if (s_val_confirm[id]) lv_label_set_text(s_val_confirm[id], txt);
}

static void confirm_disarm(void)
{
    if (s_armed != CONFIRM_COUNT) confirm_set_label(s_armed, "");
    s_armed = CONFIRM_COUNT;
}

// Forward decl: brightness edit mode's own exit, called below from the same
// 250ms poll that ages out an armed confirm row (see the timer's new second
// job, right underneath).
static void bri_edit_exit(void);

// Ticks while a confirm row is armed, so the "tap again" prompt reverts if
// the 3s window lapses without a second tap. Also ages out brightness edit
// mode after 5s of no knob/tap activity (task spec's 3rd exit condition) —
// one screen, one lightweight poll timer, same as the confirm rows use.
static void confirm_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_armed != CONFIRM_COUNT && lv_tick_elaps(s_armed_at_ms) >= CONFIRM_WINDOW_MS)
        confirm_disarm();
    if (s_bri_edit != BRI_EDIT_NONE && lv_tick_elaps(s_bri_edit_at_ms) >= BRI_EDIT_TIMEOUT_MS)
        bri_edit_exit();
}

// Returns true if this tap landed within the window of a matching prior tap
// (i.e. the action should fire now).
static bool confirm_tap(confirm_id_t id)
{
    if (s_armed == id && lv_tick_elaps(s_armed_at_ms) < CONFIRM_WINDOW_MS) {
        confirm_disarm();
        return true;
    }
    confirm_disarm();
    s_armed = id;
    s_armed_at_ms = lv_tick_get();
    confirm_set_label(id, "Tap again to confirm");
    return false;
}

/* ---- brightness edit mode -------------------------------------------------
 * Tap a Day/Night brightness row to enter; the knob then drives s_bri_pct
 * (+-10%/detent, clamped 10..100) instead of walking the rotor list, with a
 * live dial_power_preview() so the panel shows the candidate value as it's
 * dialed in. Exits (second tap of the same row / 5s idle / teardown) always
 * go through bri_edit_exit(), which is what actually persists the value —
 * nothing is written to NVS or dial_power's normal duty tables until then.
 */

static void bri_set_val_label(void)
{
    lv_obj_t *val = (s_bri_edit == BRI_EDIT_DAY) ? s_val_bri_day : s_val_bri_night;
    char buf[8];
    snprintf(buf, sizeof buf, "%u%%", (unsigned)s_bri_pct);
    lv_label_set_text(val, buf);
}

static void bri_edit_enter(bri_edit_t which)
{
    s_bri_edit = which;
    s_bri_pct = (which == BRI_EDIT_DAY) ? dial_state_get_bri_day_pct()
                                         : dial_state_get_bri_night_pct();
    s_bri_edit_at_ms = lv_tick_get();
    dial_power_preview(which == BRI_EDIT_NIGHT, s_bri_pct);
    bri_set_val_label();
    if (s_list) apply_palette(lv_obj_get_parent(s_list));
}

// The one exit path for every trigger in the header comment above (second
// tap / 5s timeout / screen teardown) — persists, ends the live preview, and
// tells dial_power to re-fade to the real (non-preview) duty. No-op if
// nothing is being edited, so callers (including destroy()) can call it
// unconditionally.
static void bri_edit_exit(void)
{
    if (s_bri_edit == BRI_EDIT_NONE) return;
    bri_edit_t was = s_bri_edit;
    dial_power_preview_end();
    if (was == BRI_EDIT_DAY) dial_state_set_bri_day_pct(s_bri_pct);
    else                     dial_state_set_bri_night_pct(s_bri_pct);
    dial_power_brightness_changed();
    s_bri_edit = BRI_EDIT_NONE;
    if (s_list) apply_palette(lv_obj_get_parent(s_list));
}

/* ---- row actions ---------------------------------------------------------*/

// Row 0 on every menu sub-screen: swiping right still works, but the gesture
// isn't discoverable on its own (owner feedback), and a row is the one back
// affordance that can't occlude the list it sits in.
static void row_back_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

// Which way is "up" is a property of the room, not the device: the dial's cable
// exits one edge, and on a nightstand that edge is as likely to point at the
// bed as away from it. Cycles 0 -> 90 -> 180 -> 270 and applies immediately, so
// the effect of the tap is the thing you're looking at.
static void row_rotation_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    uint8_t next = (st.rotation + 1) & 3;
    if (!dial_display_set_rotation(next)) {
        dial_haptics_play(HAPTIC_ERROR);   // no memory for the 90/270 scratch buffer
        return;
    }
    dial_state_set_rotation(next);
    dial_haptics_play(HAPTIC_TICK);
}

// Setpoint scale: Absolute (°F/°C) <-> Relative (−10…+10 levels). Independent
// of Units below, which continues to govern the absolute readouts (the water
// caption) in either scale.
static void row_scale_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    dial_haptics_play(HAPTIC_TICK);
    dial_state_set_rel_mode(!st.rel_mode);
}

static void row_units_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    dial_haptics_play(HAPTIC_TICK);
    dial_state_set_units_c(!st.units_c);
}

static void row_haptics_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    bool enabled = !st.haptics_enabled;
    dial_haptics_set_enabled(enabled);
    dial_state_set_haptics_enabled(enabled);
    dial_haptics_play(HAPTIC_CONFIRM);   // audible only if now enabled
}

// Tap toggles edit mode for THIS row: a second tap while it's already the one
// being edited commits (same as the timeout); a first tap — whether nothing
// was being edited, or the OTHER brightness row was — (re)enters it here,
// cleanly closing out whatever was open first so preview/persist never leaks
// between the two rows.
static void row_bri_day_cb(lv_event_t *e)
{
    (void)e;
    if (s_bri_edit == BRI_EDIT_DAY) {
        bri_edit_exit();
        dial_haptics_play(HAPTIC_CONFIRM);
        return;
    }
    bri_edit_exit();   // no-op unless the NIGHT row was mid-edit
    bri_edit_enter(BRI_EDIT_DAY);
    dial_haptics_play(HAPTIC_TICK);
}

static void row_bri_night_cb(lv_event_t *e)
{
    (void)e;
    if (s_bri_edit == BRI_EDIT_NIGHT) {
        bri_edit_exit();
        dial_haptics_play(HAPTIC_CONFIRM);
        return;
    }
    bri_edit_exit();   // no-op unless the DAY row was mid-edit
    bri_edit_enter(BRI_EDIT_NIGHT);
    dial_haptics_play(HAPTIC_TICK);
}

static void row_relink_cb(lv_event_t *e)
{
    (void)e;
    if (!confirm_tap(CONFIRM_RELINK)) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_RELINK };
    dial_cmd_post(&cmd);
}

static void row_factory_reset_cb(lv_event_t *e)
{
    (void)e;
    if (!confirm_tap(CONFIRM_FACTORY)) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_FACTORY_RESET };
    dial_cmd_post(&cmd);
}

/* ---- palette --------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    if (s_title_lbl) lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);

    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        lv_obj_set_style_border_color(row, pal->track, 0);
        uint32_t rc = lv_obj_get_child_cnt(row);
        for (uint32_t j = 0; j < rc; j++) {
            lv_obj_t *lbl = lv_obj_get_child(row, j);
            lv_obj_set_style_text_color(lbl, j == 0 ? pal->ink_primary : pal->ink_secondary, 0);
        }
    }

    // Brightness edit mode: accent the row being edited (border + value label
    // both bumped to ink_primary, same weight as the label column already
    // uses) so it reads as "live" without reaching for a thermal/warning/
    // identity token that means something else everywhere else on the dial.
    if (s_bri_edit != BRI_EDIT_NONE) {
        lv_obj_t *row = (s_bri_edit == BRI_EDIT_DAY) ? s_row_bri_day : s_row_bri_night;
        lv_obj_t *val = (s_bri_edit == BRI_EDIT_DAY) ? s_val_bri_day : s_val_bri_night;
        if (row) lv_obj_set_style_border_color(row, pal->ink_primary, 0);
        if (val) lv_obj_set_style_text_color(val, pal->ink_primary, 0);
    }
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_armed = CONFIRM_COUNT;
    s_bri_edit = BRI_EDIT_NONE;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    // No "My side" row: it only re-ran SCR_SIDEPICK, which sets the very same
    // ui_zone that one swipe on the dial already sets (and persists) — the row
    // changed nothing you couldn't change faster by swiping.
    make_row(s_list, LV_SYMBOL_LEFT "  Back", row_back_cb, NULL);
    make_row(s_list, "Scale",         row_scale_cb,         &s_val_scale);
    make_row(s_list, "Units",         row_units_cb,         &s_val_units);
    make_row(s_list, "Rotation",      row_rotation_cb,      &s_val_rotation);
    make_row(s_list, "Haptics",       row_haptics_cb,       &s_val_haptics);
    s_row_bri_day   = make_row(s_list, "Day brightness",   row_bri_day_cb,   &s_val_bri_day);
    s_row_bri_night = make_row(s_list, "Night brightness", row_bri_night_cb, &s_val_bri_night);
    make_row(s_list, "Re-link Orion", row_relink_cb,        &s_val_confirm[CONFIRM_RELINK]);
    make_row(s_list, "Factory reset", row_factory_reset_cb, &s_val_confirm[CONFIRM_FACTORY]);

    // "Tap again to confirm" is too long to share one line with "Re-link
    // Orion"/"Factory reset" — right-aligned beside them it ran INTO them
    // (same collision scr_about.c's Software update row had). So these two
    // rows' value labels sit on a second left-aligned line under the label
    // instead (scr_about.c's stacked treatment), width-capped with LONG_DOT.
    // They hold "" except while armed, so every other state keeps the
    // two-column label+value look of the rest of the list untouched.
    for (int i = 0; i < CONFIRM_COUNT; i++) {
        lv_obj_set_width(s_val_confirm[i], LV_PCT(100));
        lv_label_set_long_mode(s_val_confirm[i], LV_LABEL_LONG_DOT);
        lv_obj_align(s_val_confirm[i], LV_ALIGN_LEFT_MID, 0, 26);
    }

    // Created AFTER the list so it draws over rows scrolling beneath it —
    // same fixed title slot the other menu sub-screens use.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "SETTINGS");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr);
    dial_list_settle(s_list, 1);   // open on "Scale" (index 1), not on Back
    s_confirm_timer = lv_timer_create(confirm_timer_cb, 250, NULL);
}

static void destroy(void)
{
    if (s_confirm_timer) { lv_timer_del(s_confirm_timer); s_confirm_timer = NULL; }
    // Teardown is one of bri_edit_exit's three documented triggers (alongside
    // a second tap / the 5s timeout) — persists whatever was being dialed in
    // and ends the live LEDC preview so it can never outlive this screen.
    // No-op if nothing was being edited.
    bri_edit_exit();
    s_list = NULL;
    s_title_lbl = NULL;
    s_val_scale = s_val_units = s_val_haptics = s_val_rotation = NULL;
    s_val_bri_day = s_val_bri_night = NULL;
    s_row_bri_day = s_row_bri_night = NULL;
    for (int i = 0; i < CONFIRM_COUNT; i++) s_val_confirm[i] = NULL;
    s_armed = CONFIRM_COUNT;
}

static void on_state(const app_state_t *st)
{
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));

    static const char *ROT[] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
    lv_label_set_text(s_val_rotation, ROT[st->rotation & 3]);

    lv_label_set_text(s_val_scale, st->rel_mode ? "Relative" : "Absolute");
    // In relative mode Units governs only the water readout, so qualify it —
    // otherwise tapping Units with the hero in levels looks like it did nothing.
    if (st->rel_mode)
        lv_label_set_text(s_val_units, st->units_c ? "\xC2\xB0" "C (water)" : "\xC2\xB0" "F (water)");
    else
        lv_label_set_text(s_val_units, st->units_c ? "\xC2\xB0" "C" : "\xC2\xB0" "F");
    lv_label_set_text(s_val_haptics, st->haptics_enabled ? "On" : "Off");

    // While a brightness row is being edited its label already shows the live
    // (not-yet-persisted) candidate value — see bri_set_val_label — so a
    // state re-render here (some unrelated store commit landing mid-edit)
    // must not stomp it with the still-old value still sitting in the store.
    char buf[8];
    if (s_bri_edit != BRI_EDIT_DAY) {
        snprintf(buf, sizeof buf, "%u%%", (unsigned)st->bri_day_pct);
        lv_label_set_text(s_val_bri_day, buf);
    }
    if (s_bri_edit != BRI_EDIT_NIGHT) {
        snprintf(buf, sizeof buf, "%u%%", (unsigned)st->bri_night_pct);
        lv_label_set_text(s_val_bri_night, buf);
    }
}

// The knob walks the focused row (one per detent, dial_list's rotor snap) —
// unless a brightness row is being edited, in which case it drives that row's
// percent instead (+-10%/detent, clamped 10..100, live-previewed).
static bool on_knob(int detents)
{
    if (!s_list || detents == 0) return false;

    if (s_bri_edit != BRI_EDIT_NONE) {
        int pct = (int)s_bri_pct + detents * BRI_STEP_PCT;
        if (pct < 10)  pct = 10;
        if (pct > 100) pct = 100;
        s_bri_edit_at_ms = lv_tick_get();   // any detent resets the 5s idle window
        if (pct == (int)s_bri_pct) {
            dial_haptics_play(HAPTIC_STOP);   // clamped at a range end
            return true;
        }
        s_bri_pct = (uint8_t)pct;
        dial_power_preview(s_bri_edit == BRI_EDIT_NIGHT, s_bri_pct);
        bri_set_val_label();
        dial_haptics_play(HAPTIC_TICK);
        return true;
    }

    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_settings = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
