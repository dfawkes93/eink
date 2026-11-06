#include "display.h"

#include <Arduino.h>
#include <epd_driver.h>
#include <roboto12.h>
#include <roboto18.h>
#include <roboto32.h>

#include "fonts/robotobold20.h"

// 4 bits per pixel, 0 = black, 15 = white.
constexpr uint8_t BLACK = 0x0, DARK = 0x5, MID = 0x9, LIGHT = 0xC, WHITE = 0xF;
constexpr int FB_SIZE = EPD_WIDTH * EPD_HEIGHT / 2;
constexpr int ROW_BYTES = EPD_WIDTH / 2;
// Partial refreshes leave ghosting behind; clear the whole panel after this many.
constexpr int PARTIALS_BEFORE_FULL = 20;

static uint8_t *fb;    // frame being drawn
static uint8_t *shown; // what is currently on the panel
static int partials = PARTIALS_BEFORE_FULL;  // forces a full refresh first time

static const GFXfont *SMALL = &Roboto12, *MEDIUM = &Roboto18, *LARGE = &Roboto32;
// White on black needs heavier strokes to read well (fonts/ is made by tools/make_font.py).
static const GFXfont *BOLD = &RobotoBold20;

static int textWidth(const GFXfont *font, const char *s) {
  int32_t x = 0, y = 0, x1, y1, w, h;
  get_text_bounds(font, s, &x, &y, &x1, &y1, &w, &h, nullptr);
  return w;
}

static int capHeight(const GFXfont *font) {
  int32_t x = 0, y = 0, x1, y1, w, h;
  get_text_bounds(font, "H", &x, &y, &x1, &y1, &w, &h, nullptr);
  return h;
}

// Draws `s` with its baseline at y, shortened with "..." to fit maxWidth.
static void text(const GFXfont *font, const char *s, int x, int y, int maxWidth = 0,
                 uint8_t fg = BLACK, uint8_t bg = WHITE) {
  char buf[128];
  strlcpy(buf, s, sizeof buf);
  if (maxWidth > 0 && textWidth(font, buf) > maxWidth) {
    size_t n = strlen(buf);
    while (n > 0) {
      strcpy(buf + --n, "...");
      if (textWidth(font, buf) <= maxWidth) break;
    }
  }
  if (!buf[0]) return;
  FontProperties props = {.fg_color = fg, .bg_color = bg, .fallback_glyph = '?', .flags = 0};
  int32_t cx = x, cy = y;
  write_mode(font, buf, &cx, &cy, fb, BLACK_ON_WHITE, &props);
}

static void textRight(const GFXfont *font, const char *s, int right, int y,
                      uint8_t fg = BLACK, uint8_t bg = WHITE) {
  text(font, s, right - textWidth(font, s), y, 0, fg, bg);
}

static void thickRect(int x, int y, int w, int h, int t, uint8_t c) {
  for (int i = 0; i < t; i++) epd_draw_rect(x + i, y + i, w - 2 * i, h - 2 * i, c, fb);
}

// ---------------------------------------------------------------------------

static void drawHeader(const State &s, const LinkStatus &link) {
  text(MEDIUM, "Claude", 24, 46);

  // Deliberately no clock here: anything that changes every minute would make
  // the panel blink every minute.
  char status[128], battery[48] = "";
  if (link.onBattery && link.batteryPct >= 0)
    snprintf(battery, sizeof battery, "battery %d%%", link.batteryPct);
  if (s.dnd[0]) {
    size_t n = strlen(battery);
    snprintf(battery + n, sizeof battery - n, "%sDND until %s", n ? "   " : "", s.dnd);
  }
  if (!s.valid) {
    strcpy(status, "waiting for host...");
  } else if (link.stale) {
    snprintf(status, sizeof status, "HOST OFFLINE - last update %s", s.now);
  } else if (link.saver) {
    // Links come and go every wake in saver mode, so don't show them.
    snprintf(status, sizeof status, "BATTERY SAVER %d%% - updates every 2 min", link.batteryPct);
  } else {
    snprintf(status, sizeof status, "%s%s%s%s%s", link.usb ? "USB" : "",
             link.usb && link.ble ? " + " : "", link.ble ? "BLE" : "",
             battery[0] && (link.usb || link.ble) ? "   " : "", battery);
  }
  textRight(SMALL, status, EPD_WIDTH - 24, 42);
  epd_fill_rect(0, 64, EPD_WIDTH, 3, BLACK, fb);
}

