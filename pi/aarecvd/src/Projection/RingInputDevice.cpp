/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "RingInputDevice.hpp"
#include "../Bluetooth/PairingState.hpp"
#include "../Log.hpp"

#include <algorithm>
#include <chrono>

namespace aarecvd::projection {

using Clock = std::chrono::steady_clock;
namespace pa = aap_protobuf::service::inputsource::message;

RingInputDevice::RingInputDevice(std::shared_ptr<StreamRing> ring, std::string keysConf, uint32_t width,
                                 uint32_t height)
    : ring_(std::move(ring)), keysConf_(std::move(keysConf)), width_(width), height_(height) {}

RingInputDevice::~RingInputDevice() { shutdown(); }

void RingInputDevice::run(IdleIconHandler idleIcon, PairHandler pair, std::string pairingFile) {
  idleIcon_ = std::move(idleIcon);
  pair_ = std::move(pair);
  pairingFile_ = std::move(pairingFile);
  running_ = true;
  thread_ = std::thread([this] { loop(); });
}

void RingInputDevice::shutdown() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void RingInputDevice::start(IInputDeviceEventHandler::Pointer eventHandler) {
  std::lock_guard<std::mutex> l(mutex_);
  handler_ = std::move(eventHandler);
}

void RingInputDevice::stop(const IInputDeviceEventHandler::Pointer &eventHandler) {
  std::lock_guard<std::mutex> l(mutex_);
  if (handler_ == eventHandler) handler_.reset();
}

IInputDevice::ButtonCodes RingInputDevice::getSupportedButtonCodes() const {
  // HOME BACK CALL ENDCALL arrows ENTER mic play/pause next prev play pause
  return {3, 4, 5, 6, 0x13, 0x14, 0x15, 0x16, 0x17, 0x54, 0x55, 0x57, 0x58, 0x7e, 0x7f};
}

bool RingInputDevice::lightsEvent(int x, int y) {
  // phoenix's ILL_ON (0x77) and ILL_OFF (0x78) commands, passed on by MatMessage.dll
  int night = x == 0x77 ? 1 : x == 0x78 ? 0 : -1;
  AA_INFO("lights: phoenix command 0x%02x data 0x%x%s", x, y,
          night < 0 ? "" : (night ? " -> night True" : " -> night False"));
  if (night < 0 || (nightSeen_ && night_ == static_cast<bool>(night))) return false;
  night_ = night != 0;
  nightSeen_ = true;
  return true;
}

void RingInputDevice::loop() {
  auto beat = Clock::now() - std::chrono::seconds(10), pairLook = beat;
  uint32_t lostSaid = 0, oversizeSaid = 0;
  while (running_) {
    IInputDeviceEventHandler::Pointer h;
    try {
      if (Clock::now() - beat > std::chrono::seconds(2)) {
        beat = Clock::now();
        const uint32_t lost = ring_->eventsLost(), oversize = ring_->oversize();
        if (lost != lostSaid) {
          AA_WARN("aa: %u touch/key events from aaview were overwritten before they were read (the newest 16 were "
                  "kept)", lost - lostSaid);
          lostSaid = lost;
        }
        if (oversize != oversizeSaid) {
          AA_WARN("aa: %u video frames over %u KB dropped (more than aaview reads at once)", oversize - oversizeSaid,
                  StreamRing::MAX_RECORD_BYTES / 1024);
          oversizeSaid = oversize;
        }
        // aaview logs at the same level, coded debug 0, info 1, warning 2, error 3.
        const int lvl = log::level();
        ring_->setLoglevel(lvl == 7 ? 0 : lvl == 6 ? 1 : lvl == 4 ? 2 : 3);
        ring_->setClockEnabled(log::setClock());
        ring_->writeHeader();
      }
      if (!pairingFile_.empty() && Clock::now() - pairLook > std::chrono::milliseconds(250)) {
        pairLook = Clock::now();           // often enough for the pairing code to show on the icon quickly
        const auto ps = bt::readPairingState(pairingFile_);
        ring_->setPairing(ps.open, ps.passkey);
      }
      {
        std::lock_guard<std::mutex> l(mutex_);
        h = handler_;
      }
      if (h) h->onPoll();
      for (auto &ev : ring_->poll()) handle(ev, h);
    } catch (const std::exception &e) {
      AA_ERR("aa: idle loop: %s", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(h ? 5 : 100));
  }
}

void RingInputDevice::handle(const StreamRing::Event &ev, const IInputDeviceEventHandler::Pointer &h) {
  const int x = ev.x, y = ev.y;
  if (ev.action == StreamRing::PAIR) {     // with or without a session
    AA_INFO("aa: icon held: %s Bluetooth pairing", x ? "open" : "close");
    if (pair_) pair_(x != 0);
    return;
  }
  if (!h) {
    // With no session nobody else reads aaview's events. Headlights are remembered; icon taps mean "connect now".
    if (ev.action == StreamRing::LIGHTS) lightsEvent(x, y);
    if (ev.action == StreamRing::ICON && idleIcon_) idleIcon_(x);
    return;
  }
  switch (ev.action) {
    case StreamRing::ICON:
      h->onIconTap(x);
      return;
    case StreamRing::LIGHTS:
      if (lightsEvent(x, y)) h->onNightModeChanged(night_);
      return;
    case StreamRing::HARDKEY:
    case StreamRing::ISVEVENT: {
      if (ev.action == StreamRing::HARDKEY && x >= 300 && !(ring_->flags() & StreamRing::FLAG_PROJECTING)) {
        // Steering keys also reach us when the AA screen is not showing. kronos handles them then, so leave them.
        AA_INFO("hardkey %d with Android Auto not on screen: left to the head unit", x);
        return;
      }
      hardKeys_.loadOverrides(keysConf_);   // re-read on each press, since the file can be edited live
      std::optional<int> code;
      if (ev.action == StreamRing::HARDKEY) code = hardKeys_.lookup(x);
      AA_DBG("%s %d (0x%x) data 0x%x -> %s", StreamRing::actionName(ev.action).c_str(), x, x, y,
             code ? log::fmt("AA key 0x%x", *code).c_str() : "unmapped");
      if (code) {                          // a steering-wheel press is one event: send press + release
        h->onButtonEvent({ButtonEventType::PRESS, static_cast<uint32_t>(*code)});
        h->onButtonEvent({ButtonEventType::RELEASE, static_cast<uint32_t>(*code)});
      }
      return;
    }
    case StreamRing::KEYDOWN:
    case StreamRing::KEYUP: {
      auto code = config::virtualKeyToAA(x);
      AA_DBG("key %s vk 0x%02x scan 0x%02x -> %s", StreamRing::actionName(ev.action).c_str(), x, y,
             code ? log::fmt("AA key 0x%x", *code).c_str() : "unmapped");
      if (code)
        h->onButtonEvent({ev.action == StreamRing::KEYDOWN ? ButtonEventType::PRESS : ButtonEventType::RELEASE,
                          static_cast<uint32_t>(*code)});
      return;
    }
    case StreamRing::DOWN:
    case StreamRing::UP:
    case StreamRing::MOVE: {
      TouchEvent t;
      t.type = ev.action == StreamRing::DOWN ? pa::ACTION_DOWN : ev.action == StreamRing::UP ? pa::ACTION_UP
                                                                                            : pa::ACTION_MOVED;
      t.pointers.push_back({std::min<uint32_t>(x, width_ - 1), std::min<uint32_t>(y, height_ - 1), 0});
      t.actionIndex = 0;
      h->onTouchEvent(t);
      if (ev.action != StreamRing::MOVE) AA_DBG("touch %s %d %d", StreamRing::actionName(ev.action).c_str(), x, y);
      return;
    }
    default:
      AA_DBG("aaview event %d (%d, %d): not known here", ev.action, x, y);
  }
}

}  // namespace aarecvd::projection
