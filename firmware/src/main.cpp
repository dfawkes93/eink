// Claude usage + input-alert display for the LilyGO T5 4.7" e-paper board.
//
// The host bridge (host/eink_bridge.py) sends newline-delimited JSON over USB
// serial and/or BLE:
//   {"t":"state", ...}  full display state, see state.h
//   {"t":"ping"}        keepalive, answered with {"t":"pong"}
//   {"t":"saver"}       testing: enter battery saver now and stay there,
//                       whatever the battery says, until USB serial is back
// We send back:
//   {"t":"hello","fw":N,"saver":B} on boot/wake, so the host re-sends state
//   {"t":"dismiss","id":N}         when the dismiss button is pressed
//   {"t":"btn","pin":P,"press":"short"|"long","id":N}  on every button press
//   {"t":"cmd","do":"refresh_usage"|"dnd","id":N}      host-side button actions
// Events go out over every link, so `id` (random per boot, then counting up)
// lets the host drop the copy that arrives on the second link.
//
// Power: always on while a host talks to us over USB, or while the battery is
// above ~1/3. Below that we enter battery saver: deep sleep, waking every
// SAVER_WAKE_S to pick up state over BLE, or straight away on the dismiss and
// page buttons. The panel keeps its image while we sleep.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <epd_driver.h>
#include <esp_sleep.h>
#include <utilities.h>

#include "ble_link.h"
#include "display.h"
#include "state.h"

constexpr int FW_VERSION = 2;
constexpr uint32_t USB_TIMEOUT_MS = 90 * 1000;  // host pings every 30 s
constexpr uint32_t STALE_MS = 10 * 60 * 1000;
constexpr uint32_t BATTERY_EVERY_MS = 30 * 1000;
constexpr int BATTERY_SAMPLES = 4;              // average over 2 minutes

// Battery saver thresholds (resting Li-ion voltage; ~3.70 V is about a third).
constexpr float SAVER_ENTER_V = 3.70f, SAVER_EXIT_V = 3.80f;
constexpr float NO_BATTERY_V = 2.5f;
constexpr uint64_t SAVER_WAKE_S = 120;
constexpr uint32_t SAVER_AWAKE_MS = 12 * 1000;   // max time to wait for the host
constexpr uint32_t SAVER_LINGER_MS = 1500;       // after state arrives
constexpr uint32_t SAVER_BUTTON_MS = 30 * 1000;  // stay up after a button press
constexpr int SAVER_STALE_WAKES = 5;

// Survives deep sleep. Raw bytes rather than the structs themselves: anything
// with constructors would be re-initialised on every wake.
struct Persist {
  uint32_t magic;
  State state;
  Page page;
  LinkStatus link;  // as last drawn
  int partials;
  uint32_t alertSeq;
  bool saver;
  bool forcedSaver;
  bool off;  // powered off from the button; the panel shows the Off screen
  bool pendingDismiss;
  int wakesWithoutState;
};
constexpr uint32_t PERSIST_MAGIC = 0xC1A0DE02;
RTC_NOINIT_ATTR static uint8_t rtcBlob[sizeof(Persist)];

static State state;
static Page page = Page::Overview;
static uint32_t lastSerialMs, lastAnyMs, lastBatteryMs, lastAlertSeq;
static bool dirty = true, flashNext = false;
static bool saver, forcedSaver, pendingDismiss, gotState;
static bool interactive;  // a button was used this wake: don't rush back to sleep
static int wakesWithoutState;
static uint32_t awakeUntilMs;
static float batterySamples[BATTERY_SAMPLES];
static int nSamples, sampleIdx;
static float battery;  // latest averaged reading, volts
static String serialLine;
static uint32_t eventId;

static bool usbHost() { return lastSerialMs && millis() - lastSerialMs < USB_TIMEOUT_MS; }

static int batteryPercent(float v) {
  // Rough resting-voltage curve for a single Li-ion cell.
  static const float volts[] = {3.30, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20};
  static const int pct[] = {0, 8, 18, 33, 50, 63, 76, 90, 100};
  if (v <= volts[0]) return 0;
  for (int i = 1; i < 9; i++)
    if (v <= volts[i])
      return pct[i - 1] + (pct[i] - pct[i - 1]) * (v - volts[i - 1]) / (volts[i] - volts[i - 1]);
  return 100;
}

