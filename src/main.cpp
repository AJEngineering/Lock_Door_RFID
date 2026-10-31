// RFID door lock controller
// ESP32-WROOM-32 + RC522 + 12 V fail-safe lock (powered = locked, unpowered = open)
//
// Features:
//  - Unlock by stored RFID card or inside exit button, auto-relock after unlock time
//  - Enroll button: short press -> enroll mode (new card = add, known card = remove)
//                   hold 10 s   -> erase all cards
//  - Card UIDs stored in NVS (Preferences), survive power loss
//  - Door sensor: relock on door close, door-left-open and forced-door alarms
//  - Optional door-aligned switch (sliding doors): lock powered only when the door is fully home
//  - Brute-force lockout after repeated unknown cards
//  - RC522 health check (SPI, antenna, config registers) and automatic re-init
//  - Serial console: help, list, open, enroll, clear, time <ms>

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>
#include "config.h"

static const char FW_VERSION[] = "1.2.1";

// =====================================================================
//  Debounced active-low input (INPUT_PULLUP, pressed/closed = LOW)
// =====================================================================
class DebouncedInput {
public:
  void begin(uint8_t pin, uint32_t debounceMs) {
    pin_ = pin;
    debounceMs_ = debounceMs;
    pinMode(pin_, INPUT_PULLUP);
    delay(2);
    raw_ = stable_ = digitalRead(pin_);
    lastChange_ = stableSince_ = millis();
  }

  // Returns true once when the debounced state changes.
  bool update() {
    const int r = digitalRead(pin_);
    const uint32_t now = millis();
    if (r != raw_) {
      raw_ = r;
      lastChange_ = now;
    }
    if (raw_ != stable_ && (now - lastChange_) >= debounceMs_) {
      stable_ = raw_;
      stableSince_ = now;
      return true;
    }
    return false;
  }

  bool isActive() const { return stable_ == LOW; }
  uint32_t stableFor() const { return millis() - stableSince_; }

private:
  uint8_t pin_ = 0;
  uint32_t debounceMs_ = 30;
  int raw_ = HIGH;
  int stable_ = HIGH;
  uint32_t lastChange_ = 0;
  uint32_t stableSince_ = 0;
};

// =====================================================================
//  Non-blocking buzzer pattern player (active buzzer)
//  Pattern = ON, OFF, ON, OFF ... durations in ms, terminated by 0
// =====================================================================
namespace Pattern {
  const uint16_t BOOT[]         = {60, 60, 60, 0};
  const uint16_t GRANTED[]      = {120, 0};
  const uint16_t DENIED[]       = {700, 0};
  const uint16_t ENROLL_ENTER[] = {80, 80, 80, 80, 80, 0};
  const uint16_t ENROLL_EXIT[]  = {250, 0};
  const uint16_t CARD_ADDED[]   = {80, 60, 400, 0};
  const uint16_t CARD_REMOVED[] = {400, 60, 80, 0};
  const uint16_t STORE_FULL[]   = {80, 60, 80, 60, 80, 60, 80, 0};
  const uint16_t WIPED[]        = {1200, 0};
  const uint16_t TICK[]         = {25, 0};
  const uint16_t ALARM[]        = {150, 100, 150, 100, 150, 0};
  const uint16_t FORCED[]       = {600, 150, 600, 150, 600, 150, 600, 0};
  const uint16_t LOCKOUT[]      = {300, 150, 300, 150, 300, 0};
}

class Buzzer {
public:
  void begin(uint8_t pin) {
    pin_ = pin;
    digitalWrite(pin_, LOW);
    pinMode(pin_, OUTPUT);
  }

  void play(const uint16_t *pattern) {
    pattern_ = pattern;
    idx_ = 0;
    startStep();
  }

  void update() {
    if (!pattern_) return;
    if (millis() - stepStart_ >= pattern_[idx_]) {
      idx_++;
      startStep();
    }
  }

private:
  void startStep() {
    if (pattern_[idx_] == 0) {
      pattern_ = nullptr;
      digitalWrite(pin_, LOW);
      return;
    }
    digitalWrite(pin_, (idx_ % 2 == 0) ? HIGH : LOW);
    stepStart_ = millis();
  }

  uint8_t pin_ = 0;
  const uint16_t *pattern_ = nullptr;
  uint8_t idx_ = 0;
  uint32_t stepStart_ = 0;
};

