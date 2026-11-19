/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "Log.hpp"
#include "Config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace aarecvd::log {

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point T0 = Clock::now();

std::mutex outMutex, levelMutex;
Paths paths;
int maxLevel = 6;
double checked = -1e9;
bool haveStamp = false;
std::vector<std::optional<std::pair<long long, long long>>> lastStamp;
std::vector<std::string> warned;
bool setClock_ = true;
const bool prefixed = std::getenv("JOURNAL_STREAM") != nullptr;

double now() { return std::chrono::duration<double>(Clock::now() - T0).count(); }

}  // namespace

void init(const Paths &p) {
  std::lock_guard<std::mutex> l(levelMutex);
  paths = p;
  checked = -1e9;
  haveStamp = false;
}

int level() {
  std::vector<std::pair<int, std::string>> pending;
  int result;
  {
    std::lock_guard<std::mutex> l(levelMutex);
    double t = now();
    if (t - checked >= 5) {
      checked = t;
      auto st = config::stamp({paths.settings, paths.loglevelFile});
      if (!haveStamp || st != lastStamp) {
        bool first = !haveStamp;
        haveStamp = true;
        lastStamp = st;
        auto settings = config::readSettings(paths.settings);
        auto choice = config::logLevelOf(settings, paths.loglevelFile);
        int was = maxLevel, now_ = config::levelNumber(choice.word);
        maxLevel = now_;
        setClock_ = settings.setClock;
        // Say what is wrong with the file once, and again only if it changes.
        std::vector<std::string> w;
        for (const char *k : {"file", "log_level"})
          if (settings.warnings.count(k)) w.push_back(settings.warnings.at(k));
        if (!choice.problem.empty()) w.push_back(choice.problem);
        if (settings.warnings.count("set_clock")) w.push_back(settings.warnings.at("set_clock"));
        if (w != warned) {
          warned = w;
          for (auto &x : w) pending.emplace_back(4, "settings: " + x);
        }
        if (first || now_ != was)
          // Logged at the lower of the old and new levels, so the journal shows where the chatter stops.
          pending.emplace_back(std::min({was, now_, 6}),
                               fmt("log: level %s (from %s)", choice.word.c_str(), choice.source.c_str()));
      }
    }
    result = maxLevel;
  }
  for (auto &p : pending)
    if (p.first <= result) write(p.first, p.second);
  return result;
}

bool setClock() {
  level();
  std::lock_guard<std::mutex> l(levelMutex);
  return setClock_;
}

void write(int lvl, const std::string &msg) {
  std::lock_guard<std::mutex> l(outMutex);
  if (prefixed)
    std::printf("<%d>%8.3f %s\n", lvl, now(), msg.c_str());
  else
    std::printf("%8.3f %s\n", now(), msg.c_str());
  std::fflush(stdout);
}

std::string fmt(const char *f, ...) {
  va_list ap, ap2;
  va_start(ap, f);
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, f, ap);
  va_end(ap);
  std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
  if (n > 0) std::vsnprintf(&out[0], out.size() + 1, f, ap2);
  va_end(ap2);
  return out;
}

}  // namespace aarecvd::log
