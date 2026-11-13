#include "ast_time.h"

namespace ast {

// Algorithms after Howard Hinnant's "chrono-compatible low-level date algorithms".
int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u +
                       static_cast<unsigned>(d) - 1u;                   // [0, 365]
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

CivilTime civil_from_unix(int64_t t) {
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0) { rem += 86400; days -= 1; }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
  CivilTime c;
  c.year = static_cast<int>(y + (m <= 2u));
  c.month = static_cast<int>(m);
  c.day = static_cast<int>(d);
  c.hour = static_cast<int>(rem / 3600);
  c.minute = static_cast<int>((rem % 3600) / 60);
  c.second = static_cast<int>(rem % 60);
  return c;
}

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool civil_is_valid(const CivilTime& c) {
  static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (c.month < 1 || c.month > 12 || c.day < 1) return false;
  int max_day = dim[c.month - 1] + ((c.month == 2 && is_leap(c.year)) ? 1 : 0);
  if (c.day > max_day) return false;
  if (c.hour < 0 || c.hour > 23 || c.minute < 0 || c.minute > 59) return false;
  if (c.second < 0 || c.second > 59) return false;
  return true;
}

int64_t unix_from_civil(const CivilTime& c) {
  return days_from_civil(c.year, c.month, c.day) * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
}

static void put2(char* p, int v) {
  p[0] = static_cast<char>('0' + (v / 10) % 10);
  p[1] = static_cast<char>('0' + v % 10);
}
static void put4(char* p, int v) {
  if (v < 0) v = 0;
  if (v > 9999) v = 9999;
  p[0] = static_cast<char>('0' + (v / 1000) % 10);
  p[1] = static_cast<char>('0' + (v / 100) % 10);
  put2(p + 2, v % 100);
}

void format_created_at(int64_t unix_s, char* out) {
  const CivilTime c = civil_from_unix(minute_floor(unix_s));
  // "YYYY-MM-DD HH:MM:00 +0000"
  put4(out, c.year); out[4] = '-';
  put2(out + 5, c.month); out[7] = '-';
  put2(out + 8, c.day); out[10] = ' ';
  put2(out + 11, c.hour); out[13] = ':';
  put2(out + 14, c.minute); out[16] = ':';
  out[17] = '0'; out[18] = '0'; out[19] = ' ';
  out[20] = '+'; out[21] = '0'; out[22] = '0'; out[23] = '0'; out[24] = '0';
  out[25] = '\0';
}

void format_iso(int64_t unix_s, char* out) {
  const CivilTime c = civil_from_unix(unix_s);
  put4(out, c.year); out[4] = '-';
  put2(out + 5, c.month); out[7] = '-';
  put2(out + 8, c.day); out[10] = 'T';
  put2(out + 11, c.hour); out[13] = ':';
  put2(out + 14, c.minute); out[16] = ':';
  put2(out + 17, c.second); out[19] = 'Z';
  out[20] = '\0';
}

static bool digits(const char* s, int n, int* v) {
  int acc = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    acc = acc * 10 + (s[i] - '0');
  }
  *v = acc;
  return true;
}

bool parse_iso(const char* s, int64_t* unix_s) {
  if (s == nullptr) return false;
  size_t len = 0;
  while (s[len] != '\0' && len < 32) ++len;
  if (len < 16) return false;
  CivilTime c{};
  if (!digits(s, 4, &c.year) || s[4] != '-' || !digits(s + 5, 2, &c.month) || s[7] != '-' ||
      !digits(s + 8, 2, &c.day) || (s[10] != 'T' && s[10] != ' ' && s[10] != 't') ||
      !digits(s + 11, 2, &c.hour) || s[13] != ':' || !digits(s + 14, 2, &c.minute)) {
    return false;
  }
  size_t pos = 16;
  if (s[pos] == ':') {
    if (len < 19 || !digits(s + 17, 2, &c.second)) return false;
    pos = 19;
  }
  if (s[pos] == 'Z' || s[pos] == 'z') ++pos;
  if (s[pos] != '\0') return false;
  if (!civil_is_valid(c)) return false;
  *unix_s = unix_from_civil(c);
  return true;
}

}  // namespace ast