// =====================================================================
//  Card storage in NVS
// =====================================================================
struct CardUid {
  uint8_t len;
  uint8_t b[10];
};

static bool uidEquals(const CardUid &a, const CardUid &b) {
  return a.len == b.len && memcmp(a.b, b.b, a.len) == 0;
}

static void uidToString(const CardUid &uid, char *out, size_t outLen) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < uid.len && pos + 3 < outLen; i++) {
    pos += snprintf(out + pos, outLen - pos, i ? ":%02X" : "%02X", uid.b[i]);
  }
}

// The list is one NVS blob and the count is its length / sizeof(CardUid). An NVS
// blob write is atomic (a power cut keeps either the old or the new list), so no
// separate count key can disagree with it. RAM is changed only after the new list
// is in NVS, so a failed write leaves both unchanged.
class CardStore {
public:
  bool begin() {
    if (!prefs_.begin(NVS_NAMESPACE, false)) return false;
    ready_ = true;
    count_ = 0;
    // v1.0 format: separate "count" key. Add/remove wrote "cards" first, so the blob
    // is the newest state; erase wrote count = 0 first, then removed "cards". A power
    // cut in between left the erased list in NVS: honour count = 0 so it stays erased.
    if (prefs_.isKey("count")) {
      if (prefs_.getUChar("count", 0) == 0) {
        legacyErasePending_ = true;
        if (!finishLegacyErase()) {
          Serial.println("[NVS] interrupted v1.0 erase could not be completed, starting empty");
          return true;
        }
      } else {
        prefs_.remove("count");  // non-zero count is never acted on; retried next boot if this fails
      }
    }
    const size_t len = prefs_.getBytesLength("cards");
    if (len % sizeof(CardUid) != 0 || len > sizeof(cards_)) {
      Serial.println("[NVS] card blob corrupt, starting empty");
    } else if (len && prefs_.getBytes("cards", cards_, len) == len) {
      count_ = len / sizeof(CardUid);
    }
    return true;
  }

  uint8_t count() const { return count_; }
  const CardUid &at(uint8_t i) const { return cards_[i]; }

  int find(const CardUid &uid) const {
    for (uint8_t i = 0; i < count_; i++) {
      if (uidEquals(cards_[i], uid)) return i;
    }
    return -1;
  }

  bool add(const CardUid &uid) {
    if (count_ >= MAX_CARDS) return false;
    cards_[count_] = uid;  // slot past count_: not accepted until committed
    return commit(cards_, count_ + 1);
  }

  bool removeAt(uint8_t idx) {
    if (idx >= count_) return false;
    CardUid next[MAX_CARDS];
    memcpy(next, cards_, count_ * sizeof(CardUid));
    next[idx] = next[count_ - 1];  // order is not significant
    return commit(next, count_ - 1);
  }

  bool clear() { return commit(cards_, 0); }

  uint32_t loadUnlockTime() {
    if (!ready_) return DEFAULT_UNLOCK_TIME_MS;  // NVS not open: nothing to read
    uint32_t t = prefs_.getULong("unlock_ms", DEFAULT_UNLOCK_TIME_MS);
    return constrain(t, UNLOCK_TIME_MIN_MS, UNLOCK_TIME_MAX_MS);
  }

  bool saveUnlockTime(uint32_t ms) { return ready_ && prefs_.putULong("unlock_ms", ms) == sizeof(uint32_t); }

private:
  // Finish an interrupted v1.0 erase ("count" = 0 still in NVS): remove the stale
  // list, then the key. While this is pending "cards" can only hold the stale list,
  // because commit() writes nothing until it succeeds.
  bool finishLegacyErase() {
    if (!legacyErasePending_) return true;
    if (prefs_.isKey("cards") && !prefs_.remove("cards")) return false;
    if (prefs_.isKey("count") && !prefs_.remove("count")) return false;
    legacyErasePending_ = false;
    return true;
  }

