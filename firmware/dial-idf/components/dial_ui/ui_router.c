#include "ui_router.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dial_power.h"
#include "dial_haptics.h"

static const char *TAG = "ui_router";

static const ui_screen_t *s_screens[SCR_COUNT];
static screen_id_t        s_current = SCR_COUNT;   // none yet
static void              *s_current_arg;
static lv_timer_t        *s_dispatch;
static uint32_t           s_rendered_gen;
// dial_power's idle clock drives the standby/dial split independently of any
// state commit; polling it here (cheap: a volatile read) lets idle->standby
// and wake->dial transitions happen on their own tick instead of waiting for
// the next device-state generation bump.
static dial_power_level_t s_rendered_power_level = DPWR_ACTIVE;

// Knob detents accumulated from the decoder's esp_timer task; drained by the
// dispatcher in the LVGL task. Guarded by a spinlock: the decoder must never
// block (it shares its task with lv_tick_inc), and this critical section is
// a handful of instructions.
static portMUX_TYPE s_knob_mux = portMUX_INITIALIZER_UNLOCKED;
static int32_t      s_knob_accum;

static ui_nav_policy_t s_nav_policy;
void ui_router_set_nav_policy(ui_nav_policy_t policy) { s_nav_policy = policy; }

void ui_router_register(screen_id_t id, const ui_screen_t *scr)
{
    configASSERT(id < SCR_COUNT);
    s_screens[id] = scr;
}

void ui_router_knob_input(int detents)
{
    taskENTER_CRITICAL(&s_knob_mux);
    s_knob_accum += detents;
    taskEXIT_CRITICAL(&s_knob_mux);
}

screen_id_t ui_router_current(void) { return s_current; }

// Swipe gestures arrive on the screen object; forward to the active screen.
// All four directions are forwarded (SCR_BOOST/SCR_UPDATE_PROMPT dismiss on a
// down swipe) — screens that only care about left/right (scr_dial) filter the
// rest out themselves and return false.
static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_NONE) return;
    const ui_screen_t *scr = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (scr && scr->on_gesture && scr->on_gesture(dir)) {
        dial_state_stamp_input();
        // A consumed swipe is never also a tap. LVGL 8.4 still delivers
        // LV_EVENT_CLICKED on release to the object the touch STARTED on,
        // even after a gesture — and after a navigating swipe that object
        // is a widget of the outgoing screen, kept alive until the load
        // animation finishes, so its CLICKED would fire a second
        // navigation/action on finger-lift (e.g. the menu row a swipe-right
        // began on hijacking the exit back to the dial). Eat the rest of
        // the touch. Unconsumed gestures are left alone: a brisk drag along
        // the side of scr_dial's arc can read as a vertical gesture, and
        // eating that touch would drop the drag's LV_EVENT_RELEASED commit.
        lv_indev_wait_release(lv_indev_get_act());
    }
}

// Screens that must not let the display sleep underneath the user. Every one
// of these is a task with natural thinking pauses and no input: typing a
// password one detent per letter, holding a QR code up to a phone, watching
// an install run, choosing a boost duration, or reading the update sheet's
// three options. The rest of the UI — the dial face, the menu and its
// settings sub-screens — is deliberately NOT here: someone can wander off
// mid-menu and the standby clock taking over is exactly right.
static bool screen_blocks_sleep(screen_id_t id)
{
    switch (id) {
    case SCR_WELCOME:        // onboarding, waiting on a first tap
    case SCR_WIFI_PORTAL:    // "join this AP" instructions
    case SCR_NETPICK:        // picking a network with the knob
    case SCR_PASSKEY:        // one letter per detent — the worst case by far
    case SCR_OAUTH_QR:       // being scanned by a phone
    case SCR_SIDEPICK:       // first-run side choice
    case SCR_UPDATING:       // install in progress; screen is the progress bar
    case SCR_UPDATE_PROMPT:  // an offer the user is reading
    case SCR_BOOST:          // choosing a duration
    case SCR_BRIGHTNESS:     // live backlight preview — sleeping mid-adjust
                             // would both hide and change what is being set
        return true;
    default:
        return false;
    }
}

