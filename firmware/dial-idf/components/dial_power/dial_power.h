#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Display power / standby manager: idle-driven backlight dimming with LEDC
 * hardware fades, plus the wake-consumes-first-input rule.
 *
 * Levels (day / night duty targets):
 *   ACTIVE  — full brightness, user interacting or recently active
 *   DIMMED  — legible-but-quiet after DIM_AFTER of no input
 *   STANDBY — near-dark after STANDBY_AFTER; the router should be showing
 *             the standby face by then (it follows the same idle clock)
 *
 * Wake rule: when the display is in STANDBY, the first touch or detent must
 * wake the screen and DO NOTHING ELSE (a 3am reach must not change the
 * temperature). Input handlers call dial_power_wake_consumes() first and
 * drop the event if it returns true.
 */

typedef enum { DPWR_ACTIVE, DPWR_DIMMED, DPWR_STANDBY } dial_power_level_t;

// Start the idle timer task. Requires dial_display + dial_state up.
void dial_power_start(void);

// Current level (for the router's standby-face decision).
dial_power_level_t dial_power_level(void);

// True exactly once when an input arrives during STANDBY: the input woke the
// screen and must be consumed (not acted on). Thread-safe, call from input
// handlers before processing.
bool dial_power_wake_consumes(void);

// Night mode (bedtime window from the sleep schedule): lower duty targets
// and a warm-dim standby. Also forwards to dial_haptics_set_night().
void dial_power_set_night(bool night);

/*
 * User-configurable day/night brightness (10..100%, dial_state's
 * "bri_day"/"bri_night" prefs). power_task is still the only context that
 * ever issues an LEDC fade — it reads the current percents via the
 * dial_state getters on every apply and scales the base DUTY_DAY/DUTY_NIGHT
 * tables itself. Everyone else just pokes the decision, same as the rest of
 * this file's concurrency model.
 */

// Call after a brightness pref changes (dial_state_set_bri_day_pct/
// set_bri_night_pct already persisted it) so the new value takes effect
// within one 100ms tick instead of waiting for an unrelated level change.
void dial_power_brightness_changed(void);

// Settings-screen live preview: while set, power_task fades to `pct` percent
// of the requested table's `level` duty instead of the normal level duty.
// `night` selects which base table (DAY/NIGHT) to preview; previewing NIGHT
// during the day is intentional (the user must be able to see what they're
// choosing). `level` selects which of that table's three duty entries the
// picker is actually editing — DPWR_ACTIVE for the Day/Night brightness
// pickers (the screen is by definition in front of the user while they're
// dialing this in, so ACTIVE is what they'd otherwise be looking at), and
// DPWR_STANDBY for the Night Clock picker (bri_night_clock_pct governs ONLY
// the night standby/screensaver duty, so previewing ACTIVE there would show
// the wrong — much brighter — glow). Call again on every knob detent to
// update the live value; each call also forces a reapply so the fade starts
// within one tick. dial_power_preview_end() clears the override (also
// forcing a reapply back to the normal level duty) — screens MUST call it
// when edit mode ends, including on teardown, or the override sticks
// forever.
// Percent -> duty for the NIGHT standby clock (0 = off, 100 = brightest),
// on a quadratic curve so the dim end where a bedside clock lives gets most
// of the range. Exposed because dial_state's migration inverts it to seed
// the pref at the duty this firmware used before the setting existed.
uint8_t dial_power_night_clock_duty(uint8_t pct);

void dial_power_preview(bool night, dial_power_level_t level, uint8_t pct);
void dial_power_preview_end(void);