  // Write the new list to NVS first, then make it the live list. A pending v1.0
  // erase is completed before the write, so a later boot cannot apply it to the new list.
  bool commit(const CardUid *list, uint8_t n) {
    if (!ready_) {  // isKey()/remove() report "absent"/fail without an open namespace
      Serial.println("[NVS] not initialised, card list unchanged");
      return false;
    }
    bool ok = finishLegacyErase();
    if (ok && n == 0) {
      ok = !prefs_.isKey("cards") || prefs_.remove("cards");
    } else if (ok) {
      const size_t len = n * sizeof(CardUid);
      ok = prefs_.putBytes("cards", list, len) == len;
    }
    if (!ok) {
      Serial.println("[NVS] write failed, card list unchanged");
      return false;
    }
    if (list != cards_) memcpy(cards_, list, n * sizeof(CardUid));
    count_ = n;
    return true;
  }

  Preferences prefs_;
  CardUid cards_[MAX_CARDS];
  uint8_t count_ = 0;
  bool ready_ = false;              // NVS namespace opened by begin()
  bool legacyErasePending_ = false;  // v1.0 "count" = 0 still in NVS
};

// =====================================================================
//  Globals
// =====================================================================
static MFRC522 rfid(PIN_RC522_SS, PIN_RC522_RST);
static CardStore store;
static Buzzer buzzer;
static DebouncedInput exitBtn, enrollBtn, doorSense, doorAlign;

static uint32_t unlockTimeMs = DEFAULT_UNLOCK_TIME_MS;

static bool     doorUnlocked     = false;
static uint32_t unlockStart      = 0;
static uint32_t lockCommandedAt  = 0;
static bool     doorWasOpened    = false;  // door opened during the current unlock window
static uint32_t doorClosedAt     = 0;      // last open -> closed transition
static uint32_t doorHomeChangedAt = 0;     // last change of doorHome()
static bool     lockPowerPending = false;  // locked, lock power deferred until the door settles

static bool     enrollMode       = false;
static uint32_t enrollStart      = 0;
static bool     wipeFired        = false;
static uint32_t lastWipeTick     = 0;

static CardUid  lastUid          = {0, {0}};
static uint32_t lastUidAt        = 0;
static uint8_t  deniedCount      = 0;
static bool     lockoutActive    = false;
static uint32_t lockoutStart     = 0;

static bool     doorOpenAlarm    = false;
static bool     forcedAlarm      = false;
static uint32_t lastAlarmAt      = 0;
static bool     lastSecured      = true;
static uint32_t securedChangedAt = 0;
static uint32_t lastRfidCheck    = 0;
static byte     lastSak          = 0;      // SAK of the last card read (card type for the log)
static uint32_t lastReadFailLog  = 0;

// =====================================================================
//  Lock control
// =====================================================================
static void driveLock(bool locked) {
  digitalWrite(PIN_LOCK_DRIVE, locked ? LOCK_LEVEL_LOCKED : LOCK_LEVEL_UNLOCKED);
}

static bool doorClosed() {
  return DOOR_SENSE_CLOSED_IS_DOOR_CLOSED ? doorSense.isActive() : !doorSense.isActive();
}

static bool doorAligned() {
#if DOOR_ALIGN_SENSOR_ENABLED
  return DOOR_ALIGN_CLOSED_IS_ALIGNED ? doorAlign.isActive() : !doorAlign.isActive();
#else
  return true;
#endif
}

// Door closed (lock's magnetic sensor) and fully home (align switch, if enabled).
static bool doorHome() {
  return doorClosed() && doorAligned();
}

// Door home and settled: safe to power the lock (the bolt drops at once).
static bool doorSettled() {
  return doorHome() && millis() - doorHomeChangedAt >= RELOCK_CLOSE_DELAY_MS;
}

// Door in its locked position. While lock power is deferred it must be fully home;
// once the lock is powered the bolt holds the door and the align switch is ignored,
// so play between bolt and strike does not raise the door-left-open alarm.
static bool doorSecured() {
  return lockPowerPending ? doorHome() : doorClosed();
}

static void clearAlarms() {
  if (doorOpenAlarm || forcedAlarm) Serial.println("[ALARM] cleared");
  doorOpenAlarm = false;
  forcedAlarm = false;
}

// True when the lock has been powered and the door closed long enough for the bolt to drop.
static bool boltThrown() {
  const uint32_t now = millis();
  const uint32_t lockedFor = now - lockCommandedAt;
  const uint32_t closedFor = now - doorClosedAt;
  return !doorUnlocked && !lockPowerPending && min(lockedFor, closedFor) >= LOCK_HW_DELAY_MS + BOLT_SETTLE_MS;
}

