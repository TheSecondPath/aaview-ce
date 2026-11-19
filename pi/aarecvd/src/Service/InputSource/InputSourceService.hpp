/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/InputSource/InputSourceService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <aasdk/Error/Error.hpp>   // aasdk's header below uses it without including it
#include <aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp>
#include <aasdk/Channel/InputSource/InputSourceService.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../../Projection/Interfaces.hpp"
#include "../IService.hpp"

namespace aarecvd::service::inputsource {

class InputSourceService : public aasdk::channel::inputsource::IInputSourceServiceEventHandler,
                           public IService,
                           public std::enable_shared_from_this<InputSourceService> {
public:
  InputSourceService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                     projection::IInputDevice::Pointer inputDevice);

  void start() override;
  void stop() override {}
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onKeyBindingRequest(const aap_protobuf::service::media::sink::message::KeyBindingRequest &request) override;
  void onChannelError(const aasdk::error::Error &e) override;

  // Called from the ring poller thread. In OpenAuto these come through IInputDeviceEventHandler.
  void onButtonEvent(const projection::ButtonEvent &event);
  void onTouchEvent(const projection::TouchEvent &event);

private:
  using std::enable_shared_from_this<InputSourceService>::shared_from_this;
  void send(const aap_protobuf::service::inputsource::message::InputReport &report);

  boost::asio::io_service::strand strand_;
  std::shared_ptr<aasdk::channel::inputsource::InputSourceService> channel_;
  projection::IInputDevice::Pointer inputDevice_;
};

}  // namespace aarecvd::service::inputsource
