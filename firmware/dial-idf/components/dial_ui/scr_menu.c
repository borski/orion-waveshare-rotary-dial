/*
 * SCR_MENU — the third face of the swipe chain (chain is
 * Dial(A) <-left- Dial(B) <-left- Menu). Settings/Update/Wi-Fi/About are not
 * faces of the chain themselves: they're sub-screens one tap off this list,
 * each dismissed by swiping RIGHT back to here rather than all the way to
 * Dial(B). That indirection exists so the chain stays a fixed 3 faces
 * regardless of how many settings-ish screens accumulate.
 *
 * No title label: the focused row IS the heading, and the page dots are
 * what tell you you're on face 3 of 3 — a heading would either crowd a row
 * or get cropped by the physical round bezel up top.
 *
 * The rows live in a rotor list (dial_list.h): one row snaps focused to the
 * vertical center, neighbors shrink/fade toward the bezel, and the knob
 * walks focus one row per detent.
 *
 * Row order (owner-approved): Back, Settings, Update, Wi-Fi, About. The
 * rotor opens focused on the first row after Back (dial_list_settle(s_list,
 * 1) below), so that slot belongs to whatever destination gets used most —
 * Settings, not a one-off reference screen. Update moved up from last for
 * the same reason (it's the row you come back to check on); Wi-Fi and About
 * are post-setup reference material and sink to the bottom.
 *
 * "Update" is a PERMANENT row (M7), adjacent to About — unlike the M6
 * "Install X.Y.Z" row it replaces, it always exists and never arms a
 * confirm of its own: tapping it just navigates to SCR_UPDATE, where the
 * owner wanted OTA confirmation to actually happen ("this is where users
 * will actually confirm they want the OTA"). Its value label is the only
 * thing that changes with OTA status — see sync_update_row() below.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_ota.h"

#define CX 180
#define CY 180
#define ARC_R 165
#define ROW_H 72
#define DOT_D 12      // notification dot diameter

static lv_obj_t *s_ring;
static lv_obj_t *s_list;
static lv_obj_t *s_dot_a, *s_dot_b, *s_dot_menu;

// "Update" row's value label — "" when idle/up-to-date, the pending version
// (or "New" if it somehow arrives blank) while one is available. The row
// itself is permanent (see make_update_row); only this label's text changes.
static lv_obj_t *s_dot_update;

/* ---- row factory -----------------------------------------------------*/

// One callback for all four single-label rows: the destination screen rides
// in user_data (bound at create(), same idiom scr_dial.c uses for the zone a
// widget was built for), so adding another near-identical row never means
// adding a near-identical callback.
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

/* ---- "Update" row (M7) -----------------------------------------------
 * Permanent, unlike the M6 "Install X.Y.Z" row it replaces: it exists every
 * time this screen is open, and a tap only navigates to SCR_UPDATE (via the
 * same row_event_cb every other row uses — no confirm, no install command
 * posted from here). That's a deliberate split from the old row: the owner
 * wants the submenu to be where OTA is actually confirmed, and this row is
 * pure discoverability + navigation, matching About/Wi-Fi/Settings.
 *
 * Its value label is the one dynamic part — a small left-label/right-value
 * pair inside the same pill-row look the other rows use, rather than a
 * bespoke centered label, so "1.1.0" (or "New") reads as a badge next to
 * "Update" instead of replacing it.
 */