static void unlockDoor(const char *reason) {
  driveLock(false);
  if (!doorUnlocked) Serial.printf("[LOCK] OPEN (%s) for %lu ms\n", reason, (unsigned long)unlockTimeMs);
  else               Serial.printf("[LOCK] open time extended (%s)\n", reason);
  // Every accepted request starts a new cycle: relock-on-close waits for a passage
  // that starts now (or is in progress, door open), not for one already finished.
  doorWasOpened = !doorHome();
  doorUnlocked = true;
  lockPowerPending = false;
  unlockStart = millis();
  clearAlarms();
}

static void powerLock() {
  driveLock(true);
  lockPowerPending = false;
  lockCommandedAt = millis();
  Serial.println("[LOCK] LOCKED");
}

// Never power the lock onto an open or moving door: if the door is not closed
// and settled, keep the lock unpowered and let processTimers() power it later.
static void lockDoor() {
  doorUnlocked = false;
  doorWasOpened = false;
  if (doorSettled()) {
    powerLock();
  } else {
    driveLock(false);
    lockPowerPending = true;
    lockCommandedAt = millis();
    Serial.println("[LOCK] LOCKED, lock power deferred until the door is fully closed and settled");
  }
}

// =====================================================================
//  Enroll mode
// =====================================================================
static void setEnrollMode(bool on) {
  if (on == enrollMode) return;
  enrollMode = on;
  enrollStart = millis();
  buzzer.play(on ? Pattern::ENROLL_ENTER : Pattern::ENROLL_EXIT);
  Serial.printf("[ENROLL] %s\n", on ? "mode ON - present cards to add/remove" : "mode OFF");
}

static void wipeAllCards() {
  enrollMode = false;
  if (store.clear()) {
    buzzer.play(Pattern::WIPED);
    Serial.println("[ENROLL] all cards erased");
  } else {
    buzzer.play(Pattern::DENIED);
    Serial.println("[ENROLL] cards NOT erased (NVS write failed)");
  }
}

// =====================================================================
//  RFID
// =====================================================================
static void rfidInit() {
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
}