static LinkStatus linkStatus() {
  bool onBattery = !usbHost() && battery > NO_BATTERY_V;
  int pct = battery > NO_BATTERY_V ? batteryPercent(battery) / 5 * 5 : -1;
  return {
      .usb = usbHost(),
      .ble = !saver && bleConnected(),
      .stale = state.valid && (saver ? wakesWithoutState >= SAVER_STALE_WAKES
                                     : millis() - lastAnyMs > STALE_MS),
      .onBattery = onBattery,
      .saver = saver,
      .batteryPct = pct,
      .battery = battery,
  };
}

static float readBattery() {
  // POWER_EN has to be on for the battery divider to read correctly.
  epd_poweron();
  delay(10);
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BATT_PIN);
  epd_poweroff();
  return mv / 8 * 2 / 1000.0f;
}

static void sampleBattery() {
  batterySamples[sampleIdx] = readBattery();
  sampleIdx = (sampleIdx + 1) % BATTERY_SAMPLES;
  if (nSamples < BATTERY_SAMPLES) nSamples++;
  float sum = 0;
  for (int i = 0; i < nSamples; i++) sum += batterySamples[i];
  battery = sum / nSamples;
  lastBatteryMs = millis();
}

static void send(const char *json) {
  Serial.println(json);
  bleSend(String(json) + "\n");
}

static void enterSaver();

static void handleLine(const String &line, bool fromSerial) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;  // boot noise, partial lines, etc.
  const char *t = doc["t"] | "";
  if (fromSerial) lastSerialMs = millis();
  lastAnyMs = millis();

  if (!strcmp(t, "state")) {
    parseState(state, doc);
    gotState = true;
    // Blink the whole panel once per new alert, not on every redraw of it.
    if (state.alert && state.alertSeq != lastAlertSeq) {
      flashNext = true;
      page = Page::Overview;
    }
    lastAlertSeq = state.alertSeq;
    dirty = true;
  } else if (!strcmp(t, "saver")) {
    lastSerialMs = 0;  // pretend USB went away, or we'd wake straight back up
    forcedSaver = true;
    enterSaver();
  } else if (!strcmp(t, "ping")) {
    if (fromSerial)
      Serial.println("{\"t\":\"pong\"}");
    else
      bleSend("{\"t\":\"pong\"}\n");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleLine(serialLine, true);
      serialLine = "";
    } else if (c != '\r' && serialLine.length() < 8192) {
      serialLine += c;
    }
  }
}

// Buttons are active-low with external pull-ups (34/35/39 are input-only).
//
// BOOT is on GPIO0, which doubles as the display's config strobe (CFG_STR) and
// is normally an output held high between display operations. To read it we
// briefly make it an input: with the strobe floating high the shift
// register's outputs don't change, so the display isn't disturbed. While it's
// held we leave it as an input (driving it high would short it through the
// switch) and hold off display refreshes, which need the strobe.
static bool readPin(int pin) {
  if (pin != 0) return digitalRead(pin);
  pinMode(0, INPUT_PULLUP);
  delayMicroseconds(20);
  bool level = digitalRead(0);
  if (level) {
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
  }
  return level;
}

enum class Press { None, Short, Long };
constexpr uint32_t LONG_PRESS_MS = 800;

struct Button {
  int pin;
  bool down = false, longDone = false, ignore = false;
  uint32_t changedMs = 0, downMs = 0;
  // BOOT decides on release, so nothing redraws while it's still held.
  bool longOnRelease;
  explicit Button(int p, bool onRelease = false) : pin(p), longOnRelease(onRelease) {}

  // A button already held at boot is ignored until released, unless it's
  // the one that woke us from saver - then it's a press in progress.
  void begin(bool heldIsPress = false) {
    down = readPin(pin) == LOW;
    downMs = millis();
    ignore = down && !heldIsPress;
  }

