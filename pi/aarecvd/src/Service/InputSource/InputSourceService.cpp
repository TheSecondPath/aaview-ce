/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/InputSource/InputSourceService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Touches and keys from aaview's status sector. Every key binding request is answered "success" (OpenAuto refuses
// one that names a key it did not offer), and events are sent from the start of the session.
#include "InputSourceService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

namespace aarecvd::service::inputsource {

namespace msg = aap_protobuf::service;

InputSourceService::InputSourceService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                                       projection::IInputDevice::Pointer inputDevice)
    : strand_(ioService),
      channel_(std::make_shared<aasdk::channel::inputsource::InputSourceService>(strand_, std::move(messenger))),
      inputDevice_(std::move(inputDevice)) {}

void InputSourceService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void InputSourceService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  auto *inputChannel = service->mutable_input_source_service();
  for (auto code : inputDevice_->getSupportedButtonCodes()) inputChannel->add_keycodes_supported(static_cast<int32_t>(code));
  if (inputDevice_->hasTouchscreen()) {
    auto *touchscreenConfig = inputChannel->add_touchscreen();
    touchscreenConfig->set_width(static_cast<int32_t>(inputDevice_->getTouchscreenWidth()));
    touchscreenConfig->set_height(static_cast<int32_t>(inputDevice_->getTouchscreenHeight()));
  }
}

void InputSourceService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
  msg::control::message::ChannelOpenResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&InputSourceService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void InputSourceService::onKeyBindingRequest(const msg::media::sink::message::KeyBindingRequest &request) {
  AA_DBG("aa: input binding request %s", brief(request).c_str());
  msg::media::sink::message::KeyBindingResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&InputSourceService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendKeyBindingResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void InputSourceService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: input channel: %s", e.what());
}

void InputSourceService::send(const msg::inputsource::message::InputReport &report) {
  strand_.dispatch([this, self = this->shared_from_this(), report]() {
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then([]() {}, std::bind(&InputSourceService::onChannelError, this->shared_from_this(), std::placeholders::_1));
    channel_->sendInputReport(report, std::move(promise));
  });
}

void InputSourceService::onButtonEvent(const projection::ButtonEvent &event) {
  msg::inputsource::message::InputReport report;
  report.set_timestamp(monotonicMicros());
  auto *key = report.mutable_key_event()->add_keys();
  key->set_keycode(event.code);
  key->set_down(event.type == projection::ButtonEventType::PRESS);
  key->set_metastate(0);
  key->set_longpress(false);
  send(report);
}

void InputSourceService::onTouchEvent(const projection::TouchEvent &event) {
  msg::inputsource::message::InputReport report;
  report.set_timestamp(monotonicMicros());
  auto *touch = report.mutable_touch_event();
  for (const auto &p : event.pointers) {
    auto *loc = touch->add_pointer_data();
    loc->set_x(p.x);
    loc->set_y(p.y);
    loc->set_pointer_id(p.pointerId);
  }
  touch->set_action_index(event.actionIndex);
  touch->set_action(event.type);
  send(report);
}

}  // namespace aarecvd::service::inputsource
