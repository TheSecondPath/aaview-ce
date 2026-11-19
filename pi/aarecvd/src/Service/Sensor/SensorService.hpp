/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/Sensor/SensorService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Driving status (always unrestricted) and night mode from the head unit's headlight events. OpenAuto polls a GPIO
// or a file instead.
#pragma once

#include <atomic>

#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../../Projection/RingInputDevice.hpp"
#include "../IService.hpp"

namespace aarecvd::service::sensor {

class SensorService : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler,
                      public IService,
                      public std::enable_shared_from_this<SensorService> {
public:
  SensorService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                std::shared_ptr<projection::RingInputDevice> lights);

  void start() override;
  void stop() override {}
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onSensorStartRequest(const aap_protobuf::service::sensorsource::message::SensorRequest &request) override;
  void onChannelError(const aasdk::error::Error &e) override;

  void onNightModeChanged();           // from the ring poller thread

private:
  using std::enable_shared_from_this<SensorService>::shared_from_this;
  void sendNight();

  boost::asio::io_service::strand strand_;
  std::shared_ptr<aasdk::channel::sensorsource::SensorSourceService> channel_;
  std::shared_ptr<projection::RingInputDevice> lights_;
  bool nightOn_ = false;               // the phone asked for night-mode data
};

}  // namespace aarecvd::service::sensor
