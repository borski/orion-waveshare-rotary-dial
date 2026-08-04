# Spec: update prompt + auto-update

Status: **APPROVED AND IN BUILD (2026-07-29).** Owner approved implementation
after v1.2.0-beta.2 was verified on hardware; targets the next beta.
(Earlier status line said "not built" — that was written while v1.2.0 was
still being cut and is superseded.)

## Problem

An update can sit available indefinitely with nothing telling the user. Today
discovery is entirely passive: a version badge on the menu's Update row, which
you only see if you go looking. The counter-pressure is that this is a
**bedside device** — an unsolicited prompt when someone reaches for the dial at
3am to cool their side of the bed is worse than never shipping the update at
all. So the design problem is not "how do we notify" but "how do we notify
without ever interrupting the thing the device exists for."

Auto-update resolves it more completely than any prompt can: a dial that keeps
itself current never has to ask. The prompt then exists mainly to offer that
choice once, rather than to nag repeatedly.

## Shape

Four parts:

1. An **ambient indicator** on the dial and standby faces — always-on,
   unconditional discoverability.
2. A dismissible **update prompt** shown at most once a day, never at night.
3. An **auto-update** setting (Off / Overnight) that, once on, makes the prompt
   moot.
4. The existing **Update screen** grows the controls the prompt hands off to.

### 1. The ambient indicator (added 2026-07-29)

"Update available", small and `ink_secondary`, on both `scr_dial.c` (the
dead patch below the power disc that also carries the M6 "Finalizing
update..." pending-verify caption — the two are mutually exclusive in
practice, `pending_verify` wins the rare tick both are true) and
`scr_standby.c` (same slot, otherwise empty on that face). Shown whenever
`ota.status == OTA_AVAILABLE` and it is not night — no other conditions: no
idle window, no once-a-day ceiling, no interaction with `ota_skip`/
`ota_defer`. Those govern the sheet below, not this; a user who tapped
"Later" on the sheet still benefits from knowing an update exists. Hidden
entirely (not dimmed) at night — a bedside device must not advertise
anything at 3am.

Tappable on the dial face (opens `SCR_UPDATE`) — small text, so it carries
an `ext_click_area` the way the setpoint handle does, capped short of this
project's usual >=72px floor because the slot sits directly under the power
disc's own hit box. NOT tappable on the standby face: that screen's entire
surface is already a single wake target, and the wake-consumes-first-input
rule owns it.

### 2. The prompt

A sheet over the dial face (this project's bottom-sheet idiom — slides up
over the dial face, dismissed by a down swipe, already a screen the router
shows over the dial), NOT a hard modal that blocks the temperature control
underneath any longer than a tap.

Copy:

```
Update available
1.2.0

Takes about 2 minutes. The dial
won't respond while it installs.

[ Update now ]     <- primary, >=88px
[ Later ]          <- secondary, >=72px
  Update options   <- text row -> SCR_UPDATE
```

The install-cost line is not optional. Without it, someone taps "Update now" at
bedtime and discovers their bed is unadjustable for two minutes.

Actions:

- **Update now** — posts `CMD_OTA_APPLY`, exactly as the Update screen's
  confirmed install does; `SCR_UPDATING` takes over as it already does today.
- **Later** — sets a defer timestamp of **now + 23 hours** and dismisses.
  23, not 24, deliberately: a 24-hour defer pins the prompt to the same moment
  every day, so a bad time stays a bad time forever. 23 walks it around the
  clock until it lands somewhere the user is receptive.
- **Update options** — navigates to `SCR_UPDATE` (Settings > Update). This is
  where "stop asking" lives, so the sheet itself stays to three choices.

Dismissing by swipe behaves as **Later**.

#### When the prompt may appear (reworked 2026-07-29)

**Revision history:** the original design (below, superseded) gated the
sheet on an idle window — `DPWR_ACTIVE` plus 10-30s of no input. That window
only exists between "the display has settled" and "the display is about to
dim", i.e. after the user has typically already walked away, and the
gate was re-evaluated continuously, so a touch (which resets idle time)
withdrew the sheet mid-reach. It also stamped the once-per-24h `ota_shown`
ceiling on that same empty-room raise, so the realistic outcome was: sheet
raises into an empty room, dims away 20s later, and can't return for a day —
repeating forever. The rework separates "knowing an update exists" (the
ambient indicator on `scr_dial.c`/`scr_standby.c` — unconditional on
`ota.status == OTA_AVAILABLE` and not-night, no other gates) from "offering
one-tap install" (this sheet, below).

The sheet is now raised on a **wake edge** — `dial_power_level()` observed
going from `DPWR_STANDBY`/`DPWR_DIMMED` to `DPWR_ACTIVE` (never on the
initial boot transition) — checked once, at that instant, against:

| Entry gate | Reason |
|---|---|
| `ota.status == OTA_AVAILABLE` | nothing to offer otherwise |
| Not the skipped version | see `ota_skip` below |
| `now >= ota_defer_until` | honors "Later" |
| Not prompted in the last 24h | independent ceiling on frequency |
| **Not in the sleep window** | the single most important rule; reuse the same night flag `dial_power_set_night()` is driven from |
| `phase == PH_READY` and `have_state` | never nag a dial that is already failing |
| `dial_time_valid()` | 23h/24h logic needs a real clock |
| Auto-update is Off | if it's on, the dial will handle it; don't ask |

No idle-window gate — a wake edge IS the "a human is provably present"
signal the idle window used to approximate.

Once raised, the sheet is sticky (it is NOT re-evaluated against the table
above on every tick — that re-evaluation was the mid-reach-withdrawal bug).
It stays until the user acts (Update now / Later / Update options /
swipe-dismiss, `scr_update_prompt.c`), or one of three **separate exit
checks** fires: the display made it all the way back to `DPWR_STANDBY`,
night began, or `ota.status` stopped being `OTA_AVAILABLE`. A touch is
deliberately not one of them.

Evaluated in the worker's idle poll path, committed as a state flag the router
reads — same pattern as every other screen decision.

### 3. Auto-update

Setting on `SCR_UPDATE`: **Auto-update — Off / Overnight**.

- **Default Off** for existing devices (consent: a device that silently
  reboots itself is a surprise to someone who never asked for it). Fresh
  installs also default Off, but the prompt's "Update options" hand-off is
  what makes it discoverable — the first prompt is effectively the opt-in
  moment.
- **Overnight window**: the dial already knows the sleep schedule. Run the
  install in the quiet stretch **after wake**: `[wakeup + 60min, wakeup +
  180min]` local. Fall back to `[09:00, 11:00]` when no schedule is available.
  Additional conditions: no user input for >=30 min, `PH_READY`, not currently
  in the sleep window, and no thermal relief active on either zone (a timed
  boost with a live countdown on screen — genuinely disruptive to interrupt
  with the install takeover; temporary by construction, so unlike a bed-on
  check it can never permanently disqualify anyone). Deliberately NOT gated
  on the zones being on/off (2026-07-29 fix) — the dial is a remote control,
  not the bed's controller, so its ~30s reboot to apply an install has zero
  effect on the bed's own operation; a zones-off requirement protected
  against nothing and silently, permanently starved auto-update for anyone
  who runs their topper through the day.
- Install proceeds exactly like a manual one (`dial_ota_download_and_apply`),
  including the takeover screen if someone happens to walk up mid-install, and
  the confirm-on-stable-boot behavior added in v1.0.10 that keeps a
  power-cycle from silently rolling it back.
- **Failure**: retry the next day. After **two failed installs of the same
  version**, stop auto-attempting it and surface the error on `SCR_UPDATE`
  (the row already has an error line). This prevents a bad release from
  putting a dial into a nightly reboot loop.
- Beta channel interacts cleanly: if Beta builds is on, auto-update tracks
  prereleases too. That is the intended combination for a test dial.

### 4. Update screen additions

`SCR_UPDATE` becomes the single place update behavior is controlled:

```
< Back
Check for updates      <status / tap to check / tap-twice to install>
Auto-update            Off | Overnight
Skip this version      <only while an update is available>
Beta builds            On | Off
```

**Skip this version** stores the exact version string; anything newer prompts
again. This is the "don't show it again" of the original sketch, moved off the
sheet so the sheet stays simple and the destructive-ish choice is made
somewhere with room to explain it.

## Persistence

All in the existing NVS namespace `"ui"`, following the `beta` / `sched_follow`
pattern (field in `app_state_t`, seeded in `dial_state_init`, restored in
`dial_state_restore_prefs`, immediate-commit setter):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ota_auto` | u8 | 0 | 0 = Off, 1 = Overnight |
| `ota_defer` | u32 | 0 | epoch seconds; prompt suppressed until then ("Later") |
| `ota_skip` | str | "" | version string the user chose to skip |
| `ota_shown` | u32 | 0 | epoch of last prompt; enforces the once-per-day ceiling |

`ota_defer` / `ota_shown` are wall-clock epochs, so they survive reboots
correctly — a device that reboots hourly must not get a fresh prompt each time.

## Explicitly out of scope

- Any prompt during the sleep window, under any condition.
- Forced updates or a countdown the user cannot dismiss.
- Auto-update defaulting to On without an explicit choice.
- Release notes on the device (no room, and the text would be stale the moment
  it shipped) — the version number plus the GitHub release is enough.

## Test plan

Testable without waiting for a real release:

1. Publish a `-beta.N` prerelease, enable Beta builds on the test dial, and
   let it discover the update — exercises the real availability path.
2. Verify the prompt does NOT appear inside the sleep window (temporarily set
   a bedtime that covers now, via the Orion app).
3. Verify "Later" suppresses for 23h across a reboot.
4. Verify "Skip this version" suppresses that version, and that publishing a
   newer prerelease prompts again.
5. Turn on Auto-update, set a wakeup time so the window is imminent, and
   confirm the install runs unattended and confirms itself after reboot.