// Re-entering the current screen with a DIFFERENT arg rebuilds it (the dial
// screen swaps zones this way); with the same arg it's a no-op, so
// phase-driven navigation can call this every tick harmlessly.
void ui_router_go(screen_id_t id, void *arg, lv_scr_load_anim_t anim)
{
    configASSERT(id < SCR_COUNT && s_screens[id]);
    if (id == s_current && arg == s_current_arg) return;

    // lv_scr_load_anim's auto_del frees the OLD screen only when the load
    // animation completes; a second animated load starting inside that window
    // orphans the first old screen (LVGL never deletes it). A connect-fail
    // loop can flap phases faster than the 220ms animation for hours, leaking
    // a screen per flap until lv_obj_create() below returns NULL and the
    // load path dereferences it (field incident: LoadProhibited @0x20 in
    // lv_scr_load_anim). Forcing NONE (synchronous delete) whenever the
    // previous animated load could still be in flight closes the leak.
    static uint32_t s_last_anim_ms;
    if (anim != LV_SCR_LOAD_ANIM_NONE) {
        if (lv_tick_elaps(s_last_anim_ms) < 400) anim = LV_SCR_LOAD_ANIM_NONE;
        else s_last_anim_ms = lv_tick_get();
    }

    const ui_screen_t *old = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (old && old->destroy) old->destroy();

    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        // Out of LVGL heap — nothing sane to render. Skip the navigation
        // rather than crash; the next dispatch tick retries.
        LV_LOG_ERROR("ui_router: lv_obj_create failed, skipping nav");
        s_current = SCR_COUNT;   // force a rebuild on the next tick
        return;
    }
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_current = id;
    s_current_arg = arg;
    dial_power_inhibit(DPWR_INHIBIT_SCREEN, screen_blocks_sleep(id));
    s_screens[id]->create(scr, arg);

    // auto_del frees the previous screen (and its widgets) after the animation.
    lv_scr_load_anim(scr, anim, anim == LV_SCR_LOAD_ANIM_NONE ? 0 : 220, 0, true);

    // Render current state immediately so the new screen never shows stale "--".
    app_state_t st;
    dial_state_get(&st);
    s_rendered_gen = st.generation;
    if (s_screens[id]->on_state) s_screens[id]->on_state(&st);
}

// Ask the nav policy where the app belongs right now, ignoring the fact that
// the user is currently parked on a screen they opened on purpose, and go
// there. Every sticky branch in the policy keys off ui_router_current() —
// "they deliberately opened this, don't yank them off it" — so blanking the
// current screen for the length of the call is exactly what turns that
// question into "where do they belong?". The answer then comes from the
// policy itself instead of from a second, drifting copy of its rules living
// inside a screen: the menu used to hardcode SCR_DIAL here, which rendered an
// empty face whenever there was no dial to go back to (mid-link, mid-setup,
// or before the first device state lands).
void ui_router_go_home(lv_scr_load_anim_t anim)
{
    if (!s_nav_policy) return;
    app_state_t st;
    dial_state_get(&st);
    void *arg = s_current_arg;
    screen_id_t save = s_current;
    s_current = SCR_COUNT;
    screen_id_t want = s_nav_policy(&st, &arg);
    s_current = save;
    ui_router_go(want, arg, anim);
}

// Runs in the LVGL task every 50ms: drain knob detents into the active screen
// and re-render when the state generation moved. This is the ONLY place the
// worker's commits reach LVGL, so the worker never needs the LVGL lock.
static void dispatch_tick(lv_timer_t *t)
{
    (void)t;
    const ui_screen_t *scr = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (!scr) return;

    taskENTER_CRITICAL(&s_knob_mux);
    int32_t detents = s_knob_accum;
    s_knob_accum = 0;
    taskEXIT_CRITICAL(&s_knob_mux);

    // Knob turns are silent: the encoder's own detents are the feedback, and
    // a haptic pulse per detent on top of them reads as noise (owner
    // request). Muting around the dispatch catches every screen's on_knob --
    // including any added later -- while leaving taps, confirms and drags
    // (which have no mechanical feedback of their own) untouched.
    if (detents != 0 && scr->on_knob) {
        dial_haptics_mute(true);
        bool handled = scr->on_knob((int)detents);
        dial_haptics_mute(false);
        if (handled) dial_state_stamp_input();
    }

    app_state_t st;
    dial_state_get(&st);
    dial_power_level_t power_level = dial_power_level();
    bool gen_changed   = (st.generation != s_rendered_gen);
    bool power_changed = (power_level != s_rendered_power_level);
    if (gen_changed || power_changed) {
        s_rendered_gen = st.generation;
        s_rendered_power_level = power_level;
        if (s_nav_policy) {
            void *arg = s_current_arg;
            screen_id_t want = s_nav_policy(&st, &arg);
            ui_router_go(want, arg, LV_SCR_LOAD_ANIM_FADE_ON);  // no-op if unchanged
            scr = s_screens[s_current];
        }
        if (scr && scr->on_state) scr->on_state(&st);
    }
}

void ui_router_start(screen_id_t first, void *arg)
{
    configASSERT(!s_dispatch);
    s_dispatch = lv_timer_create(dispatch_tick, 50, NULL);
    ui_router_go(first, arg, LV_SCR_LOAD_ANIM_NONE);
    ESP_LOGI(TAG, "router up, screen %d", first);
}