// The RC522 reads ISO/IEC 14443A cards only. Their UID is 4, 7 or 10 bytes
// (cascade levels 1..3); all three are stored and compared with their length.
static bool readCard(CardUid &out) {
  // Same as PICC_IsNewCardPresent(), but keeps the ATQA for the log below.
  byte atqa[2];
  byte atqaLen = sizeof(atqa);
  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
  const MFRC522::StatusCode req = rfid.PICC_RequestA(atqa, &atqaLen);
  if (req != MFRC522::STATUS_OK && req != MFRC522::STATUS_COLLISION) return false;  // no 14443A card in the field

  const MFRC522::StatusCode sel = rfid.PICC_Select(&rfid.uid);
  if (sel != MFRC522::STATUS_OK) {
    // A card answered REQA but anticollision/select failed: a non-standard 14443A
    // card (e.g. Topaz/Jewel) or poor coupling. Rate-limited: the card is not halted
    // and answers again on every loop.
    if (millis() - lastReadFailLog >= CARD_REPEAT_GUARD_MS) {
      lastReadFailLog = millis();
      Serial.printf("[RFID] card answered (ATQA %02X %02X) but UID read failed: %s\n", atqa[0], atqa[1],
                    reinterpret_cast<const char *>(MFRC522::GetStatusCodeName(sel)));
    }
    return false;
  }
  out.len = min<uint8_t>(rfid.uid.size, sizeof(out.b));
  memcpy(out.b, rfid.uid.uidByte, out.len);
  lastSak = rfid.uid.sak;
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

static const char *lastCardType() {
  return reinterpret_cast<const char *>(MFRC522::PICC_GetTypeName(MFRC522::PICC_GetType(lastSak)));
}

// The RC522 can stop reading cards while SPI still works: after ESD or a supply
// glitch on the reader it resets its registers (antenna off, default config) but
// VersionReg still answers. Check the registers rfidInit() sets and re-init if any
// of them was lost.
static void checkReaderHealth() {
  if (millis() - lastRfidCheck < RC522_CHECK_INTERVAL_MS) return;
  lastRfidCheck = millis();
  const byte ver  = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  const byte tx   = rfid.PCD_ReadRegister(MFRC522::TxControlReg);  // bits 1..0: TX2/TX1 antenna drivers on
  const byte ask  = rfid.PCD_ReadRegister(MFRC522::TxASKReg);      // PCD_Init() writes 0x40 (reset value 0x00)
  const byte gain = rfid.PCD_GetAntennaGain();                     // rfidInit() sets RxGain_max (reset value 0x40)
  const char *fault = nullptr;
  if (ver == 0x00 || ver == 0xFF)                      fault = "not responding";
  else if ((tx & 0x03) != 0x03)                        fault = "antenna off";
  else if (ask != 0x40 || gain != MFRC522::RxGain_max) fault = "registers reset";
  if (fault) {
    Serial.printf("[RFID] reader %s (ver 0x%02X tx 0x%02X ask 0x%02X gain 0x%02X), re-init\n",
                  fault, ver, tx, ask, gain);
    rfidInit();
  }
}

static void handleCard(const CardUid &uid) {
  char s[32];
  uidToString(uid, s, sizeof(s));

  // Enroll mode expires here too: processTimers() runs after processRfid(), so a card
  // read just past the timeout would otherwise still be added/removed and extend the mode.
  if (enrollMode && millis() - enrollStart >= ENROLL_TIMEOUT_MS) setEnrollMode(false);

  if (enrollMode) {
    enrollStart = millis();  // stay in enroll mode while cards keep coming
    const int idx = store.find(uid);
    if (idx >= 0) {
      if (store.removeAt(idx)) {
        buzzer.play(Pattern::CARD_REMOVED);
        Serial.printf("[ENROLL] removed %s (%u stored)\n", s, store.count());
      } else {
        buzzer.play(Pattern::DENIED);
        Serial.printf("[ENROLL] %s NOT removed (NVS write failed)\n", s);
      }
    } else if (store.count() >= MAX_CARDS) {
      buzzer.play(Pattern::STORE_FULL);
      Serial.printf("[ENROLL] store full (%u), %s not added\n", MAX_CARDS, s);
    } else if (store.add(uid)) {
      buzzer.play(Pattern::CARD_ADDED);
      Serial.printf("[ENROLL] added %s (%u bytes, %s, %u stored)\n", s, uid.len, lastCardType(), store.count());
    } else {
      buzzer.play(Pattern::DENIED);
      Serial.printf("[ENROLL] %s NOT added (NVS write failed)\n", s);
    }
    return;
  }

  if (store.find(uid) >= 0) {
    deniedCount = 0;
    buzzer.play(Pattern::GRANTED);
    Serial.printf("[RFID] granted %s\n", s);
    unlockDoor("card");
  } else {
    buzzer.play(Pattern::DENIED);
    Serial.printf("[RFID] denied %s (%u bytes, %s)\n", s, uid.len, lastCardType());
    if (++deniedCount >= MAX_DENIED_ATTEMPTS) {
      lockoutActive = true;
      lockoutStart = millis();
      deniedCount = 0;
      buzzer.play(Pattern::LOCKOUT);
      Serial.printf("[RFID] too many unknown cards, reader locked out for %lu ms\n",
                    (unsigned long)DENIED_LOCKOUT_MS);
    }
  }
}

static void processRfid() {
  checkReaderHealth();

  if (lockoutActive) {
    if (millis() - lockoutStart < DENIED_LOCKOUT_MS) return;
    lockoutActive = false;
    Serial.println("[RFID] lockout ended");
  }

  CardUid uid;
  if (!readCard(uid)) return;

  if (uidEquals(uid, lastUid) && (millis() - lastUidAt) < CARD_REPEAT_GUARD_MS) return;
  lastUid = uid;
  lastUidAt = millis();

  handleCard(uid);
}

// =====================================================================
//  Inputs
// =====================================================================
static void processButtons() {
  // Exit button: unlock on press
  if (exitBtn.update() && exitBtn.isActive()) {
    buzzer.play(Pattern::GRANTED);
    unlockDoor("exit button");
  }

  // Enroll button: short press toggles enroll mode, long hold wipes all cards
  if (enrollBtn.update()) {
    if (enrollBtn.isActive()) {
      wipeFired = false;
      lastWipeTick = 0;
    } else if (!wipeFired) {
      setEnrollMode(!enrollMode);
    }
  }
  if (enrollBtn.isActive() && !wipeFired) {
    const uint32_t held = enrollBtn.stableFor();
    if (held >= WIPE_HOLD_MS) {
      wipeFired = true;
      wipeAllCards();
    } else if (held >= WIPE_TICK_START_MS && held - lastWipeTick >= 1000) {
      lastWipeTick = held;
      buzzer.play(Pattern::TICK);
    }
  }

  // Door sensors
  const bool wasHome = doorHome();
  if (doorSense.update()) {
    if (doorClosed()) {
      doorClosedAt = millis();
      Serial.println("[DOOR] CLOSED");
    } else {
      Serial.println("[DOOR] OPEN");
      if (doorUnlocked) {
        doorWasOpened = true;
      }
#if DOOR_FORCED_ALARM_ENABLED
      else if (boltThrown()) {
        forcedAlarm = true;
        lastAlarmAt = millis() - ALARM_REPEAT_MS;  // sound immediately
        Serial.println("[ALARM] door FORCED open while locked");
      }
#endif
    }
  }
#if DOOR_ALIGN_SENSOR_ENABLED
  if (doorAlign.update()) Serial.println(doorAligned() ? "[DOOR] ALIGNED" : "[DOOR] NOT ALIGNED");
#endif
  if (doorHome() != wasHome) doorHomeChangedAt = millis();
}

// =====================================================================
//  Timers, alarm, LED
// =====================================================================
static void processTimers() {
  uint32_t now = millis();

  if (doorUnlocked && now - unlockStart >= unlockTimeMs) lockDoor();

  // Deferred relock: door now closed and settled
  if (lockPowerPending && doorSettled()) powerLock();

#if RELOCK_ON_DOOR_CLOSE
  // Door closed after passage and stayed closed for RELOCK_CLOSE_DELAY_MS
  if (doorUnlocked && doorWasOpened && doorSettled()) {
    Serial.println("[LOCK] door closed after passage, relocking");
    lockDoor();
  }
#endif

  if (enrollMode && now - enrollStart >= ENROLL_TIMEOUT_MS) setEnrollMode(false);

  // lockDoor()/powerLock() above may have stamped lockCommandedAt later than 'now';
  // now - lockCommandedAt would then wrap to ~2^32 and fire the alarm at once.
  now = millis();

  const bool secured = doorSecured();
  if (secured != lastSecured) {
    lastSecured = secured;
    securedChangedAt = now;
  }

#if DOOR_OPEN_ALARM_ENABLED
  // Door not secured continuously for DOOR_OPEN_ALARM_MS (a rebound after closing restarts it)
  if (!doorUnlocked && !secured && !doorOpenAlarm && !forcedAlarm &&
      now - lockCommandedAt >= DOOR_OPEN_ALARM_MS && now - securedChangedAt >= DOOR_OPEN_ALARM_MS) {
    doorOpenAlarm = true;
    lastAlarmAt = now - ALARM_REPEAT_MS;  // sound immediately
    Serial.println(doorClosed() ? "[ALARM] door not fully closed (align switch open)" : "[ALARM] door left open");
  }
#endif

  if ((doorOpenAlarm || forcedAlarm) && secured) clearAlarms();

  if ((doorOpenAlarm || forcedAlarm) && now - lastAlarmAt >= ALARM_REPEAT_MS) {
    lastAlarmAt = now;
    buzzer.play(forcedAlarm ? Pattern::FORCED : Pattern::ALARM);
  }
}

static void updateLed() {
  const uint32_t now = millis();
  bool on;
  if (enrollMode)         on = (now / 100) % 2;   // fast blink
  else if (lockoutActive) on = (now / 500) % 2;   // slow blink
  else                    on = doorUnlocked;
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

// =====================================================================
//  Serial console
// =====================================================================
static void printHelp() {
  Serial.println(F("Commands:\n"
                   "  help        this text\n"
                   "  list        list stored cards\n"
                   "  open        unlock the door\n"
                   "  enroll      toggle enroll mode\n"
                   "  clear       erase all cards\n"
                   "  time <ms>   set unlock time (500..60000), saved to NVS"));
}

static void processCommand(char *cmd) {
  if (!strcmp(cmd, "help")) {
    printHelp();
  } else if (!strcmp(cmd, "list")) {
    Serial.printf("%u/%u cards, unlock time %lu ms\n", store.count(), MAX_CARDS,
                  (unsigned long)unlockTimeMs);
    char s[32];
    for (uint8_t i = 0; i < store.count(); i++) {
      uidToString(store.at(i), s, sizeof(s));
      Serial.printf("  %2u: %s\n", i + 1, s);
    }
  } else if (!strcmp(cmd, "open")) {
    unlockDoor("serial");
  } else if (!strcmp(cmd, "enroll")) {
    setEnrollMode(!enrollMode);
  } else if (!strcmp(cmd, "clear")) {
    wipeAllCards();
  } else if (!strncmp(cmd, "time ", 5)) {
    const long ms = atol(cmd + 5);
    if (ms < (long)UNLOCK_TIME_MIN_MS || ms > (long)UNLOCK_TIME_MAX_MS) {
      Serial.println("out of range (500..60000)");
    } else {
      unlockTimeMs = ms;
      if (store.saveUnlockTime(unlockTimeMs)) Serial.printf("unlock time = %lu ms\n", (unsigned long)unlockTimeMs);
      else Serial.printf("unlock time = %lu ms until reboot (NVS write failed)\n", (unsigned long)unlockTimeMs);
    }
  } else if (cmd[0]) {
    Serial.println("unknown command, type 'help'");
  }
}

static void processSerial() {
  static char buf[32];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n < sizeof(buf) - 1) buf[n++] = c;
      continue;
    }
    buf[n] = '\0';
    n = 0;
    processCommand(buf);
  }
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  // Lock first: set the level before enabling the output driver.
  digitalWrite(PIN_LOCK_DRIVE, LOCK_LEVEL_LOCKED);
  pinMode(PIN_LOCK_DRIVE, OUTPUT);
  lockCommandedAt = millis();

  pinMode(PIN_STATUS_LED, OUTPUT);
  buzzer.begin(PIN_BUZZER);

  Serial.begin(115200);
  delay(50);
  Serial.printf("\nRFID door lock v%s\n", FW_VERSION);

  exitBtn.begin(PIN_EXIT_BUTTON, BUTTON_DEBOUNCE_MS);
  enrollBtn.begin(PIN_ENROLL_BUTTON, BUTTON_DEBOUNCE_MS);
  doorSense.begin(PIN_DOOR_SENSE, SENSE_DEBOUNCE_MS);
