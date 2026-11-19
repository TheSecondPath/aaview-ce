/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <aasdk/Channel/IChannel.hpp>
#include <aasdk/Messenger/Message.hpp>
#include <aasdk/Messenger/MessageId.hpp>
#include <google/protobuf/message.h>

namespace aarecvd::service {

// The monotonic clock in microseconds: the timestamp for pings, input events and microphone chunks.
inline uint64_t monotonicMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline std::string brief(const google::protobuf::Message &m) {
  std::string s = m.ShortDebugString();
  return s.empty() ? "{}" : "{" + s + "}";
}

// Sends an encrypted message with our own message id on an aasdk channel, for the replies aasdk gets wrong (the
// microphone channel's setup and open responses; see MediaSource/MicrophoneService.cpp).
inline void sendSpecific(aasdk::channel::IChannel &channel, uint16_t messageId, const google::protobuf::Message &body,
                         aasdk::channel::SendPromise::Pointer promise) {
  auto message = std::make_shared<aasdk::messenger::Message>(channel.getId(), aasdk::messenger::EncryptionType::ENCRYPTED,
                                                             aasdk::messenger::MessageType::SPECIFIC);
  message->insertPayload(aasdk::messenger::MessageId(messageId).getData());
  message->insertPayload(body);
  channel.send(std::move(message), std::move(promise));
}

}  // namespace aarecvd::service
