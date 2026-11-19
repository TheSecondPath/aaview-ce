/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Reads aaview's status sector on its own thread for the life of the program. Every 5 ms during a session (100 ms
// without one) it takes the events aaview reported (touches, keys, steering-wheel and dash keys, headlight changes,
// taps on its floating icon) and hands them to the session's handler. Without a session, headlight changes are
// remembered and icon taps go to aarecvd --bt-only. Every 2 s it rewrites the header (aaview's log level, the clock
// fields) so aaview can tell the Pi is there.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "../Config.hpp"
#include "Interfaces.hpp"
#include "StreamRing.hpp"

namespace aarecvd::projection {

class RingInputDevice : public IInputDevice {
public:
  typedef std::function<void(int state)> IdleIconHandler;
  typedef std::function<void(bool open)> PairHandler;   // the icon held 3 s: open or close Bluetooth pairing

  RingInputDevice(std::shared_ptr<StreamRing> ring, std::string keysConf, uint32_t width, uint32_t height);
  ~RingInputDevice() override;

  // Starts the thread. pairingFile is where aarecvd --bt-only says whether pairing is open (PairingState.hpp); that
  // is passed on to aaview in the header. An empty pairingFile turns this off.
  void run(IdleIconHandler idleIcon, PairHandler pair = nullptr, std::string pairingFile = "");
  void shutdown();

  void start(IInputDeviceEventHandler::Pointer eventHandler) override;
  void stop(const IInputDeviceEventHandler::Pointer &eventHandler) override;
  ButtonCodes getSupportedButtonCodes() const override;
  bool hasTouchscreen() const override { return true; }
  uint32_t getTouchscreenWidth() const override { return width_; }
  uint32_t getTouchscreenHeight() const override { return height_; }

  // Night mode for the phone (dark map and theme) follows the headlights. It is kept across sessions.
  bool night() const { return night_; }

private:
  void loop();
  bool lightsEvent(int x, int y);       // true when the night state changed
  void handle(const StreamRing::Event &ev, const IInputDeviceEventHandler::Pointer &h);

  std::shared_ptr<StreamRing> ring_;
  std::string keysConf_;
  uint32_t width_, height_;
  config::HardKeys hardKeys_;
  std::mutex mutex_;
  IInputDeviceEventHandler::Pointer handler_;
  IdleIconHandler idleIcon_;
  PairHandler pair_;
  std::string pairingFile_;
  std::atomic<bool> night_{false}, nightSeen_{false}, running_{false};
  std::thread thread_;
};

}  // namespace aarecvd::projection
