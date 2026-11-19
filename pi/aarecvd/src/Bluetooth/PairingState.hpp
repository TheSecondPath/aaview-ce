/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Bluetooth pairing state, passed from aarecvd --bt-only, which does the pairing, to aarecvd, which owns the stream
// header that tells aaview. It is a small file, /run/veloaa/pairing, that exists only while pairing is open:
//
//   until <seconds>     when pairing closes by itself, in CLOCK_MONOTONIC seconds (the same in both processes)
//   passkey <n>         the 6-digit code of a phone that is pairing, or 0 if there is none yet
//
// A file left behind by a --bt-only that died is ignored once its "until" has passed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#include <unistd.h>

namespace aarecvd::bt {

inline double monotonicSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

struct PairingState {
  bool open = false;
  uint32_t passkey = 0;
};

// Written whole and renamed into place, so the reader never sees half a file.
inline bool writePairingState(const std::string &path, double until, uint32_t passkey) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "until " << static_cast<long long>(until) << "\npasskey " << passkey << "\n";
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline void clearPairingState(const std::string &path) { ::unlink(path.c_str()); }

inline PairingState readPairingState(const std::string &path) {
  PairingState st;
  std::ifstream f(path);
  if (!f) return st;
  std::string key;
  long long until = 0, passkey = 0;
  while (f >> key) {
    if (key == "until") f >> until;
    else if (key == "passkey") f >> passkey;
    else break;
  }
  if (until <= 0 || static_cast<double>(until) < monotonicSeconds()) return st;
  st.open = true;
  st.passkey = passkey > 0 && passkey <= 999999 ? static_cast<uint32_t>(passkey) : 0;
  return st;
}

}  // namespace aarecvd::bt
