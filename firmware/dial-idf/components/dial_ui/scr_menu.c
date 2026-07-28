/*
 * SCR_MENU — the third face of the swipe chain, replacing the old spot
 * Tonight used to occupy directly off Dial(B) (chain is now
 * Dial(A) <-left- Dial(B) <-left- Menu). Tonight/Settings/Wi-Fi/About are
 * no longer faces of the chain themselves: they're sub-screens one tap
 * off this list, each dismissed by swiping RIGHT back to here rather than
 * all the way to Dial(B). That indirection exists so the chain stays a
 * fixed 3 faces regardless of how many settings-ish screens accumulate.
 *
 * No title label: the focused row IS the heading, and the page dots are
 * what tell you you're on face 3 of 3 — a heading would either crowd a row
 * or get cropped by the physical round bezel up top.
 *
 * The rows live in a rotor list (dial_list.h): one row snaps focused to the
 * vertical center, neighbors shrink/fade toward the bezel, and the knob
 * walks focus one row per detent.
 *
 * One row is conditional: "Install X.Y.Z" (M6 discoverability — the update
 * control used to live only in About, which owner feedback called hard to
 * find), appended after About whenever st->ota.status is OTA_AVAILABLE and
 * removed the instant it isn't — see the "install update row" section below.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_ota.h"

#define CX 180
#define CY 180
#define ARC_R 165
#define ROW_H 72
#define OTA_CONFIRM_WINDOW_MS 3000

static lv_obj_t *s_ring;
static lv_obj_t *s_list;
static lv_obj_t *s_dot_a, *s_dot_b, *s_dot_menu;

// "Install X.Y.Z" row — present only while an update is available.
static lv_obj_t   *s_row_ota;          // NULL when no update is available
static lv_obj_t   *s_lbl_ota;          // that row's single centered label
static bool        s_ota_armed;        // "Tap again to install" is showing
static uint32_t    s_ota_armed_at_ms;
static bool        s_ota_apply_sent;   // confirmed; holding the label until the worker catches up
static char        s_ota_latest[16];   // cached "X.Y.Z" for the resting label text
static lv_timer_t *s_ota_confirm_timer;

/* ---- row factory -----------------------------------------------------*/

// One callback for all four rows: the destination screen rides in
// user_data (bound at create(), same idiom scr_dial.c uses for the zone a
// widget was built for), so adding a fifth row never means adding a fifth
// near-identical callback.
// The dial face the menu returns to: whichever side the user was last on
// (ui_zone), which is also the only side that exists on a single-zone topper.
static zone_idx_t back_zone(void)
{
    app_state_t st;
    dial_state_get(&st);
    return st.ui_zone;
}

static void row_event_cb(lv_event_t *e)
{
    dial_haptics_play(HAPTIC_TICK);
    screen_id_t dest = (screen_id_t)(uintptr_t)lv_event_get_user_data(e);
    // The Back row is the one destination that moves BACKWARD along the chain
    // (to the dial the menu was swiped in from), so it gets the reverse
    // transition and the zone arg the dial needs; every other row descends
    // into a sub-screen. Keeping it a row (rather than floating chrome) means
    // it never occludes the list and the knob can reach it like anything else.
    if (dest == SCR_DIAL) {
        ui_router_go(SCR_DIAL, (void *)(uintptr_t)back_zone(), LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        return;
    }
    ui_router_go(dest, NULL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, screen_id_t dest)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    // Pressed-state color itself is set in the palette pass (day/night can
    // flip while this screen is sitting idle underneath another face) —
    // only the opacity/selector is fixed at create time.
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)dest);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, label_txt);
    lv_obj_center(lbl);

    return row;
}

