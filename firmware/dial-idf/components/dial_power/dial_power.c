#include "dial_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "dial_state.h"
#include "dial_haptics.h"

static const char *TAG = "power";

// Backlight is LEDC timer 3 / channel 1 (lcd_bl_pwm_bsp); we install the fade
// service and drive the same channel with hardware fades.
#define BL_MODE    LEDC_LOW_SPEED_MODE
#define BL_CHANNEL LEDC_CHANNEL_1

#define DIM_AFTER_US      (30LL * 1000000)   // 30s idle -> dimmed
#define STANDBY_AFTER_US  (90LL * 1000000)   // 90s idle -> standby
#define FADE_MS           400

// Duty targets (8-bit). Night values keep the room dark. These are the BASE
// tables at the user's brightness pref == 100%; the apply path below scales
// them by dial_state's bri_day/bri_night percent (10..100) at every tick, so
// these constants must stay exactly what a 100% preference has always meant.
// Exception: DUTY_NIGHT.standby (the screensaver clock at night) is scaled by
// its OWN pref, bri_night_clock_pct, not bri_night_pct — see power_task's
// apply path below for the split and dial_state.h for the migration that
// keeps existing devices' clock glow unchanged when this pref was introduced.
typedef struct { uint8_t active, dimmed, standby; } duty_set_t;
static const duty_set_t DUTY_DAY   = { 255, 90, 25 };
static const duty_set_t DUTY_NIGHT = { 140, 40, 6  };

// Floors applied AFTER scaling, so a 10% preference dims a lot without ever
// blacking the panel out or making the standby glow imperceptible.
#define FLOOR_ACTIVE   13
#define FLOOR_DIMMED   10
#define FLOOR_STANDBY   4

/*
 * The night clock gets its own percent -> duty curve instead of scaling
 * DUTY_NIGHT.standby, because scaling that base is useless: it is 6, and the
 * standby floor is 4, so the whole 0-100% range collapsed into duties 4-6 --
 * indistinguishable to the eye (owner-reported, and correct).
 *
 * Range is therefore explicit: 0 turns the clock OFF (a dark bedroom is a
 * legitimate choice; touch or a detent still wakes the dial to the night
 * ACTIVE duty), and 100 reaches NIGHT_CLOCK_MAX_DUTY, comfortably readable
 * across a room. The curve is quadratic because perceived brightness is not
 * linear in duty: the interesting decisions all live in the first few duty
 * steps, and a linear ramp would spend most of the knob's travel on
 * differences nobody can see. Squaring puts ~a third of the range below duty
 * 7, which is where a bedside clock at 3am actually lives.
 *
 * 30% lands on duty 6 -- exactly the fixed value this firmware used before
 * the setting existed, which is what dial_state's migration seeds to.
 */
#define NIGHT_CLOCK_MAX_DUTY 64

uint8_t dial_power_night_clock_duty(uint8_t pct)
{
    if (pct == 0) return 0;                 // off, deliberately
    if (pct > 100) pct = 100;
    uint32_t d = ((uint32_t)NIGHT_CLOCK_MAX_DUTY * pct * pct) / 10000;
    return d < 1 ? 1 : (uint8_t)d;          // any non-zero choice stays visible
}

/*
 * Day/Night percent -> duty. The slider reads 0-100, but 0 is NOT black: it
 * lands on the dimmest duty this firmware has ever produced for that tier,
 * which is what the old 10%-floored slider bottomed out at. That keeps the
 * end values identical (owner: "make it go to 0% instead of 10% but keep the
 * end values the same") while giving the whole knob travel to the range
 * people actually use -- and it keeps the in-use screen legible at 0, which
 * a literal 0 duty would not.
 *
 * min = the old floor for this tier: max(hard floor, base/10), i.e. exactly
 * what scale_duty(base, 10, floor) returned before. Interpolating from there
 * to base is EXACTLY appearance-preserving for the old 10-100 values once
 * they are rescaled onto 0-100 (dial_state's migration does that): both
 * curves are linear in percent through the same two endpoints.
 */
