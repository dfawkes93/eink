#pragma once
#include <ArduinoJson.h>

// Everything the host tells us. The host sends the whole state every time, so
// the board never has to merge partial updates.

constexpr int MAX_SESSIONS = 6;
constexpr int MAX_BREAKDOWN = 4;

struct UsageWindow {
  bool valid = false;
  int pct = 0;          // utilisation, 0-100+
  int elapsed = -1;     // how far through the window we are, 0-100, -1 unknown
  char reset[24] = "";  // local reset time, preformatted by the host
  char resetIn[16] = "";
};

struct SessionInfo {
  char name[32] = "";
  char state[12] = "";  // "waiting", "idle", "working"
  char msg[96] = "";
  char since[8] = "";   // HH:MM
};

struct BreakdownRow {
  char name[20] = "";
  int pct = 0;
};

struct State {
  bool valid = false;
  char now[8] = "";        // host clock at send time, HH:MM
  char usageAge[16] = "";  // when usage was last fetched, HH:MM
  char error[64] = "";     // host-side problem, e.g. expired token
  char dnd[8] = "";        // do-not-disturb until HH:MM, empty when off
  UsageWindow session, week;
  SessionInfo sessions[MAX_SESSIONS];
  int nSessions = 0;
  BreakdownRow breakdown[MAX_BREAKDOWN];
  int nBreakdown = 0;
  bool alert = false;      // some session is waiting and not dismissed
  uint32_t alertSeq = 0;   // bumped by the host for every new alert
};

inline void copyStr(char *dst, size_t n, JsonVariantConst v) {
  const char *s = v | "";
  strlcpy(dst, s, n);
}

inline void parseWindow(UsageWindow &w, JsonVariantConst j) {
  w.valid = !j.isNull();
  if (!w.valid) return;
  w.pct = j["pct"] | 0;
  w.elapsed = j["elapsed"] | -1;
  copyStr(w.reset, sizeof w.reset, j["reset"]);
  copyStr(w.resetIn, sizeof w.resetIn, j["reset_in"]);
}

inline void parseState(State &s, JsonDocument &doc) {
  s = State();
  s.valid = true;
  copyStr(s.now, sizeof s.now, doc["now"]);
  copyStr(s.usageAge, sizeof s.usageAge, doc["usage_at"]);
  copyStr(s.error, sizeof s.error, doc["error"]);
  copyStr(s.dnd, sizeof s.dnd, doc["dnd"]);
  parseWindow(s.session, doc["session"]);
  parseWindow(s.week, doc["week"]);
  for (JsonVariantConst j : doc["sessions"].as<JsonArrayConst>()) {
    if (s.nSessions == MAX_SESSIONS) break;
    SessionInfo &si = s.sessions[s.nSessions++];
    copyStr(si.name, sizeof si.name, j["name"]);
    copyStr(si.state, sizeof si.state, j["state"]);
    copyStr(si.msg, sizeof si.msg, j["msg"]);
    copyStr(si.since, sizeof si.since, j["since"]);
  }
  for (JsonVariantConst j : doc["breakdown"].as<JsonArrayConst>()) {
    if (s.nBreakdown == MAX_BREAKDOWN) break;
    BreakdownRow &r = s.breakdown[s.nBreakdown++];
    copyStr(r.name, sizeof r.name, j["name"]);
    r.pct = j["pct"] | 0;
  }
  s.alert = doc["alert"] | false;
  s.alertSeq = doc["alert_seq"] | 0;
}
