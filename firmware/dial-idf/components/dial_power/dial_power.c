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
typedef struct { uint8_t active, dimmed, standby; } duty_set_t;
static const duty_set_t DUTY_DAY   = { 255, 90, 25 };
static const duty_set_t DUTY_NIGHT = { 140, 40, 6  };

// Floors applied AFTER scaling, so a 10% preference dims a lot without ever
// blacking the panel out or making the standby glow imperceptible.
#define FLOOR_ACTIVE   13
#define FLOOR_DIMMED   10
#define FLOOR_STANDBY   4

static inline uint8_t scale_duty(uint8_t base, uint8_t pct, uint8_t floor)
{
    uint32_t v = ((uint32_t)base * pct) / 100;
    if (v < floor) v = floor;
    if (v > 255)   v = 255;
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
                uint8_t pct = s_night ? dial_state_get_bri_night_pct()
                                       : dial_state_get_bri_day_pct();
                uint8_t duty;
                switch (want) {
                case DPWR_ACTIVE:  duty = scale_duty(d->active,  pct, FLOOR_ACTIVE);  break;
                case DPWR_DIMMED:  duty = scale_duty(d->dimmed,  pct, FLOOR_DIMMED);  break;
                case DPWR_STANDBY: duty = scale_duty(d->standby, pct, FLOOR_STANDBY); break;
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

void dial_power_preview(bool night, uint8_t pct)
{
    const duty_set_t *base = night ? &DUTY_NIGHT : &DUTY_DAY;
    s_preview_duty = scale_duty(base->active, pct, FLOOR_ACTIVE);
    s_force_reapply = true;
}

void dial_power_preview_end(void)
{
    s_preview_duty = -1;
    s_force_reapply = true;
}