  Press poll() {
    bool now = readPin(pin) == LOW;
    Press p = Press::None;
    if (now != down && millis() - changedMs > 40) {
      down = now;
      changedMs = millis();
      if (down) {
        downMs = millis();
        longDone = false;
      } else if (ignore) {
        ignore = false;
      } else if (!longDone) {
        p = longOnRelease && millis() - downMs >= LONG_PRESS_MS ? Press::Long : Press::Short;
      }
    } else if (down && !ignore && !longOnRelease && !longDone &&
               millis() - downMs >= LONG_PRESS_MS) {
      longDone = true;
      p = Press::Long;
    }
    if (p != Press::None) {
      char msg[80];
      snprintf(msg, sizeof msg, "{\"t\":\"btn\",\"pin\":%d,\"press\":\"%s\",\"id\":%lu}", pin,
               p == Press::Long ? "long" : "short", ++eventId);
      send(msg);
    }
    return p;
  }
};
static Button btnDismiss(BUTTON_1), btnPage(BUTTON_2), btnRefresh(BUTTON_3), btnBoot(0, true);

static void hostCommand(const char *what) {
  char msg[64];
  snprintf(msg, sizeof msg, "{\"t\":\"cmd\",\"do\":\"%s\",\"id\":%lu}", what, ++eventId);
  send(msg);
}

static void powerOff();

static void dismiss() {
  pendingDismiss = true;
  // Clear locally straight away; the host will confirm with a new state.
  if (state.alert) {
    state.alert = false;
    dirty = true;
  }
}

static void togglePage() {
  page = page == Page::Overview ? Page::Details : Page::Overview;
  dirty = true;
}

// Layout, left to right: RST, BOOT, 35, 34, 39.
//   BOOT  short: refresh usage now      long: do-not-disturb 1 h on/off
//   35    short: switch page            long: power off (35 or 34 turns it on)
//   34    short: dismiss alert
//   39    short: full refresh           long: battery saver now
static void pollButtons() {
  Press boot = btnBoot.poll(), page35 = btnPage.poll(), dis = btnDismiss.poll(),
        ref = btnRefresh.poll();
  if (boot == Press::Short) hostCommand("refresh_usage");
  if (boot == Press::Long) hostCommand("dnd");
  if (page35 == Press::Short) togglePage();
  if (page35 == Press::Long) powerOff();
  if (dis == Press::Short) dismiss();
  if (ref == Press::Short) flashNext = dirty = true;
  if (ref == Press::Long) {
    forcedSaver = true;  // stays until USB serial is back, like {"t":"saver"}
    enterSaver();
  }
  bool any = boot != Press::None || page35 != Press::None || dis != Press::None ||
             ref != Press::None;
  if (any && saver) {
    interactive = true;
    awakeUntilMs = millis() + SAVER_BUTTON_MS;
  }
}

// --- deep sleep ----------------------------------------------------------------

static void armWakeButtons() {
  // A wake button still held from the press that sent us to sleep would wake
  // us straight back up.
  uint32_t t0 = millis();
  while ((digitalRead(BUTTON_1) == LOW || digitalRead(BUTTON_2) == LOW) && millis() - t0 < 10000)
    delay(20);
  delay(50);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_1, 0);
  esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_2, ESP_EXT1_WAKEUP_ALL_LOW);
}

static void savePersist(const LinkStatus &link, bool off = false) {
  Persist p = {
      .magic = PERSIST_MAGIC,
      .state = state,
      .page = page,
      .link = link,
      .partials = displayPartials(),
      .alertSeq = lastAlertSeq,
      .saver = saver,
      .forcedSaver = forcedSaver,
      .off = off,
      .pendingDismiss = pendingDismiss,
      .wakesWithoutState = wakesWithoutState,
  };
  memcpy(rtcBlob, &p, sizeof p);
}

static void sleepNow() {
  wakesWithoutState = gotState ? 0 : wakesWithoutState + 1;
  if (!gotState) page = Page::Overview;  // details would just go stale
  LinkStatus link = linkStatus();
  displayRender(state, page, link, flashNext);
  savePersist(link);

  bleDisconnectAll();
  delay(200);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SAVER_WAKE_S * 1000000ULL);
  armWakeButtons();
  // Leave the panel's PWR_EN as epd_poweroff() left it: on battery that line
  // also holds the board's main supply on.
  esp_deep_sleep_start();
}

static void powerOff() {
  displayMessage("Off", "Press the page or dismiss button to turn on");
  savePersist(linkStatus(), true);
  bleDisconnectAll();
  delay(200);
  Serial.flush();
  armWakeButtons();
  esp_deep_sleep_start();
}

static void enterSaver() {
  saver = true;
  page = Page::Overview;
  dirty = true;
  sleepNow();
}