#if DOOR_ALIGN_SENSOR_ENABLED
  doorAlign.begin(PIN_DOOR_ALIGN, SENSE_DEBOUNCE_MS);
#endif
  doorClosedAt = doorHomeChangedAt = millis() - RELOCK_CLOSE_DELAY_MS;  // door home at boot counts as settled
  lockDoor();  // door not home at boot: lock power deferred until it is home and settled
  lastSecured = doorSecured();
  securedChangedAt = millis();

  if (!store.begin()) Serial.println("[NVS] init failed: no stored cards, default unlock time, changes not saved");
  unlockTimeMs = store.loadUnlockTime();
  Serial.printf("[NVS] %u cards stored, unlock time %lu ms\n", store.count(),
                (unsigned long)unlockTimeMs);

  SPI.begin(PIN_RC522_SCK, PIN_RC522_MISO, PIN_RC522_MOSI, PIN_RC522_SS);
  rfidInit();
  const byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] RC522 version 0x%02X%s\n", v, (v == 0x00 || v == 0xFF) ? " (NOT FOUND - check wiring)" : "");

  Serial.printf("[DOOR] %s\n", doorClosed() ? "CLOSED" : "OPEN");
#if DOOR_ALIGN_SENSOR_ENABLED
  Serial.printf("[DOOR] align switch on GPIO%u: %s\n", PIN_DOOR_ALIGN, doorAligned() ? "ALIGNED" : "NOT ALIGNED");
#endif
  if (store.count() == 0) Serial.println("No cards stored: press the enroll button and present a card.");
  printHelp();

  buzzer.play(Pattern::BOOT);

  // Reset if loop() stalls > 5 s (all driver options keep the door locked through the reset)
  enableLoopWDT();
}

void loop() {
  buzzer.update();
  processButtons();
  processRfid();
  processTimers();
  processSerial();
  updateLed();
}
