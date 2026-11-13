// Serial CLI parser (contract 7.6), independent of Serial: text line in,
// Command struct out. The ESP32 sketch and the tests share this code.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "ast_sim.h"

namespace ast {

enum class CmdType {
  kEmpty,        // blank line
  kError,        // see Command::error
  kHelp,
  kStatus,
  kTime,         // show
  kTimeSet,      // time <ISO-8601 UTC>           -> unix
  kBuffer,       // show
  kBufferDump,   // buffer dump [n]               -> num (default 20)
  kBufferClear,  // buffer clear yes
  kUploadNow,    // upload | upload now
  kUploadEvery,  // upload every <5-60>           -> num
  kRun,          // run <0-999>                   -> num
  kPanel,        // panel A|B                     -> panel
  kSim,          // sim on [scenario] | sim off   -> flag, scenario
  kCalShow,      // cal
  kCalPoint,     // cal <trk|fix|use> <v|i> <1|2> <dmm value>   (V or mA)
  kCalReset,     // cal <trk|fix|use> <v|i> reset
  kCalShunt,     // cal <trk|fix|use> shunt <ohms> [current LSB in mA]
  kWifi,         // wifi <ssid> <pass>            -> s1, s2 (quotes allowed)
  kKeys,         // keys <ch1> <write> [read]     -> s1, s2, s3
  kReboot,
  kPower         // power: CPU MHz, Wi-Fi state, radio-on time today
};

enum class CalChannel : uint8_t { kTrk = 0, kFix = 1, kUse = 2 };
const char* cal_channel_name(CalChannel c);

struct Command {
  CmdType type = CmdType::kEmpty;
  std::string error;       // kError: text for the operator
  uint32_t num = 0;
  int64_t unix_time = 0;
  bool flag = false;
  char panel = 'A';
  bool has_scenario = false;
  Scenario scenario = Scenario::kClear;
  CalChannel cal_channel = CalChannel::kTrk;
  bool cal_is_current = false;   // false: voltage (V), true: current (mA)
  int cal_point = 0;             // 1 or 2
  double value = 0;              // DMM value / shunt ohms
  double value2 = 0;             // current LSB (mA), 0 = keep
  std::string s1, s2, s3;
};

// Splits a line into words. Double quotes group words ("My Phone"); inside
// quotes \" and \\ are literal. Returns false on an unterminated quote.
bool cli_tokenize(const std::string& line, std::vector<std::string>* words);

Command cli_parse(const std::string& line);
const char* cli_help();

// Collects characters into lines: CR, LF or CRLF end a line, backspace/DEL
// edit, other control characters are ignored, over-long lines are discarded.
class LineReader {
 public:
  explicit LineReader(size_t max_len = 160) : max_len_(max_len) {}
  // Returns true when a complete line is available in *line.
  bool feed(char c, std::string* line);

 private:
  size_t max_len_;
  std::string buf_;
  bool overflow_ = false;
  bool last_was_cr_ = false;
};

}  // namespace ast