/* ---- "install update" row (M6 discoverability) ----------------------------
 * Appended after About whenever st->ota.status == OTA_AVAILABLE, deleted the
 * instant it isn't — an actual add/remove rather than a hidden always-present
 * row, because dial_list's rotor math (dial_list.c) counts children directly
 * (lv_obj_get_child_cnt) for both its knob-clamp range and its zoom/fade
 * pass, and LVGL's flex layout skips HIDDEN children entirely (lv_flex.c) —
 * a hidden row would still occupy a knob-reachable slot with no widget ever
 * laid out under it, an off-by-one the user would feel as the list falling
 * one row short at the far end. Add/remove keeps the child count and the
 * visible row count in exact agreement, at the cost of this section existing.
 *
 * Installing takes over the screen for minutes, so a stray tap must not fire
 * it — same tap-twice-within-3s confirm pattern scr_settings.c's destructive
 * rows and scr_about.c's Software update row use (About's s_ota_apply_sent
 * latch included: it holds "Starting install..." over the gap between the
 * confirming tap and the worker actually draining CMD_OTA_APPLY, so a routine
 * unrelated state commit landing in that gap can't repaint the row back to
 * its pre-confirm text — see scr_about.c's row_ota_cb/render_ota_row comments
 * for the field incident that guard fixes).
 */

static void set_ota_resting_label(void)
{
    if (!s_lbl_ota) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Install %s", s_ota_latest);
    lv_label_set_text(s_lbl_ota, buf);
}

static void row_install_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    if ((dial_ota_status_t)st.ota.status != OTA_AVAILABLE) return;   // stale tap, row about to go
    if (s_ota_apply_sent) return;   // already confirmed; worker hasn't drained CMD_OTA_APPLY yet

    if (s_ota_armed && lv_tick_elaps(s_ota_armed_at_ms) < OTA_CONFIRM_WINDOW_MS) {
        s_ota_armed = false;
        dial_haptics_play(HAPTIC_CONFIRM);
        s_ota_apply_sent = true;
        if (s_lbl_ota) lv_label_set_text(s_lbl_ota, "Starting install...");
        app_cmd_t cmd = { .kind = CMD_OTA_APPLY };
        dial_cmd_post(&cmd);
        return;
    }

    s_ota_armed = true;
    s_ota_armed_at_ms = lv_tick_get();
    dial_haptics_play(HAPTIC_TICK);
    if (s_lbl_ota) lv_label_set_text(s_lbl_ota, "Tap again to install");
}

// Ages the "Tap again to install" prompt back out after 3s of no second tap —
// same 250ms sweep idiom scr_settings.c/scr_about.c run for their own confirm
// rows (which this screen otherwise has none of, hence its own small timer).
static void ota_confirm_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ota_armed && lv_tick_elaps(s_ota_armed_at_ms) >= OTA_CONFIRM_WINDOW_MS) {
        s_ota_armed = false;
        set_ota_resting_label();
    }
}

static lv_obj_t *make_ota_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_install_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_ota = lv_label_create(row);
    lv_obj_set_style_text_font(s_lbl_ota, &lv_font_montserrat_24, 0);
    lv_obj_center(s_lbl_ota);
    return row;
}

// dial_list's zoom/fade recompute (rotor_update, dial_list.c) only runs off
// LV_EVENT_SCROLL — fine for the knob/drag paths that already fire one, but
// a row this function adds or removes lands OUTSIDE that path (create()'s
// initial build is covered by dial_list_settle()'s own pass instead; this
// matters for a row appearing/disappearing while the menu is already open).
// Force a layout + synthesize the scroll event dial_list already listens
// for, so the row is never left sitting at LVGL's untransformed defaults
// until the next real scroll or knob turn.
static void refresh_list_transforms(void)
{
    lv_obj_update_layout(s_list);
    lv_event_send(s_list, LV_EVENT_SCROLL, NULL);
}

// Keeps the row's existence + label in step with the worker-owned OTA
// status. Called from both create() (a menu opened with an update already
// pending shows the row from frame one) and on_state() (a check/download
// that starts or finishes WHILE the menu is open is reflected live, without
// leaving the router).
static void sync_ota_row(const app_state_t *st)
{
    bool avail = ((dial_ota_status_t)st->ota.status == OTA_AVAILABLE);
    if (avail) strlcpy(s_ota_latest, st->ota.latest, sizeof(s_ota_latest));

    if (avail && !s_row_ota) {
        s_row_ota = make_ota_row(s_list);
        set_ota_resting_label();
        refresh_list_transforms();
        return;
    }
    if (!avail && s_row_ota) {
        lv_obj_del(s_row_ota);
        s_row_ota = NULL;
        s_lbl_ota = NULL;
        s_ota_armed = false;
        s_ota_apply_sent = false;
        refresh_list_transforms();
        return;
    }
    if (!s_row_ota || s_ota_armed) return;   // no row to label, or "Tap again..." holds
    if (s_ota_apply_sent) return;            // holding "Starting install..." (see header comment)
    set_ota_resting_label();
}