static void drawUsage(int x, int w, const char *label, const UsageWindow &u) {
  text(SMALL, label, x, 104);
  if (!u.valid) {
    text(MEDIUM, "--", x, 176);
    return;
  }
  char buf[48];
  snprintf(buf, sizeof buf, "%d%%", u.pct);
  text(LARGE, buf, x, 178);

  // The bar: filled to usage, with a tick showing how far through the window
  // we are. Fill ahead of the tick means you're burning faster than the clock.
  const int by = 196, bh = 30;
  int pct = constrain(u.pct, 0, 100);
  epd_fill_rect(x, by, (w * pct) / 100, bh, u.pct >= 90 ? BLACK : DARK, fb);
  thickRect(x, by, w, bh, 2, BLACK);
  if (u.elapsed >= 0) {
    int tx = x + (w * constrain(u.elapsed, 0, 100)) / 100;
    epd_fill_rect(tx - 2, by - 10, 4, bh + 20, BLACK, fb);
  }

  if (u.resetIn[0])
    snprintf(buf, sizeof buf, "resets %s (in %s)", u.reset, u.resetIn);
  else
    snprintf(buf, sizeof buf, "resets %s", u.reset);
  text(SMALL, buf, x, 262, w);
}

// One line, fixed height so the session list below keeps room for 3 rows:
// "NEEDS INPUT", then the waiting sessions' names, as many as fit, then
// "+N more" for the rest.
static int drawAlertBanner(const State &s, int y) {
  constexpr int PAD = 16;
  const int cap = capHeight(BOLD), h = cap + 2 * PAD, base = y + PAD + cap;
  epd_fill_rect(0, y, EPD_WIDTH, h, BLACK, fb);

  const char *label = "NEEDS INPUT";
  text(BOLD, label, 24, base, 0, WHITE, BLACK);

  const char *names[MAX_SESSIONS];
  int n = 0;
  for (int i = 0; i < s.nSessions; i++)
    if (!strcmp(s.sessions[i].state, "waiting")) names[n++] = s.sessions[i].name;

  const int x = 24 + textWidth(BOLD, label) + 40, maxW = EPD_WIDTH - 24 - x;
  // Take whole names while they, and the "+N more" for the rest, still fit.
  char line[MAX_SESSIONS * 40] = "", more[16] = "";
  int shown = 0;
  for (; shown < n; shown++) {
    char next[sizeof line], rest[16] = "";
    snprintf(next, sizeof next, "%s%s%s", line, shown ? ", " : "", names[shown]);
    if (shown + 1 < n) snprintf(rest, sizeof rest, " +%d more", n - shown - 1);
    char test[sizeof line + 16];
    snprintf(test, sizeof test, "%s%s", next, rest);
    if (shown > 0 && textWidth(BOLD, test) > maxW) break;
    strlcpy(line, next, sizeof line);
  }
  if (shown < n) snprintf(more, sizeof more, " +%d more", n - shown);
  const int moreW = more[0] ? textWidth(BOLD, more) : 0;
  // A single name that is too long on its own gets cut with "..." instead.
  text(BOLD, line, x, base, maxW - moreW, WHITE, BLACK);
  if (more[0]) text(BOLD, more, x + min(textWidth(BOLD, line), maxW - moreW), base, 0, LIGHT, BLACK);
  return y + h + 8;
}

static void drawSessions(const State &s, int y) {
  if (s.alert) y = drawAlertBanner(s, y);

  if (s.nSessions == 0) {
    text(SMALL, "No active Claude sessions", 24, y + 40, 0, MID);
    return;
  }
  const int rowH = 46;
  for (int i = 0; i < s.nSessions && y + rowH <= EPD_HEIGHT; i++, y += rowH) {
    const SessionInfo &si = s.sessions[i];
    const int my = y + 12;
    if (!strcmp(si.state, "waiting")) {
      epd_fill_rect(24, my, 22, 22, BLACK, fb);
    } else if (!strcmp(si.state, "working")) {
      epd_fill_rect(24, my, 22, 22, MID, fb);
    } else {
      thickRect(24, my, 22, 22, 2, BLACK);
    }
    text(SMALL, si.name, 64, y + 32, 260);
    text(SMALL, si.msg[0] ? si.msg : si.state, 340, y + 32, 500, DARK);
    textRight(SMALL, si.since, EPD_WIDTH - 24, y + 32, MID);
  }
}

