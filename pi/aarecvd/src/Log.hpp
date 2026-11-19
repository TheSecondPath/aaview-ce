/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Log lines on stdout: "<prio>   12.345 text" under systemd (JOURNAL_STREAM set), else "   12.345 text". The level
// in force comes from, in this order: log_level= in /boot/firmware/veloaa.txt, one word in /etc/aarecv/loglevel,
// else info. Both files are looked at again every 5 s.
#pragma once

#include <string>

namespace aarecvd::log {

struct Paths {
  std::string settings = "/boot/firmware/veloaa.txt";
  std::string loglevelFile = "/etc/aarecv/loglevel";
};

void init(const Paths &paths);
// The syslog priority in force (3 error, 4 warning, 6 info, 7 debug). Re-reads the files at most every 5 s.
int level();
// veloaa.txt's set_clock: whether to set the head unit's clock (header fields at bytes 44/48).
bool setClock();
inline bool enabled(int lvl) { return lvl <= level(); }
void write(int lvl, const std::string &msg);
std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));

}  // namespace aarecvd::log

#define AA_LOG(lvl, ...)                                                                  \
  do {                                                                                    \
    if (::aarecvd::log::enabled(lvl)) ::aarecvd::log::write(lvl, ::aarecvd::log::fmt(__VA_ARGS__)); \
  } while (0)
#define AA_DBG(...) AA_LOG(7, __VA_ARGS__)
#define AA_INFO(...) AA_LOG(6, __VA_ARGS__)
#define AA_WARN(...) AA_LOG(4, __VA_ARGS__)
#define AA_ERR(...) AA_LOG(3, __VA_ARGS__)