/* ---- palette -----------------------------------------------------------*/

// Walks s_list generically (row -> its one label) instead of naming four
// pairs of statics — same "palette walk" scr_settings.c's apply_palette
// uses for its longer row list.
static void apply_palette(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_ring);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);

    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        lv_obj_set_style_bg_color(row, pal->surface, LV_STATE_PRESSED);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_color(lbl, pal->ink_primary, 0);
    }

    // Page dots — same row the dial faces draw (dial_dots_layout owns which
    // dots exist and where); Menu's own dot is always the filled one here.
    dial_dots_layout(st, s_dot_b, s_dot_a, s_dot_menu);
    lv_obj_set_style_bg_color(s_dot_a, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_b, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_menu, pal->ink_secondary, 0);
}

/* ---- vtable ------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_row_ota = NULL;
    s_lbl_ota = NULL;
    s_ota_armed = false;
    s_ota_apply_sent = false;
    s_ota_latest[0] = '\0';
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Chassis hairline ring — identical geometry to scr_tonight's/
    // scr_standby's: every face is the same chassis object, just a
    // different face of it (design-spec.md's "a chassis that persists").
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

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", SCR_DIAL);
    make_row(s_list, "Tonight",  SCR_TONIGHT);
    make_row(s_list, "Settings", SCR_SETTINGS);
    make_row(s_list, "Wi-Fi",    SCR_WIFI);
    make_row(s_list, "About",    SCR_ABOUT);

    s_dot_a = lv_obj_create(scr);
    lv_obj_set_size(s_dot_a, 6, 6);
    lv_obj_set_style_radius(s_dot_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_a, 0, 0);
    lv_obj_clear_flag(s_dot_a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_a, LV_ALIGN_CENTER, 164 - CX, 340 - CY);

    s_dot_b = lv_obj_create(scr);
    lv_obj_set_size(s_dot_b, 6, 6);
    lv_obj_set_style_radius(s_dot_b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_b, 0, 0);
    lv_obj_clear_flag(s_dot_b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_b, LV_ALIGN_CENTER, 180 - CX, 340 - CY);

    s_dot_menu = lv_obj_create(scr);
    lv_obj_set_size(s_dot_menu, 6, 6);
    lv_obj_set_style_radius(s_dot_menu, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_menu, 0, 0);
    lv_obj_clear_flag(s_dot_menu, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_menu, LV_ALIGN_CENTER, 196 - CX, 340 - CY);

    app_state_t st;
    dial_state_get(&st);
    // Before apply_palette()'s generic row walk, so a menu opened with an
    // update already available shows the "Install X.Y.Z" row correctly
    // colored from frame one instead of catching up on the next state commit.
    sync_ota_row(&st);
    apply_palette(&st);
    dial_list_settle(s_list, 1);   // open on "Tonight", not on Back
    s_ota_confirm_timer = lv_timer_create(ota_confirm_timer_cb, 250, NULL);
}

static void destroy(void)
{
    if (s_ota_confirm_timer) { lv_timer_del(s_ota_confirm_timer); s_ota_confirm_timer = NULL; }
    s_ring = NULL;
    s_list = NULL;
    s_dot_a = s_dot_b = s_dot_menu = NULL;
    s_row_ota = NULL;
    s_lbl_ota = NULL;
    s_ota_armed = false;
    s_ota_apply_sent = false;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    // Before apply_palette(), same ordering reason as create(): a row this
    // adds needs to exist before the generic palette walk reaches it.
    sync_ota_row(st);
    apply_palette(st);   // palette + the page-dot row (zone count can change under us)
}

// The knob walks the focused row (one per detent); ends of the list voice
// the same range-stop haptic the dial's temperature stops use.
static bool on_knob(int detents)
{
    if (!s_list) return false;
    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    // Back to the side the user came in from — ui_zone is already whatever that
    // swipe committed, so there's nothing to re-commit here.
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)back_zone(), LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_menu = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