// Visible only while an update is actually pending. Colour comes from the
// palette pass (day/night can flip while this screen sits idle underneath
// another face), so this only decides presence.
static void set_update_row_dot(lv_obj_t *dot, const app_state_t *st)
{
    if (!dot) return;
    bool pending = (dial_ota_status_t)st->ota.status == OTA_AVAILABLE;
    if (pending) lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_update_row(lv_obj_t *parent, const app_state_t *st)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 24, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)SCR_UPDATE);

    // CENTERED, exactly like every other row on this menu (make_row's
    // lv_obj_center) — this row borrowed the Settings-screen label/value
    // split at first, and a left-aligned label sitting among centered ones
    // reads as broken alignment, not as a different row type.
    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, "Update");
    lv_obj_center(lbl);

    // A notification dot rather than the version string (owner request): at a
    // glance the only question this row answers from the menu is "is there
    // something waiting", and a dot answers it without competing with the
    // label for width or needing to be read. The version itself is one tap
    // away on SCR_UPDATE, which shows it against the installed one.
    //
    // Aligned OUT_RIGHT of the label rather than to the row's right edge, so
    // it reads as attached to the word "Update" instead of floating at the
    // bezel -- and it tracks the label, which stays centered like every
    // other row on this menu.
    s_dot_update = lv_obj_create(row);
    lv_obj_set_size(s_dot_update, DOT_D, DOT_D);
    lv_obj_set_style_radius(s_dot_update, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_update, 0, 0);
    lv_obj_clear_flag(s_dot_update, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(s_dot_update, lbl, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    set_update_row_dot(s_dot_update, st);

    return row;
}

// Keeps the value label in step with the worker-owned OTA status. Called
// from both create() (a menu opened with an update already pending shows
// the badge from frame one) and on_state() (a check/download that starts or
// finishes WHILE the menu is open updates it live, without leaving the
// router). Unlike the row it replaces, the ROW itself never appears or
// disappears — only this label's text does — so there is no dial_list child-
// count/transform bookkeeping to redo here.
static void sync_update_row(const app_state_t *st)
{
    set_update_row_dot(s_dot_update, st);
}

/* ---- palette -----------------------------------------------------------*/

// Walks s_list generically (row -> every label it has) instead of naming
// each row's labels — same "palette walk" scr_settings.c/scr_about.c use
// for their own longer row lists. Needed here (not just a single-label
// walk) now that the Update row carries two labels of its own.
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
        uint32_t rc = lv_obj_get_child_cnt(row);
        for (uint32_t j = 0; j < rc; j++) {
            lv_obj_t *lbl = lv_obj_get_child(row, j);
            lv_obj_set_style_text_color(lbl, j == 0 ? pal->ink_primary : pal->ink_secondary, 0);
        }
    }

    // The dot is an lv_obj, not a label, so the text-colour pass above does
    // nothing for it. ink_primary, deliberately NOT `warning`: an available
    // update is an offer, not a fault, and this dial reserves the warning
    // tone for things that are actually wrong. At night the palette's own
    // ink_primary is already warm-dimmed, so the dot quiets down with
    // everything else instead of glowing at a sleeping household.
    if (s_dot_update) lv_obj_set_style_bg_color(s_dot_update, pal->ink_primary, 0);

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
    s_dot_update = NULL;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Chassis hairline ring — identical geometry to scr_standby's: every
    // face is the same chassis object, just a different face of it
    // (design-spec.md's "a chassis that persists").
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

    app_state_t st;
    dial_state_get(&st);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", SCR_DIAL);
    make_row(s_list, "Settings", SCR_SETTINGS);
    make_update_row(s_list, &st);
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

    apply_palette(&st);
    dial_list_settle(s_list, 1);   // open on "Settings", not on Back
}

static void destroy(void)
{
    s_ring = NULL;
    s_list = NULL;
    s_dot_a = s_dot_b = s_dot_menu = NULL;
    s_dot_update = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    sync_update_row(st);
    apply_palette(st);   // palette + the page-dot row (zone count can change under us)
}

// The knob walks the focused row (one per detent); ends of the list voice
// the same range-stop haptic the dial's temperature stops use.
static bool on_knob(int detents)
{
    if (!s_list) return false;
    int r = dial_list_knob(s_list, detents);
    if (r < 0) dial_haptics_play_soft(HAPTIC_STOP);   // rotor hit its first/last row
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
