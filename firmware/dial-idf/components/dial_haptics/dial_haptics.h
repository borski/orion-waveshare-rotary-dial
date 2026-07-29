#pragma once
#include <stdbool.h>

/*
 * DRV2605 LRA haptics. HAPTIC_EN is hardwired to 3V3 and TRIG to GND on this
 * board, so the GO-bit over I2C is the only trigger path.
 *
 * Effects fire from a small dedicated task via a queue: callers never block
 * on the I2C bus (the knob decoder and LVGL task must stay fast), and a new
 * effect simply replaces one still playing (never queue haptics — lagging
 * the knob feels broken).
 */

typedef enum {
    HAPTIC_TICK,      // one knob detent — soft, short
    HAPTIC_STOP,      // range end — firmer double
    HAPTIC_CONFIRM,   // action committed
    HAPTIC_ERROR,     // rejected / failed
} haptic_effect_t;

/*
 * Feedback level (Settings' "Haptics" row — cycles
 * Off -> Low -> High -> Auto -> Off). Two underlying intensity profiles:
 *   FIRM = the .day effect variants + the full overdrive clamp.
 *   SOFT = the .night effect variants + a reduced overdrive clamp.
 * LOW and HIGH are the user's explicit, time-of-day-independent choice
 * (HIGH really does mean firm at 3am, if that's what was picked); AUTO is
 * the adaptive one, following the sleep window via dial_haptics_set_night().
 *
 * Numeric values are deliberately the pre-existing NVS "haptics" byte's
 * values (0/1, an On/Off bool) extended, NOT a fresh 0..3 enumeration — see
 * dial_state.h's app_state_t.haptics_level. Every device in the field
 * already stores 0 or 1; 1 ("on") already meant "the adaptive day/night
 * behavior", so it maps to AUTO with no migration and no surprised users.
 */
typedef enum {
    HAPTIC_LEVEL_OFF  = 0,   // nothing plays
    // 1 is LOW, and deliberately so: every device that ever ran the old
    // on/off firmware has a stored 1 meaning "haptics on", and LOW is the
    // level this dial should feel like (owner, 2026-07-29: High is too much
    // and Auto is High for half the day). Numbering it here means those
    // devices land on the right feel with no migration code, while anyone
    // who deliberately picks Auto stores 2 and keeps it.
    HAPTIC_LEVEL_LOW  = 1,   // SOFT always, regardless of time of day
    HAPTIC_LEVEL_AUTO = 2,   // FIRM during the day, SOFT during the sleep
                             // window — today's pre-M7 "enabled" behavior,
                             // unchanged, and the default
    HAPTIC_LEVEL_HIGH = 3,   // FIRM always, regardless of time of day
} haptic_level_t;

// Probe + configure the chip (LRA mode, library 6, auto-calibration persisted
// in NVS). Requires i2c_master_Init() to have run. Safe to call when the chip
// is absent — haptics just stay disabled.
void dial_haptics_init(void);

// Mute/unmute playback for a scope. The screen router wraps knob dispatch in
// this: the encoder has real mechanical detents you can feel, and a motor
// pulse on top of every one of them is too much (owner, 2026-07-29). Muting
// at the dispatch boundary rather than deleting ~40 call sites keeps each
// screen's tap/confirm/drag feedback intact -- those have no mechanical
// counterpart -- and cannot be missed by a screen added later.
void dial_haptics_mute(bool muted);

// Range-end feedback: always the SOFT variant, whatever the user's haptics
// level is, and audible through dial_haptics_mute() (the router mutes knob
// dispatch, and this is the one knob-time pulse worth keeping -- hitting a
// limit is information the encoder's detents cannot convey, since it feels
// identical to every other click). Still silent at HAPTIC_LEVEL_OFF: "off"
// has to mean off. Owner, 2026-07-29: "except maybe when it hits an end
// state I would be ok with a haptic at low ... should not change with the
// haptics settings".
void dial_haptics_play_soft(haptic_effect_t fx);

// Fire an effect (non-blocking; drops if level is OFF, muted, or chip absent).
void dial_haptics_play(haptic_effect_t fx);

// Night attenuation: true = the sleep window is active (owner asleep
// nearby). Only actually changes anything at HAPTIC_LEVEL_AUTO — LOW/HIGH
// are global choices the sleep window must not perturb, and OFF plays
// nothing either way. Still safe (and expected) to call unconditionally on
// every transition regardless of the current level.
void dial_haptics_set_night(bool night);

// Feedback level (settings toggle; persisted by the caller). Also rewrites
// REG_OD_CLAMP immediately for the new level+night combination (a plain
// register write — the chip stays in internal-trigger mode, so this never
// re-runs the audible auto-calibration).
void dial_haptics_set_level(haptic_level_t level);