static void drawOverview(const State &s) {
  const int colW = (EPD_WIDTH - 24 * 3) / 2;
  drawUsage(24, colW, "SESSION (5 hours)", s.session);
  drawUsage(48 + colW, colW, "WEEK", s.week);
  epd_fill_rect(0, 284, EPD_WIDTH, 2, BLACK, fb);
  drawSessions(s, 292);
}

static void drawDetails(const State &s, const LinkStatus &link) {
  int y = 110;
  text(MEDIUM, "This week by product", 24, y);
  y += 20;
  for (int i = 0; i < s.nBreakdown; i++, y += 50) {
    const BreakdownRow &r = s.breakdown[i];
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", r.pct);
    text(SMALL, r.name, 24, y + 32);
    const int bx = 220, bw = 560;
    epd_fill_rect(bx, y + 12, (bw * constrain(r.pct, 0, 100)) / 100, 24, DARK, fb);
    thickRect(bx, y + 12, bw, 24, 2, BLACK);
    textRight(SMALL, pct, EPD_WIDTH - 24, y + 32);
  }

  char buf[96];
  y = 420;
  snprintf(buf, sizeof buf, "Usage fetched %s", s.usageAge[0] ? s.usageAge : "never");
  text(SMALL, buf, 24, y);
  if (link.battery > 0) {
    snprintf(buf, sizeof buf, "Battery %.2f V", link.battery);
    textRight(SMALL, buf, EPD_WIDTH - 24, y);
  }
  if (s.error[0]) {
    snprintf(buf, sizeof buf, "Host: %s", s.error);
    text(SMALL, buf, 24, y + 40, EPD_WIDTH - 48);
  }
  // Left to right on the board; "a/b" = press / hold.
  text(SMALL, "BOOT usage/DND    35 page/off    34 dismiss    39 refresh/saver", 24, 520,
       EPD_WIDTH - 48, MID);
}

// ---------------------------------------------------------------------------

void displayBegin() {
  fb = (uint8_t *)ps_calloc(1, FB_SIZE);
  shown = (uint8_t *)ps_calloc(1, FB_SIZE);
  memset(fb, 0xFF, FB_SIZE);
  memset(shown, 0xFF, FB_SIZE);
  epd_init();
}

static void compose(const State &s, Page page, const LinkStatus &link) {
  memset(fb, 0xFF, FB_SIZE);
  drawHeader(s, link);
  if (page == Page::Overview)
    drawOverview(s);
  else
    drawDetails(s, link);
}

void displayAssumeShown(const State &s, Page page, const LinkStatus &link, int partialCount) {
  compose(s, page, link);
  memcpy(shown, fb, FB_SIZE);
  partials = partialCount;
}

int displayPartials() { return partials; }

void displayMessage(const char *title, const char *subtitle) {
  memset(fb, 0xFF, FB_SIZE);
  text(LARGE, title, (EPD_WIDTH - textWidth(LARGE, title)) / 2, 250);
  text(SMALL, subtitle, (EPD_WIDTH - textWidth(SMALL, subtitle)) / 2, 320, 0, DARK);
  Rect_t area = epd_full_screen();
  epd_poweron();
  epd_clear();
  epd_draw_grayscale_image(area, fb);
  epd_poweroff();
  memcpy(shown, fb, FB_SIZE);
  partials = 0;
}

void displayRender(const State &s, Page page, const LinkStatus &link, bool flash) {
  compose(s, page, link);

  bool full = flash || partials >= PARTIALS_BEFORE_FULL;
  int top = 0, bottom = EPD_HEIGHT;
  if (!full) {
    // Refresh only the band of rows that actually changed.
    while (top < EPD_HEIGHT && !memcmp(fb + top * ROW_BYTES, shown + top * ROW_BYTES, ROW_BYTES)) top++;
    if (top == EPD_HEIGHT) return;
    while (bottom > top && !memcmp(fb + (bottom - 1) * ROW_BYTES, shown + (bottom - 1) * ROW_BYTES, ROW_BYTES)) bottom--;
  }

  Rect_t area = {.x = 0, .y = top, .width = EPD_WIDTH, .height = bottom - top};
  epd_poweron();
  if (full) {
    epd_clear();
    partials = 0;
  } else {
    epd_clear_area(area);
    partials++;
  }
  epd_draw_grayscale_image(area, fb + top * ROW_BYTES);
  epd_poweroff();
  memcpy(shown, fb, FB_SIZE);
}
