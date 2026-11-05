#pragma once
#include "state.h"

enum class Page : uint8_t { Overview, Details };

// Plain data so it can be kept in RTC memory across deep sleep.
struct LinkStatus {
  bool usb;            // a host has talked to us over serial recently
  bool ble;            // a BLE central is connected
  bool stale;          // nothing from the host for a long time
  bool onBattery;
  bool saver;          // battery saver: deep sleep between check-ins
  int batteryPct;      // -1 if unknown
  float battery;       // volts, 0 if unknown
};

void displayBegin();
// Redraws whatever changed. `flash` forces a full-screen clear first, which on
// e-paper is a visible black/white blink - we use it to announce new alerts.
void displayRender(const State &s, Page page, const LinkStatus &link, bool flash);
// After deep sleep the panel still shows the last frame but our copy of it is
// gone. Rebuild that copy from the inputs that drew it, without touching the
// panel, so the next render can still refresh only what changed.
void displayAssumeShown(const State &s, Page page, const LinkStatus &link, int partials);
int displayPartials();
// Full-screen message, e.g. the Off screen. Always a full refresh.
void displayMessage(const char *title, const char *subtitle);
