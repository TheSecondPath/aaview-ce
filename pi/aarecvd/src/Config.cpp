/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "Config.hpp"
#include "Log.hpp"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aarecvd::config {

namespace {

constexpr size_t MAX_BYTES = 4096;
const char *DEFAULT_LEVEL = "info";

const std::map<std::string, std::string> &levelWords() {
  static const std::map<std::string, std::string> m = {{"error", "error"}, {"err", "error"},
                                                       {"warning", "warning"}, {"warn", "warning"},
                                                       {"info", "info"}, {"debug", "debug"}};
  return m;
}

bool isOn(const std::u32string &v) { return v == U"on" || v == U"yes" || v == U"true" || v == U"1"; }
bool isOff(const std::u32string &v) { return v == U"off" || v == U"no" || v == U"false" || v == U"0"; }

// Whitespace as Python's str.isspace() defines it.
bool pySpace(char32_t c) {
  return c == 0x20 || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
         c == 0x205f || c == 0x3000;
}

std::u32string strip(const std::u32string &s) {
  size_t a = 0, b = s.size();
  while (a < b && pySpace(s[a])) ++a;
  while (b > a && pySpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::u32string lower(std::u32string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  return s;
}

std::string utf8(const std::u32string &s) {
  std::string out;
  for (char32_t c : s) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

std::string ascii(const std::u32string &s) {   // only for words already known to be ASCII
  std::string out;
  for (auto c : s) out += c < 0x80 ? static_cast<char>(c) : '?';
  return out;
}

// A value from the file, made safe to put in a log line.
std::string show(const std::u32string &v) {
  std::string out;
  for (size_t i = 0; i < v.size() && i < 20; ++i) out += (v[i] >= ' ' && v[i] <= '~') ? static_cast<char>(v[i]) : '?';
  return out;
}

bool garbled(const std::u32string &line) {
  for (auto c : line)
    if ((c < ' ' && c != '\t') || c == 0x7f || c == 0xfffd) return true;
  return false;
}

std::u32string decodeUtf16(const std::string &d, bool little) {
  std::u32string out;
  size_t i = 2;                       // after the BOM
  while (i + 1 < d.size()) {
    unsigned a = static_cast<unsigned char>(d[i]), b = static_cast<unsigned char>(d[i + 1]);
    char32_t u = little ? (a | (b << 8)) : ((a << 8) | b);
    i += 2;
    if (u >= 0xd800 && u <= 0xdbff) {
      if (i + 1 < d.size()) {
        unsigned c = static_cast<unsigned char>(d[i]), e = static_cast<unsigned char>(d[i + 1]);
        char32_t l = little ? (c | (e << 8)) : ((c << 8) | e);
        if (l >= 0xdc00 && l <= 0xdfff) {
          out += 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00);
          i += 2;
          continue;
        }
      }
      out += 0xfffd;
    } else if (u >= 0xdc00 && u <= 0xdfff) {
      out += 0xfffd;
    } else {
      out += u;
    }
  }
  if (i < d.size()) out += 0xfffd;   // an odd byte at the end
  return out;
}

std::u32string decodeUtf8(const std::string &d) {
  std::u32string out;
  size_t i = 0;
  if (d.size() >= 3 && d.compare(0, 3, "\xef\xbb\xbf") == 0) i = 3;   // utf-8-sig
  while (i < d.size()) {
    unsigned c = static_cast<unsigned char>(d[i]);
    int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (n < 0 || (n == 1 && c < 0xc2) || (n == 3 && c > 0xf4)) {
      out += 0xfffd;
      ++i;
      continue;
    }
    char32_t u = n == 0 ? c : (c & (0x3f >> n));
    size_t j = 1;
    for (; j <= static_cast<size_t>(n); ++j) {
      if (i + j >= d.size() || (static_cast<unsigned char>(d[i + j]) & 0xc0) != 0x80) break;
      u = (u << 6) | (static_cast<unsigned char>(d[i + j]) & 0x3f);
    }
    if (j <= static_cast<size_t>(n) || (n == 2 && (u < 0x800 || (u >= 0xd800 && u <= 0xdfff))) ||
        (n == 3 && (u < 0x10000 || u > 0x10ffff))) {
      out += 0xfffd;
      i += j;                         // skip the invalid part, roughly as Python's 'replace' does
      continue;
    }
    out += u;
    i += n + 1;
  }
  return out;
}

std::string basename(const std::string &p) {
  auto k = p.find_last_of('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

void fill(Settings &s, const std::string &path) {
  const std::string name = basename(path);
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    int e = errno;
    struct stat st{};
    if (e == ENOENT || e == ENOTDIR || ::stat(path.c_str(), &st) != 0) {
      s.warnings["file"] = log::fmt("no %s: using the defaults (home Wi-Fi off, log level %s)", path.c_str(),
                                    DEFAULT_LEVEL);
    } else {
      s.warnings["file"] = log::fmt("%s cannot be read (%s): using the defaults", path.c_str(), std::strerror(e));
    }
    return;
  }
  std::string data(MAX_BYTES + 1, '\0');
  f.read(&data[0], static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<size_t>(f.gcount()));

  std::vector<std::string> notes;
  const bool tooLong = data.size() > MAX_BYTES;
  data.resize(std::min(data.size(), MAX_BYTES));
  std::u32string text;
  if (data.size() >= 2 && (data.compare(0, 2, "\xff\xfe") == 0 || data.compare(0, 2, "\xfe\xff") == 0))
    text = decodeUtf16(data, data[0] == '\xff');     // what Notepad calls "Unicode"
  else
    text = decodeUtf8(data);
  if (tooLong) {
    auto a = text.rfind(U'\n'), b = text.rfind(U'\r');
    long cut = std::max(a == std::u32string::npos ? -1L : static_cast<long>(a),
                        b == std::u32string::npos ? -1L : static_cast<long>(b));
    text = text.substr(0, static_cast<size_t>(cut + 1));
    notes.push_back(log::fmt("it is longer than %zu KB, the rest is not read", MAX_BYTES / 1024));
  }
  if (strip(text).empty() && !tooLong) {
    s.warnings["file"] = log::fmt("%s is empty: using the defaults", path.c_str());
    return;
  }

  std::vector<int> bad;
  std::map<std::string, std::string> problems;
  // Lines end in CRLF, CR or LF.
  std::vector<std::u32string> lines;
  std::u32string cur;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == U'\r' || text[i] == U'\n') {
      if (text[i] == U'\r' && i + 1 < text.size() && text[i + 1] == U'\n') ++i;
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += text[i];
    }
  }
  lines.push_back(cur);
  int n = 0;
  for (auto line : lines) {
    ++n;
    line = strip(line);
    if (line.empty() || line[0] == U'#') continue;
    auto eq = line.find(U'=');
    if (eq == std::u32string::npos || garbled(line)) {
      bad.push_back(n);
      continue;
    }
    std::u32string key = lower(strip(line.substr(0, eq))), value = strip(line.substr(eq + 1));
    const std::string k = ascii(key);
    if (k != "home_wifi" && k != "home_wifi_ssid" && k != "log_level" && k != "set_clock") continue;
    if (k != "home_wifi_ssid") value = lower(strip(value.substr(0, value.find(U'#'))));   // a Wi-Fi name may hold a #
    s.given.erase(k);
    problems.erase(k);
    if (k == "home_wifi") {
      s.homeWifi = isOn(value);
      if (isOn(value) || isOff(value))
        s.given.insert(k);
      else if (!value.empty())
        problems[k] = log::fmt("home_wifi \"%s\" is not on or off", show(value).c_str());
    } else if (k == "home_wifi_ssid") {
      if (value.size() >= 2 && value.front() == U'"' && value.back() == U'"')
        value = value.substr(1, value.size() - 2);       // quotes keep spaces at the ends: "My Network "
      s.homeWifiSsid.clear();
      const std::string u = utf8(value);
      if (u.size() > 32)
        problems[k] = "home_wifi_ssid is longer than 32 bytes, which no Wi-Fi name is";
      else if (!u.empty()) {
        s.homeWifiSsid = u;
        s.given.insert(k);
      }
    } else if (k == "set_clock") {
      s.setClock = !isOff(value);
      if (isOn(value) || isOff(value))
        s.given.insert(k);
      else if (!value.empty())
        problems[k] = log::fmt("set_clock \"%s\" in %s is not on or off: on", show(value).c_str(), name.c_str());
    } else if (k == "log_level") {
      auto it = levelWords().find(ascii(value));
      bool known = it != levelWords().end() &&
                   std::all_of(value.begin(), value.end(), [](char32_t c) { return c < 0x80; });
      s.logLevel = known ? it->second : DEFAULT_LEVEL;
      if (known)
        s.given.insert(k);
      else if (!value.empty())
        problems[k] = log::fmt("log_level \"%s\" in %s is not error, warning, info or debug", show(value).c_str(),
                               name.c_str());
    }
  }
  if (!bad.empty()) {
    std::string shown;
    for (size_t i = 0; i < bad.size() && i < 5; ++i) shown += (i ? ", " : "") + std::to_string(bad[i]);
    if (bad.size() > 5) shown += log::fmt(" and %zu more", bad.size() - 5);
    notes.push_back(log::fmt("line%s %s not understood (each line is key=value, or starts with #)",
                             bad.size() > 1 ? "s" : "", shown.c_str()));
  }
  if (!notes.empty()) {
    std::string j;
    for (size_t i = 0; i < notes.size(); ++i) j += (i ? "; " : "") + notes[i];
    s.warnings["file"] = path + ": " + j;
  }
  std::vector<std::string> home;
  for (const char *k : {"home_wifi", "home_wifi_ssid"})
    if (problems.count(k)) home.push_back(problems[k]);
  if (s.homeWifi && s.homeWifiSsid.empty()) {
    s.homeWifi = false;
    if (!problems.count("home_wifi_ssid")) home.push_back("home_wifi=on but home_wifi_ssid is blank");
  }
  if (!home.empty()) {
    std::string j;
    for (size_t i = 0; i < home.size(); ++i) j += (i ? "; " : "") + home[i];
    s.warnings["home_wifi"] = name + ": " + j + ": home Wi-Fi stays off";
  }
  if (problems.count("log_level")) s.warnings["log_level"] = problems["log_level"];
  if (problems.count("set_clock")) s.warnings["set_clock"] = problems["set_clock"];
}

}  // namespace

Settings readSettings(const std::string &path) {
  Settings s;
  s.path = path;
  try {
    fill(s, path);
  } catch (const std::exception &e) {   // whatever it is, the service must keep running
    s = Settings();
    s.path = path;
    s.warnings["file"] = log::fmt("%s could not be read (%s): using the defaults", path.c_str(), e.what());
  }
  return s;
}

LevelChoice logLevelOf(const Settings &settings, const std::string &legacy) {
  if (settings.given.count("log_level")) return {settings.logLevel, basename(settings.path), ""};
  std::ifstream f(legacy, std::ios::binary);
  if (!f) return {DEFAULT_LEVEL, "the default", ""};
  std::string data(64, '\0');
  f.read(&data[0], 64);
  data.resize(static_cast<size_t>(f.gcount()));
  for (auto &c : data)
    if (static_cast<unsigned char>(c) >= 0x80) c = '?';   // non-ASCII bytes show as ?
  std::istringstream in(data);
  std::string w;
  if (!(in >> w)) return {DEFAULT_LEVEL, "the default", ""};
  std::string lw = w;
  for (auto &c : lw) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto it = levelWords().find(lw);
  if (it == levelWords().end()) {
    std::u32string u(w.begin(), w.end());
    return {DEFAULT_LEVEL, "the default",
            log::fmt("\"%s\" in %s is not error, warning, info or debug", show(u).c_str(), legacy.c_str())};
  }
  return {it->second, legacy, ""};
}

int levelNumber(const std::string &word) {
  if (word == "error") return 3;
  if (word == "warning") return 4;
  if (word == "debug") return 7;
  return 6;
}

std::string iwEscape(const std::string &ssid) {
  std::string out;
  for (size_t i = 0; i < ssid.size(); ++i) {
    unsigned char b = static_cast<unsigned char>(ssid[i]);
    if ((b >= 0x21 && b <= 0x7e && b != 0x5c) || (b == 0x20 && i > 0 && i + 1 < ssid.size()))
      out += static_cast<char>(b);
    else
      out += log::fmt("\\x%02x", b);
  }
  return out;
}

int showSettings(const std::string &path, const std::string &legacy, bool shell, bool dump) {
  const Settings s = readSettings(path);
  const LevelChoice lc = logLevelOf(s, legacy);
  auto warning = [&s](const char *k) {
    auto it = s.warnings.find(k);
    return it == s.warnings.end() ? std::string() : it->second;
  };
  std::string logWarning = warning("log_level");
  if (!lc.problem.empty()) logWarning += (logWarning.empty() ? "" : "; ") + lc.problem;
  std::vector<std::string> lines;
  if (dump) {                       // the desk test's view: what the file set, before the level is chosen
    std::string given;
    for (auto &g : s.given) given += (given.empty() ? "" : ",") + g;
    lines = {"home_wifi=" + std::string(s.homeWifi ? "on" : "off"), "home_wifi_ssid=" + s.homeWifiSsid,
             "file_log_level=" + s.logLevel, "set_clock=" + std::string(s.setClock ? "on" : "off"),
             "given=" + given, "level=" + lc.word, "level_source=" + lc.source, "level_problem=" + lc.problem};
    for (auto &w : s.warnings) lines.push_back("warning." + w.first + "=" + w.second);
  } else if (shell) {
    lines = {"home_wifi=" + std::string(s.homeWifi ? "on" : "off"),
             "home_wifi_ssid=" + s.homeWifiSsid,
             "home_wifi_ssid_iw=" + iwEscape(s.homeWifiSsid),
             "log_level=" + lc.word,
             "log_level_num=" + std::to_string(levelNumber(lc.word)),
             "set_clock=" + std::string(s.setClock ? "on" : "off"),
             "warn_file=" + warning("file"),
             "warn_home=" + warning("home_wifi"),
             "warn_log=" + logWarning};
  } else {
    lines = {"settings file: " + path,
             "home Wi-Fi:    " + (s.homeWifi ? "on, network \"" + s.homeWifiSsid + "\"" : std::string("off")),
             "log level:     " + lc.word + " (from " + lc.source + ")",
             "set clock:     " + std::string(s.setClock ? "on" : "off")};
    for (const std::string &w : {warning("file"), warning("home_wifi"), logWarning, warning("set_clock")})
      if (!w.empty()) lines.push_back("warning: " + w);
  }
  std::string out;
  for (auto &l : lines) out += l + "\n";
  return std::fwrite(out.data(), 1, out.size(), stdout) == out.size() && std::fflush(stdout) == 0 ? 0 : 1;
}

std::vector<std::optional<std::pair<long long, long long>>> stamp(const std::vector<std::string> &paths) {
  std::vector<std::optional<std::pair<long long, long long>>> out;
  for (auto &p : paths) {
    struct stat st{};
    if (::stat(p.c_str(), &st) == 0)
      out.emplace_back(std::make_pair(static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec,
                                      static_cast<long long>(st.st_size)));
    else
      out.emplace_back(std::nullopt);
  }
  return out;
}

void readKeyValues(const std::string &path,
                   const std::function<void(const std::string &key, const std::string &value)> &use) {
  auto trim = [](const std::string &s) {
    auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
  };
  std::ifstream f(path);
  std::string ln;
  while (f && std::getline(f, ln)) {
    auto first = ln.find_first_not_of(" \t");
    if (first == std::string::npos || ln[first] == '#') continue;
    auto eq = ln.find('=');
    if (eq == std::string::npos) continue;
    use(trim(ln.substr(0, eq)), trim(ln.substr(eq + 1)));
  }
}

bool apPeer(const std::string &peerIp, const std::string &confPath) {
  std::string ip;
  readKeyValues(confPath, [&ip](const std::string &key, const std::string &value) {
    if (key == "ip") ip = value;               // only ip= is used; nothing else in this file is kept
  });
  in_addr a{}, b{};
  if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &a) != 1 || inet_pton(AF_INET, peerIp.c_str(), &b) != 1)
    return false;
  return (ntohl(a.s_addr) & 0xffffff00u) == (ntohl(b.s_addr) & 0xffffff00u);
}

