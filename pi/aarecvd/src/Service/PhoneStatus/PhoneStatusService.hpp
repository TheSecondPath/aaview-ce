/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/PhoneStatus/PhoneStatusService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Phone status (calls, signal strength), only logged. aasdk's PhoneStatusService drops the phone's PHONE_STATUS
// messages without passing them on (they reach its "Message Id not Handled" branch), so this service receives on its
// own small channel class, RawChannel, built on aasdk's Channel base class without changing aasdk.
#pragma once

#include <functional>

#include <aasdk/Channel/Channel.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <boost/asio.hpp>

#include "../IService.hpp"

namespace aarecvd::service::phonestatus {

class RawChannel : public aasdk::channel::Channel, public std::enable_shared_from_this<RawChannel> {
public:
  typedef std::function<void(uint16_t messageId, const aasdk::common::DataConstBuffer &payload)> MessageHandler;
  typedef std::function<void(const aasdk::error::Error &e)> ErrorHandler;
  RawChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger,
             aasdk::messenger::ChannelId channelId);
  void receive(MessageHandler onMessage, ErrorHandler onError);
  void sendChannelOpenResponse(const aap_protobuf::service::control::message::ChannelOpenResponse &response,
                               aasdk::channel::SendPromise::Pointer promise);
};

class PhoneStatusService : public IService, public std::enable_shared_from_this<PhoneStatusService> {
public:
  PhoneStatusService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger);
  void start() override;
  void stop() override {}
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

private:
  void receive();
  void onMessage(uint16_t messageId, const aasdk::common::DataConstBuffer &payload);
  void onChannelError(const aasdk::error::Error &e);

  boost::asio::io_service::strand strand_;
  std::shared_ptr<RawChannel> channel_;
};

}  // namespace aarecvd::service::phonestatus