static void exitSaver() {
  saver = forcedSaver = false;
  dirty = true;
}

// Restores what we had before deep sleep. Returns false on a cold boot.
static bool restorePersist() {
  Persist p;
  memcpy(&p, rtcBlob, sizeof p);
  if (p.magic != PERSIST_MAGIC || esp_reset_reason() != ESP_RST_DEEPSLEEP) return false;
  if (p.off) {
    // Turning back on: the panel shows the Off screen, so redraw it all.
    state = p.state;
    lastAlertSeq = p.alertSeq;
    flashNext = dirty = true;
    return true;
  }
  state = p.state;
  page = p.page;
  lastAlertSeq = p.alertSeq;
  saver = p.saver;
  forcedSaver = p.forcedSaver;
  pendingDismiss = p.pendingDismiss;
  wakesWithoutState = p.wakesWithoutState;
  displayAssumeShown(p.state, p.page, p.link, p.partials);
  dirty = false;
  return true;
}

// -------------------------------------------------------------------------------

void setup() {
  Serial.setRxBufferSize(8192);  // e-paper refreshes block loop() for ~1 s
  Serial.begin(115200);
  for (int pin : {BUTTON_1, BUTTON_2, BUTTON_3}) pinMode(pin, INPUT);
  displayBegin();
  eventId = esp_random() & 0xFFFFFF00;

  bool restored = restorePersist();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Button *waker = cause == ESP_SLEEP_WAKEUP_EXT0   ? &btnDismiss
                  : cause == ESP_SLEEP_WAKEUP_EXT1 ? &btnPage
                                                   : nullptr;
  // The press that turns the board on is just "on"; the press that wakes it
  // from saver counts as a normal press.
  bool wakePress = restored && saver && waker;
  for (Button *b : {&btnDismiss, &btnPage, &btnRefresh, &btnBoot}) b->begin(wakePress && b == waker);
  if (wakePress && waker->down == false) {
    // Tapped and released before we got here: treat it as a short press.
    if (waker == &btnDismiss) dismiss();
    if (waker == &btnPage) togglePage();
  }
  if (restored && saver) {
    awakeUntilMs = millis() + SAVER_AWAKE_MS;
    if (wakePress) {
      interactive = true;
      awakeUntilMs = millis() + SAVER_BUTTON_MS;
    }
  }
  sampleBattery();
  if (saver && !forcedSaver && battery >= SAVER_EXIT_V) exitSaver();  // charged while asleep

  bleBegin("Claude E-Ink");
  char hello[96];
  snprintf(hello, sizeof hello, "{\"t\":\"hello\",\"fw\":%d,\"saver\":%s,\"battery\":%.2f}",
           FW_VERSION, saver ? "true" : "false", battery);
  send(hello);
}

void loop() {
  pollSerial();
  String line;
  while (bleReadLine(line)) handleLine(line, false);
  pollButtons();

  if (pendingDismiss && (usbHost() || bleConnected())) {
    char msg[40];
    snprintf(msg, sizeof msg, "{\"t\":\"dismiss\",\"id\":%lu}", ++eventId);
    send(msg);
    pendingDismiss = false;
  }

  if (saver) {
    if (usbHost()) {
      exitSaver();  // plugged back in
    } else {
      // Once the host has delivered state, stay a moment for any dismiss to
      // go out, then sleep. Without a host, give up at awakeUntilMs.
      if (gotState && !pendingDismiss && !interactive)
        awakeUntilMs = min(awakeUntilMs, lastAnyMs + SAVER_LINGER_MS);
      if ((int32_t)(millis() - awakeUntilMs) >= 0) sleepNow();
    }
  } else if (millis() - lastBatteryMs > BATTERY_EVERY_MS) {
    sampleBattery();
    LinkStatus link = linkStatus();
    if (link.onBattery && nSamples == BATTERY_SAMPLES && battery <= SAVER_ENTER_V) enterSaver();
  }

  // Link/staleness changes only touch the header; the band diff in
  // displayRender keeps that cheap, so just re-render every few seconds.
  static uint32_t lastRenderMs;
  if (!btnBoot.down && (dirty || millis() - lastRenderMs > 5000)) {
    displayRender(state, page, linkStatus(), flashNext);
    dirty = flashNext = false;
    lastRenderMs = millis();
  }
  delay(10);
}
