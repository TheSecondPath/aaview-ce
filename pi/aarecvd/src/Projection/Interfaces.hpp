/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Projection/IVideoOutput.hpp, IAudioOutput.hpp, IAudioInput.hpp, IInputDevice.hpp,
 *  IInputDeviceEventHandler.hpp, InputEvent.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The projection interfaces the services talk to. OpenAuto's have Qt types in them (QRect); these don't, and they
// add what the stream-ring back ends need: the head unit's lights, its floating icon and a poll tick.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <aasdk/Common/Data.hpp>
#include <aasdk/IO/Promise.hpp>
#include <aasdk/Messenger/Timestamp.hpp>
#include <aap_protobuf/service/inputsource/message/PointerAction.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>

namespace aarecvd::projection {

class IVideoOutput {
public:
  typedef std::shared_ptr<IVideoOutput> Pointer;
  virtual ~IVideoOutput() = default;
  virtual bool open() = 0;
  virtual bool init() = 0;
  virtual void write(aasdk::messenger::Timestamp::ValueType timestamp, const aasdk::common::DataConstBuffer &buffer) = 0;
  virtual void stop() = 0;
  virtual aap_protobuf::service::media::sink::message::VideoFrameRateType getVideoFPS() const = 0;
  virtual aap_protobuf::service::media::sink::message::VideoCodecResolutionType getVideoResolution() const = 0;
  virtual size_t getScreenDPI() const = 0;
  virtual uint32_t getWidthMargin() const = 0;
  virtual uint32_t getHeightMargin() const = 0;
};

class IAudioOutput {
public:
  typedef std::shared_ptr<IAudioOutput> Pointer;
  virtual ~IAudioOutput() = default;
  virtual bool open() = 0;
  // Returns false if the chunk was dropped because the ring had no room.
  virtual bool write(aasdk::messenger::Timestamp::ValueType timestamp, const aasdk::common::DataConstBuffer &buffer) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void suspend() = 0;
  virtual uint32_t getSampleSize() const = 0;
  virtual uint32_t getChannelCount() const = 0;
  virtual uint32_t getSampleRate() const = 0;
};

class IAudioInput {
public:
  typedef aasdk::io::Promise<void, void> StartPromise;
  typedef aasdk::io::Promise<aasdk::common::Data, void> ReadPromise;
  typedef std::shared_ptr<IAudioInput> Pointer;
  virtual ~IAudioInput() = default;
  virtual bool open() = 0;
  virtual bool isActive() const = 0;
  virtual void read(ReadPromise::Pointer promise) = 0;
  virtual void start(StartPromise::Pointer promise) = 0;
  virtual void stop() = 0;
  virtual void poll() = 0;            // called every 5 ms by the ring poller while a session is up
  virtual uint32_t getSampleSize() const = 0;
  virtual uint32_t getChannelCount() const = 0;
  virtual uint32_t getSampleRate() const = 0;
};

enum class ButtonEventType { NONE, PRESS, RELEASE };

struct ButtonEvent {
  ButtonEventType type;
  uint32_t code;                       // Android Auto key code
};

struct TouchPoint {
  uint32_t x, y, pointerId;
};

struct TouchEvent {
  aap_protobuf::service::inputsource::message::PointerAction type;
  std::vector<TouchPoint> pointers;
  uint32_t actionIndex;
};

class IInputDeviceEventHandler {
public:
  typedef std::shared_ptr<IInputDeviceEventHandler> Pointer;
  virtual ~IInputDeviceEventHandler() = default;
  virtual void onButtonEvent(const ButtonEvent &event) = 0;
  virtual void onTouchEvent(const TouchEvent &event) = 0;
  // Not in OpenAuto:
  virtual void onNightModeChanged(bool night) = 0;   // the headlights went on or off
  virtual void onIconTap(int state) = 0;             // aaview's floating icon, during a session
  virtual void onPoll() = 0;                         // every 5 ms (keyframe watch, microphone)
};

class IInputDevice {
public:
  typedef std::shared_ptr<IInputDevice> Pointer;
  typedef std::vector<uint32_t> ButtonCodes;
  virtual ~IInputDevice() = default;
  virtual void start(IInputDeviceEventHandler::Pointer eventHandler) = 0;
  // Only stops if eventHandler is still the current one, since a newer session may have started meanwhile.
  virtual void stop(const IInputDeviceEventHandler::Pointer &eventHandler) = 0;
  virtual ButtonCodes getSupportedButtonCodes() const = 0;
  virtual bool hasTouchscreen() const = 0;
  virtual uint32_t getTouchscreenWidth() const = 0;
  virtual uint32_t getTouchscreenHeight() const = 0;
};

}  // namespace aarecvd::projection
