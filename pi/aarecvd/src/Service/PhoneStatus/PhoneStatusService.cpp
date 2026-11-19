/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/PhoneStatus/PhoneStatusService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "PhoneStatusService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

#include <aasdk/Messenger/MessageId.hpp>
#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/phonestatus/PhoneStatusMessageId.pb.h>
#include <aap_protobuf/service/phonestatus/message/PhoneStatus.pb.h>

namespace aarecvd::service::phonestatus {

namespace msg = aap_protobuf::service;

RawChannel::RawChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger,
                       aasdk::messenger::ChannelId channelId)
    : Channel(strand, std::move(messenger), channelId) {}

void RawChannel::receive(MessageHandler onMessage, ErrorHandler onError) {
  auto promise = aasdk::messenger::ReceivePromise::defer(strand_);
  promise->then(
      [onMessage](aasdk::messenger::Message::Pointer message) {
        aasdk::messenger::MessageId messageId(message->getPayload());
        aasdk::common::DataConstBuffer payload(message->getPayload(), messageId.getSizeOf());
        onMessage(messageId.getId(), payload);
      },
      [onError](const aasdk::error::Error &e) { onError(e); });
  messenger_->enqueueReceive(channelId_, std::move(promise));
}

void RawChannel::sendChannelOpenResponse(const msg::control::message::ChannelOpenResponse &response,
                                         aasdk::channel::SendPromise::Pointer promise) {
  auto message = std::make_shared<aasdk::messenger::Message>(channelId_, aasdk::messenger::EncryptionType::ENCRYPTED,
                                                             aasdk::messenger::MessageType::CONTROL);
  message->insertPayload(
      aasdk::messenger::MessageId(msg::control::message::ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE).getData());
  message->insertPayload(response);
  this->send(std::move(message), std::move(promise));
}

PhoneStatusService::PhoneStatusService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(ioService),
      channel_(std::make_shared<RawChannel>(strand_, std::move(messenger), aasdk::messenger::ChannelId::PHONE_STATUS)) {}

void PhoneStatusService::start() {
  strand_.dispatch([this, self = shared_from_this()]() { receive(); });
}

void PhoneStatusService::receive() {
  auto self = shared_from_this();
  channel_->receive([self](uint16_t id, const aasdk::common::DataConstBuffer &p) { self->onMessage(id, p); },
                    [self](const aasdk::error::Error &e) { self->onChannelError(e); });
}

void PhoneStatusService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  service->mutable_phone_status_service();
}

void PhoneStatusService::onMessage(uint16_t messageId, const aasdk::common::DataConstBuffer &payload) {
  if (messageId == msg::control::message::ControlMessageType::MESSAGE_CHANNEL_OPEN_REQUEST) {
    AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
    msg::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    auto self = shared_from_this();
    promise->then([]() {}, [self](const aasdk::error::Error &e) { self->onChannelError(e); });
    channel_->sendChannelOpenResponse(response, std::move(promise));
  } else if (messageId == msg::phonestatus::PHONE_STATUS) {
    msg::phonestatus::message::PhoneStatus status;
    if (status.ParseFromArray(payload.cdata, static_cast<int>(payload.size))) {
      for (auto &call : *status.mutable_calls())
        if (call.has_caller_thumbnail())
          call.set_caller_thumbnail(log::fmt("<%zu bytes>", call.caller_thumbnail().size()));
      AA_INFO("aa: phone status %s", brief(status).c_str());
    } else {
      AA_DBG("aa: phone status: %zu bytes that do not parse", payload.size);
    }
  } else {
    AA_DBG("aa: phone status message 0x%04x (%zu bytes)", messageId, payload.size);
  }
  receive();
}

void PhoneStatusService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: phone status channel: %s", e.what());
}

}  // namespace aarecvd::service::phonestatus