// Steering wheel and dash keys: LG key code -> AA key code.
HardKeys::HardKeys()
    : map_{{301, 0x57}, {302, 0x58}, {303, 0x55}, {307, 0x54}, {308, 0x06}, {309, 0x05}, {19, 0x57}, {20, 0x58},
           {1, 0x7f}, {2, 0x7f}, {3, 0x7f}, {5, 0x7f}} {}

void HardKeys::loadOverrides(const std::string &path) {
  map_ = HardKeys().map_;                      // so removed lines go back to the default
  std::ifstream f(path);
  std::string ln;
  while (f && std::getline(f, ln)) {
    ln = ln.substr(0, ln.find('#'));
    auto eq = ln.find('=');
    if (eq == std::string::npos) continue;
    auto k = pyInt0(ln.substr(0, eq)), v = pyInt0(ln.substr(eq + 1));
    if (!k || !v) return;                      // a bad number ends the reading; later lines are ignored
    if (*v)
      map_[static_cast<int>(*k)] = static_cast<int>(*v);
    else
      map_.erase(static_cast<int>(*k));
  }
}

std::optional<int> HardKeys::lookup(int lgKey) const {
  auto it = map_.find(lgKey);
  if (it == map_.end()) return std::nullopt;
  return it->second;
}

std::optional<int> virtualKeyToAA(int vk) {
  static const std::map<int, int> m = {{0x25, 0x15}, {0x26, 0x13}, {0x27, 0x16}, {0x28, 0x14}, {0x0D, 0x17},
                                       {0x1B, 0x04}, {0x08, 0x04}, {0x24, 0x03},
                                       {0xB0, 0x57}, {0xB1, 0x58}, {0xB3, 0x55}, {0xB2, 0x7F}};
  auto it = m.find(vk);
  if (it == m.end()) return std::nullopt;
  return it->second;
}

std::optional<long long> pyInt0(const std::string &text) {
  auto b = text.find_first_not_of(" \t\r\n\v\f"), e = text.find_last_not_of(" \t\r\n\v\f");
  if (b == std::string::npos) return std::nullopt;
  std::string s = text.substr(b, e - b + 1);
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = s[0] == '-';
    s = s.substr(1);
  }
  int base = 10;
  if (s.size() > 2 && s[0] == '0' && std::strchr("xXoObB", s[1])) {
    base = (s[1] == 'x' || s[1] == 'X') ? 16 : (s[1] == 'o' || s[1] == 'O') ? 8 : 2;
    s = s.substr(2);
  } else if (s.size() > 1 && s[0] == '0') {
    if (s.find_first_not_of("0_") != std::string::npos) return std::nullopt;   // int('010', 0) is an error
  }
  if (s.empty()) return std::nullopt;
  long long v = 0;
  for (char c : s) {
    if (c == '_') continue;
    int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
            : std::isalpha(static_cast<unsigned char>(c)) ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                                                          : 99;
    if (d >= base) return std::nullopt;
    v = v * base + d;
  }
  return neg ? -v : v;
}

}  // namespace aarecvd::config