static inline uint8_t tier_duty(uint8_t base, uint8_t pct, uint8_t floor)
{
    uint32_t min = (uint32_t)base / 10;
    if (min < floor) min = floor;
    if (pct > 100) pct = 100;
    uint32_t v = min + ((uint32_t)(base - min) * pct) / 100;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/*
 * Concurrency model (deliberate, after review):
 *  - s_level (the DECIDED level) is guarded by a spinlock so it can be read
 *    and written from the knob decoder's esp_timer callback, the LVGL task's
 *    touch filter, and the worker — none of which may block on a mutex.
 *  - LEDC fades are issued ONLY by power_task. Everyone else just changes the
 *    decision; power_task notices (s_applied != decided, or a forced reapply)
 *    within one 100ms tick. A wake therefore starts its fade <=100ms after
 *    the input — imperceptible against the 400ms fade itself.
 */
static portMUX_TYPE s_lvl_spin = portMUX_INITIALIZER_UNLOCKED;
static dial_power_level_t s_level = DPWR_ACTIVE;

static volatile bool s_night;
static volatile bool s_force_reapply;   // set_night flips tables mid-level

// Settings-screen live preview override: -1 = none, else the exact duty to
// fade to instead of the normal level duty. Set only from dial_power_preview
// (LVGL task); read and applied only by power_task, same rule as s_level.
static volatile int s_preview_duty = -1;

static void fade_to(uint8_t duty)
{
    ledc_set_fade_with_time(BL_MODE, BL_CHANNEL, duty, FADE_MS);
    ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
}

static const duty_set_t *duties(void) { return s_night ? &DUTY_NIGHT : &DUTY_DAY; }

static dial_power_level_t level_get(void)
{
    taskENTER_CRITICAL(&s_lvl_spin);
    dial_power_level_t l = s_level;
    taskEXIT_CRITICAL(&s_lvl_spin);
    return l;
}

// Runs only in power_task: decide from idle time (unless someone already
// forced ACTIVE via wake), then apply the duty when it changed.
static void power_task(void *arg)
{
    (void)arg;
    dial_power_level_t applied = (dial_power_level_t)-1;
    for (;;) {
        int64_t idle = esp_timer_get_time() - dial_state_last_input_us();
        dial_power_level_t want =
            (idle >= STANDBY_AFTER_US) ? DPWR_STANDBY :
            (idle >= DIM_AFTER_US)     ? DPWR_DIMMED  : DPWR_ACTIVE;

        taskENTER_CRITICAL(&s_lvl_spin);
        s_level = want;
        taskEXIT_CRITICAL(&s_lvl_spin);

        if (want != applied || s_force_reapply) {
            s_force_reapply = false;
            int preview = s_preview_duty;
            if (preview >= 0) {
                // Settings screen is live-previewing a candidate value —
                // that duty wins outright, independent of the idle level.
                fade_to((uint8_t)preview);
            } else {
                const duty_set_t *d = duties();
                // ACTIVE/DIMMED always follow bri_day_pct/bri_night_pct. STANDBY
                // (the screensaver clock) is the one tier that forks by table:
                // day's standby duty still follows bri_day_pct (untouched by this
                // feature), but night's standby duty follows the separate
                // bri_night_clock_pct pref instead — that's the whole point of
                // the setting (a bedroom clock glow independent of in-use night
                // brightness). See dial_power_preview's header comment for the
                // matching preview-side split.
                uint8_t pct_live = s_night ? dial_state_get_bri_night_pct()
                                            : dial_state_get_bri_day_pct();
                uint8_t pct_standby = s_night ? dial_state_get_bri_night_clock_pct()
                                               : dial_state_get_bri_day_pct();
                uint8_t duty;
                switch (want) {
                case DPWR_ACTIVE:  duty = tier_duty(d->active,  pct_live,    FLOOR_ACTIVE);  break;
                case DPWR_DIMMED:  duty = tier_duty(d->dimmed,  pct_live,    FLOOR_DIMMED);  break;
                case DPWR_STANDBY:
                    duty = s_night ? dial_power_night_clock_duty(pct_standby)
                                   : tier_duty(d->standby, pct_standby, FLOOR_STANDBY);
                    break;
                default:           duty = d->active; break;
                }
                fade_to(duty);
            }
            applied = want;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void dial_power_start(void)
{
    ledc_fade_func_install(0);
    fade_to(duties()->active);
    xTaskCreate(power_task, "power", 2560, NULL, 2, NULL);
}

dial_power_level_t dial_power_level(void) { return level_get(); }

// Called from the knob esp_timer callback and the LVGL touch filter: must be
// lock-free-ish (spinlock only) and must NOT touch LEDC. Flipping the level
// here makes the wake decision immediately visible to both callers and the
// router; power_task issues the actual fade on its next tick (input was just
// stamped, so it computes ACTIVE too).
bool dial_power_wake_consumes(void)
{
    bool consumed = false;
    taskENTER_CRITICAL(&s_lvl_spin);
    if (s_level == DPWR_STANDBY) {
        s_level = DPWR_ACTIVE;
        consumed = true;
    }
    taskEXIT_CRITICAL(&s_lvl_spin);
    return consumed;
}

void dial_power_set_night(bool night)
{
    if (s_night == night) return;
    s_night = night;
    dial_haptics_set_night(night);
    s_force_reapply = true;   // power_task re-fades with the new duty table
    ESP_LOGI(TAG, "night mode %s", night ? "on" : "off");
}

// Called (from the LVGL task) after dial_state_set_bri_day_pct/
// set_bri_night_pct already persisted the new preference. Just a decision
// flip, like every other setter in this file — power_task issues the actual
// fade on its next 100ms tick.
void dial_power_brightness_changed(void)
{
    s_force_reapply = true;
}

void dial_power_preview(bool night, dial_power_level_t level, uint8_t pct)
{
    const duty_set_t *base = night ? &DUTY_NIGHT : &DUTY_DAY;
    uint8_t duty;
    switch (level) {
    case DPWR_DIMMED:  duty = tier_duty(base->dimmed,  pct, FLOOR_DIMMED);  break;
    case DPWR_STANDBY:
        // Same curve power_task applies, or the preview would be showing a
        // brightness the setting cannot actually produce.
        duty = night ? dial_power_night_clock_duty(pct)
                     : tier_duty(base->standby, pct, FLOOR_STANDBY);
        break;
    case DPWR_ACTIVE:
    default:           duty = tier_duty(base->active,  pct, FLOOR_ACTIVE);  break;
    }
    s_preview_duty = duty;
    s_force_reapply = true;
}

void dial_power_preview_end(void)
{
    s_preview_duty = -1;
    s_force_reapply = true;
}
