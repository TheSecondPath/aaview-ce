/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// `aarecvd --bt-only`: the Bluetooth start-up on its own, in a process next to the session one, as OpenAuto runs
// btservice next to autoapp. The session process sends one line per datagram on /run/veloaa/aarecv.sock:
// "session up" or "session ended" (repeated every 10 s), "icon <state>" for a tap on aaview's icon with no session,
// and "pair on" / "pair off" for a 3 s hold on it.
#pragma once

#include <string>

#include "Bluetooth.hpp"

namespace aarecvd::bt {

// Runs until SIGTERM or SIGINT and returns the exit code. With noBluetooth (--no-bt) it only listens.
int runBtOnly(const Options &options, const std::string &notifyPath, bool noBluetooth);

}  // namespace aarecvd::bt
