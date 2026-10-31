#pragma once
#include <Arduino.h>

// =====================================================================
//  Pin map - ESP32-WROOM-32 DevKit (see README "Pin map")
// =====================================================================

// RC522 on VSPI (3.3 V only)
constexpr uint8_t PIN_RC522_SCK  = 18;
constexpr uint8_t PIN_RC522_MISO = 19;
constexpr uint8_t PIN_RC522_MOSI = 23;
constexpr uint8_t PIN_RC522_SS   = 5;    // "SDA" on the RC522 board
constexpr uint8_t PIN_RC522_RST  = 22;

constexpr uint8_t PIN_LOCK_DRIVE    = 25;  // Rg 220R -> IRF8736 gate (option B) or R1 -> Q1 base (options A/C)
constexpr uint8_t PIN_BUZZER        = 26;  // to NPN base, active buzzer
constexpr uint8_t PIN_DOOR_SENSE    = 27;  // door sensor NO (magnetic, in the lock), COM -> GND
constexpr uint8_t PIN_EXIT_BUTTON   = 32;  // inside exit push button -> GND
constexpr uint8_t PIN_ENROLL_BUTTON = 33;  // enroll push button -> GND (mount inside!)
constexpr uint8_t PIN_DOOR_ALIGN    = 4;   // optional door-aligned microswitch -> GND (DOOR_ALIGN_SENSOR_ENABLED)
constexpr uint8_t PIN_STATUS_LED    = 2;   // on-board LED

// =====================================================================
//  Lock driver topology (see README "Lock driver")
// =====================================================================
//  0 = Option B (current hardware): IRF8736 logic-level N-MOSFET, low side,
//      gate driven directly: GPIO25 -> 220R -> gate, 10k gate pull-up to 3.3 V.
//      GPIO HIGH or Hi-Z (reset, boot, flashing) -> lock powered -> LOCKED.
//      GPIO LOW -> lock unpowered -> OPEN.
//  1 = GPIO HIGH -> OPEN, GPIO LOW / Hi-Z -> LOCKED.
//      Option A: N-MOSFET (IRFZ44N, ...) low side + one NPN and 12 V gate pull-up.
//      Option C: IRF4905 P-channel high side + two NPN stages.
#define LOCK_DRIVER_NPN_INVERTER 0

constexpr uint8_t LOCK_LEVEL_LOCKED   = LOCK_DRIVER_NPN_INVERTER ? LOW : HIGH;
constexpr uint8_t LOCK_LEVEL_UNLOCKED = LOCK_DRIVER_NPN_INVERTER ? HIGH : LOW;

// =====================================================================
//  Door sensor (COM/NO magnetic contact built into the lock)
// =====================================================================
// true  -> NO contact is CLOSED when the door is closed (magnet aligned).
// false -> NO contact is CLOSED when the door is open.
// Verify with the Serial log ("[DOOR] ...") and flip if needed.
#define DOOR_SENSE_CLOSED_IS_DOOR_CLOSED true

// Relock when the door closes after being opened during an unlock window,
// instead of waiting for the rest of the unlock time (anti-tailgating).
#define RELOCK_ON_DOOR_CLOSE 1

// The powered lock drops the bolt as soon as it sees the door magnet, so the
// lock is never powered while the door is open or still settling: it is powered
// only after the door has stayed closed for RELOCK_CLOSE_DELAY_MS (door rebound /
// slam). Applies to every relock (door close, unlock time expiry, boot).
// Reopening within the delay restarts it on the next close.
constexpr uint32_t RELOCK_CLOSE_DELAY_MS = 1000;

// =====================================================================
//  Door-aligned switch (optional, for sliding doors)
// =====================================================================
// The lock's magnetic sensor also reports "closed" when a sliding door stops a
// few cm short, and a powered lock then drops the bolt onto the strike body.
// 1 -> a microswitch at the door stop, pressed only when the door is fully home,
//      must also be closed (and settled for RELOCK_CLOSE_DELAY_MS) before the lock
//      is powered. Once powered, the lock ignores this switch.
// 0 -> switch not fitted or ignored.
// Fit and wire the switch before setting 1: an open switch keeps the lock unpowered.
#define DOOR_ALIGN_SENSOR_ENABLED 1

// true  -> switch contact CLOSED (GPIO LOW) when the door is fully home (NO contact, pressed).
// false -> switch contact OPEN when the door is fully home (NC contact).
#define DOOR_ALIGN_CLOSED_IS_ALIGNED true

// Door-left-open alarm: door still open (or, with the align switch, not fully
// home while lock power is deferred) DOOR_OPEN_ALARM_MS after relock.
#define DOOR_OPEN_ALARM_ENABLED 1

// Forced-door alarm: door opens while locked and the bolt has had time to drop.
#define DOOR_FORCED_ALARM_ENABLED 1

// =====================================================================
//  Lock relock-delay selector (0 s / 3 s / 6 s switch on the lock body)
// =====================================================================
// Set to match the selector position. With power applied and the door closed,
// the bolt drops only after this delay. The forced-door alarm is armed only
// after the door has been closed and locked for LOCK_HW_DELAY_MS + BOLT_SETTLE_MS.
constexpr uint32_t LOCK_HW_DELAY_MS        = 0;
constexpr uint32_t BOLT_SETTLE_MS          = 1000;

// =====================================================================
//  Timing / behaviour
// =====================================================================
constexpr uint32_t DEFAULT_UNLOCK_TIME_MS = 10000;  // changeable at runtime: "time <ms>"
constexpr uint32_t UNLOCK_TIME_MIN_MS     = 500;
constexpr uint32_t UNLOCK_TIME_MAX_MS     = 60000;

constexpr uint32_t ENROLL_TIMEOUT_MS      = 15000;  // enroll mode auto-exit (refreshed per card)
constexpr uint32_t WIPE_HOLD_MS           = 10000;  // hold enroll button -> erase all cards
constexpr uint32_t WIPE_TICK_START_MS     = 2000;   // start 1 Hz ticks while holding

constexpr uint32_t BUTTON_DEBOUNCE_MS     = 30;
constexpr uint32_t SENSE_DEBOUNCE_MS      = 50;

constexpr uint32_t CARD_REPEAT_GUARD_MS   = 1500;   // ignore the same UID within this window
constexpr uint8_t  MAX_DENIED_ATTEMPTS    = 5;      // consecutive unknown cards before lockout
constexpr uint32_t DENIED_LOCKOUT_MS      = 30000;  // reader ignored during lockout

constexpr uint32_t DOOR_OPEN_ALARM_MS     = 15000;  // time to pass through and close the door
constexpr uint32_t ALARM_REPEAT_MS        = 3000;

constexpr uint32_t RC522_CHECK_INTERVAL_MS = 5000;  // re-init the reader if it stops answering or loses its config

// =====================================================================
//  Card storage (NVS via Preferences)
// =====================================================================
constexpr uint8_t MAX_CARDS     = 50;
constexpr char    NVS_NAMESPACE[] = "doorlock";
