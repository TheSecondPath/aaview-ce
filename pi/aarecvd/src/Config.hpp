/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The files the receiver reads: veloaa.txt, the settings a user edits (aa-ap and install.sh read it through
// 'aarecvd --show-settings --shell'), /etc/aarecv/loglevel, the ip= line of /etc/aarecv.conf for the access point
// check, and the key overrides in /etc/aarecv-keys.conf. Nothing here throws or writes.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aarecvd::config {

// /boot/firmware/veloaa.txt is on the SD card's FAT boot partition, so it can be edited on any PC. It has one
// key=value per line, and lines starting with # are comments. Case and spaces around the = do not matter, and
// unknown keys are skipped silently. At most 4 KB is read.
//   home_wifi       off | on    (default off)    join the home network named below when it is in range
//   home_wifi_ssid  <name>      (default blank)  its name (its password is NOT kept here: NetworkManager has it)
//   log_level       error | warning | info | debug   (default info)
//   set_clock       on | off    (default on)     set the head unit's clock from the Pi's (only while the Pi's own
//                                                clock is set from the internet)
// Anything wrong (no file, an empty or garbled file, a line without =, an unknown value) leaves that key at its
// default and is reported in warnings, for the caller to log once.
struct Settings {
  std::string path;
  bool homeWifi = false;                         // false whenever the name is blank
  std::string homeWifiSsid;                      // UTF-8, at most 32 bytes
  std::string logLevel = "info";
  bool setClock = true;
  std::set<std::string> given;                   // keys the file validly set
  std::map<std::string, std::string> warnings;   // 'file', 'home_wifi', 'log_level' or 'set_clock' -> one line
};

// Never throws.
Settings readSettings(const std::string &path);

struct LevelChoice {
  std::string word, source, problem;            // problem is empty when there is none
};
// The level in force: veloaa.txt's log_level if it validly sets one, else the one word in /etc/aarecv/loglevel
// (for development, changed over SSH without a reboot), else info.
LevelChoice logLevelOf(const Settings &settings, const std::string &legacy);
int levelNumber(const std::string &word);       // error 3, warning 4, info 6, debug 7 (syslog priorities)

// The SSID as `iw dev ... scan` prints it (iw's print_ssid_escaped): bytes outside printable ASCII, a backslash,
// and a space at either end come out as \xNN.
std::string iwEscape(const std::string &ssid);

// 'aarecvd --show-settings': the settings in force, in words. With shell, key=value lines for aa-ap and install.sh,
// which parse them with sed and never eval them: home_wifi, home_wifi_ssid, home_wifi_ssid_iw, log_level,
// log_level_num, set_clock, warn_file, warn_home, warn_log. With dump, every field, for the desk test. Returns the
// exit code.
int showSettings(const std::string &path, const std::string &legacy, bool shell, bool dump);

// Modification time and size of each file, to notice when one changes.
std::vector<std::optional<std::pair<long long, long long>>> stamp(const std::vector<std::string> &paths);

// Calls use(key, value) for each key=value line, skipping blank lines and # comments.
void readKeyValues(const std::string &path,
                   const std::function<void(const std::string &key, const std::string &value)> &use);

// Whether this IPv4 client is on the /24 around ip= in /etc/aarecv.conf.
bool apPeer(const std::string &peerIp, const std::string &confPath);

// LG hard key codes -> AA key codes. Overrides from the keys file stay in effect for the life of the process.
class HardKeys {
public:
  HardKeys();
  void loadOverrides(const std::string &path);
  std::optional<int> lookup(int lgKey) const;

private:
  std::map<int, int> map_;
};

// Windows virtual key codes from the head unit -> AA key codes.
std::optional<int> virtualKeyToAA(int vk);

// Parses a number as Python's int(text, 0) does (0x/0o/0b prefixes, underscores), for the key overrides file.
std::optional<long long> pyInt0(const std::string &text);

}  // namespace aarecvd::config
