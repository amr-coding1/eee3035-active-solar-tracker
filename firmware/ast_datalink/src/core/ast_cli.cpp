#include "ast_cli.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "ast_config.h"
#include "ast_time.h"

namespace ast {

const char* cal_channel_name(CalChannel c) {
  switch (c) {
    case CalChannel::kTrk: return "trk";
    case CalChannel::kFix: return "fix";
    case CalChannel::kUse: return "use";
  }
  return "?";
}

const char* cli_help() {
  return "AST data link - commands (contract 7.6)\n"
         "  help                         this text\n"
         "  status                       state, faults, Wi-Fi, last upload\n"
         "  time                         show UTC time and its source\n"
         "  time <YYYY-MM-DDTHH:MM:SSZ>  set the clock by hand (UTC)\n"
         "  buffer                       ring buffer counters\n"
         "  buffer dump [n]              newest n records as CSV (default 20)\n"
         "  buffer clear yes             erase stored records (seq is kept)\n"
         "  upload                       upload now\n"
         "  upload every <5-60>          upload interval in minutes\n"
         "  run <0-999>                  test run id (0 none, 900-999 calibration)\n"
         "  panel A|B                    panel on the tracker\n"
         "  sim on [clear|cloudy|overcast|night|lamp] | sim off\n"
         "  cal                          show sensor readings and calibration\n"
         "  cal <trk|fix|use> <v|i> <1|2> <DMM value>   two-point cal (V or mA)\n"
         "  cal <trk|fix|use> <v|i> reset\n"
         "  cal <trk|fix|use> shunt <ohms> [current LSB mA]\n"
         "  wifi <ssid> <pass>           2.4 GHz hotspot (use \"quotes\" for spaces)\n"
         "  keys <ch1> <write> [read]    ThingSpeak channel id and API keys\n"
         "  power                        CPU MHz, Wi-Fi state, radio-on time today\n"
         "  reboot\n";
}

bool cli_tokenize(const std::string& line, std::vector<std::string>* words) {
  words->clear();
  std::string cur;
  bool in_word = false, in_quote = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (in_quote) {
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
        cur.push_back(line[++i]);
      } else if (c == '"') {
        in_quote = false;
      } else {
        cur.push_back(c);
      }
    } else if (c == '"') {
      in_quote = true;
      in_word = true;   // "" is an empty word
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (in_word) { words->push_back(cur); cur.clear(); in_word = false; }
    } else {
      cur.push_back(c);
      in_word = true;
    }
  }
  if (in_quote) return false;
  if (in_word) words->push_back(cur);
  return true;
}

namespace {

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool parse_uint(const std::string& s, uint32_t max, uint32_t* out) {
  if (s.empty() || s.size() > 9) return false;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint32_t>(c - '0');
  }
  if (v > max) return false;
  *out = v;
  return true;
}

bool parse_double(const std::string& s, double* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double v = strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || errno != 0 || !isfinite(v)) return false;
  *out = v;
  return true;
}

Command err(const std::string& text) {
  Command c;
  c.type = CmdType::kError;
  c.error = text;
  return c;
}

bool is_printable(const std::string& s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) return false;
  }
  return true;
}

bool is_key_chars(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

Command parse_cal(const std::vector<std::string>& w) {
  Command c;
  if (w.size() == 1) { c.type = CmdType::kCalShow; return c; }
  const std::string ch = lower(w[1]);
  if (ch == "show") { c.type = CmdType::kCalShow; return c; }
  if (ch == "trk") c.cal_channel = CalChannel::kTrk;
  else if (ch == "fix") c.cal_channel = CalChannel::kFix;
  else if (ch == "use") c.cal_channel = CalChannel::kUse;
  else return err("cal: channel must be trk, fix or use");
  if (w.size() < 4) return err("usage: cal <trk|fix|use> <v|i> <1|2> <DMM value> | <v|i> reset | shunt <ohms> [lsb mA]");
  const std::string what = lower(w[2]);
  if (what == "shunt") {
    if (w.size() > 5 || !parse_double(w[3], &c.value) || c.value < 0.001 || c.value > 10.0) {
      return err("cal: shunt must be 0.001 to 10 ohm");
    }
    if (w.size() == 5 && (!parse_double(w[4], &c.value2) || c.value2 < 0.001 || c.value2 > 10.0)) {
      return err("cal: current LSB must be 0.001 to 10 mA");
    }
    c.type = CmdType::kCalShunt;
    return c;
  }
  if (what == "v") c.cal_is_current = false;
  else if (what == "i") c.cal_is_current = true;
  else return err("cal: quantity must be v (volts) or i (milliamps)");
  if (lower(w[3]) == "reset") {
    if (w.size() != 4) return err("usage: cal <ch> <v|i> reset");
    c.type = CmdType::kCalReset;
    return c;
  }
  if (w.size() != 5) return err("usage: cal <ch> <v|i> <1|2> <DMM value>");
  if (w[3] == "1") c.cal_point = 1;
  else if (w[3] == "2") c.cal_point = 2;
  else return err("cal: point must be 1 or 2");
  if (!parse_double(w[4], &c.value) || c.value < -100000.0 || c.value > 100000.0) {
    return err("cal: DMM value is not a number");
  }
  c.type = CmdType::kCalPoint;
  return c;
}

}  // namespace

Command cli_parse(const std::string& line) {
  std::vector<std::string> w;
  if (!cli_tokenize(line, &w)) return err("unterminated quote");
  Command c;
  if (w.empty()) return c;  // kEmpty
  const std::string cmd = lower(w[0]);
  const size_t n = w.size();

  if (cmd == "help" || cmd == "?") {
    c.type = CmdType::kHelp;
  } else if (cmd == "status") {
    if (n != 1) return err("usage: status");
    c.type = CmdType::kStatus;
  } else if (cmd == "time") {
    if (n == 1) { c.type = CmdType::kTime; return c; }
    // Allow both "time 2026-11-20T10:00:00Z" and "time 2026-11-20 10:00:00".
    std::string iso = w[1];
    if (n == 3) iso += "T" + w[2];
    if (n > 3 || !parse_iso(iso.c_str(), &c.unix_time)) return err("usage: time <YYYY-MM-DDTHH:MM:SSZ> (UTC)");
    if (c.unix_time < kMinPlausibleUnix) return err("time: date is before 2026");
    c.type = CmdType::kTimeSet;
  } else if (cmd == "buffer") {
    if (n == 1) { c.type = CmdType::kBuffer; return c; }
    const std::string sub = lower(w[1]);
    if (sub == "dump") {
      c.num = 20;
      if (n > 3 || (n == 3 && (!parse_uint(w[2], kRingCapacity, &c.num) || c.num == 0))) {
        return err("usage: buffer dump [1-11520]");
      }
      c.type = CmdType::kBufferDump;
    } else if (sub == "clear") {
      if (n != 3 || lower(w[2]) != "yes") return err("this erases stored samples: type 'buffer clear yes'");
      c.type = CmdType::kBufferClear;
    } else {
      return err("usage: buffer | buffer dump [n] | buffer clear yes");
    }
  } else if (cmd == "upload") {
    if (n == 1 || (n == 2 && lower(w[1]) == "now")) { c.type = CmdType::kUploadNow; return c; }
    if (n == 3 && lower(w[1]) == "every") {
      if (!parse_uint(w[2], 1000, &c.num) || c.num < static_cast<uint32_t>(kUploadMinLow) ||
          c.num > static_cast<uint32_t>(kUploadMinHigh)) {
        return err("upload every: 5 to 60 minutes");
      }
      c.type = CmdType::kUploadEvery;
      return c;
    }
    return err("usage: upload | upload every <5-60>");
  } else if (cmd == "run") {
    if (n != 2 || !parse_uint(w[1], kRunMax, &c.num)) return err("usage: run <0-999>");
    c.type = CmdType::kRun;
  } else if (cmd == "panel") {
    if (n != 2 || w[1].size() != 1) return err("usage: panel A|B");
    const char p = w[1][0];
    if (p == 'A' || p == 'a') c.panel = 'A';
    else if (p == 'B' || p == 'b') c.panel = 'B';
    else return err("usage: panel A|B");
    c.type = CmdType::kPanel;
  } else if (cmd == "sim") {
    if (n < 2 || n > 3) return err("usage: sim on [scenario] | sim off");
    const std::string sw = lower(w[1]);
    if (sw == "on") c.flag = true;
    else if (sw == "off") c.flag = false;
    else return err("usage: sim on [scenario] | sim off");
    if (n == 3) {
      if (!c.flag || !scenario_from_name(lower(w[2]).c_str(), &c.scenario)) {
        return err("sim: scenario must be clear, cloudy, overcast, night or lamp");
      }
      c.has_scenario = true;
    }
    c.type = CmdType::kSim;
  } else if (cmd == "cal") {
    return parse_cal(w);
  } else if (cmd == "wifi") {
    if (n != 3) return err("usage: wifi <ssid> <pass>  (use \"quotes\" if there are spaces)");
    if (w[1].empty() || w[1].size() > 32 || !is_printable(w[1])) return err("wifi: SSID must be 1-32 characters");
    // WPA2 passphrase: 8-63 characters; empty = open network.
    if (!(w[2].empty() || (w[2].size() >= 8 && w[2].size() <= 63)) || !is_printable(w[2])) {
      return err("wifi: password must be 8-63 characters (or \"\" for an open network)");
    }
    c.s1 = w[1];
    c.s2 = w[2];
    c.type = CmdType::kWifi;
  } else if (cmd == "keys") {
    if (n < 3 || n > 4) return err("usage: keys <ch1> <write> [read]");
    uint32_t ch = 0;
    if (!parse_uint(w[1], 999999999u, &ch) || ch == 0) return err("keys: channel id must be a number");
    if (!is_key_chars(w[2])) return err("keys: write key must be letters and digits");
    if (n == 4 && !is_key_chars(w[3])) return err("keys: read key must be letters and digits");
    c.s1 = w[1];
    c.s2 = w[2];
    if (n == 4) c.s3 = w[3];
    c.num = ch;
    c.type = CmdType::kKeys;
  } else if (cmd == "power") {
    if (n != 1) return err("usage: power");
    c.type = CmdType::kPower;
  } else if (cmd == "reboot") {
    if (n != 1) return err("usage: reboot");
    c.type = CmdType::kReboot;
  } else {
    return err("unknown command '" + w[0] + "' (try: help)");
  }
  return c;
}

bool LineReader::feed(char c, std::string* line) {
  if (c == '\n' && last_was_cr_) {  // second half of CRLF
    last_was_cr_ = false;
    return false;
  }
  last_was_cr_ = (c == '\r');
  if (c == '\r' || c == '\n') {
    const bool had_overflow = overflow_;
    overflow_ = false;
    if (had_overflow) {
      buf_.clear();
      return false;   // an over-long line is thrown away, never half-executed
    }
    *line = buf_;
    buf_.clear();
    return true;
  }
  if (c == '\b' || c == 0x7F) {
    if (!buf_.empty()) buf_.pop_back();
    return false;
  }
  if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return false;
  if (buf_.size() >= max_len_) {
    overflow_ = true;
    return false;
  }
  buf_.push_back(c);
  return false;
}

}  // namespace ast
