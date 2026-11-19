/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/NavigationStatus/NavigationStatusService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Turn-by-turn status for an instrument cluster, only logged. aasdk passes on NAVIGATION_STATUS, TURN_EVENT and
// DISTANCE_EVENT, but drops the newer NAVIGATION_STATE and CURRENT_POSITION messages with a note in its own log.
#pragma once

#include <aasdk/Channel/NavigationStatus/INavigationStatusServiceEventHandler.hpp>
#include <aasdk/Channel/NavigationStatus/NavigationStatusService.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../IService.hpp"

namespace aarecvd::service::navigationstatus {

class NavigationStatusService : public aasdk::channel::navigationstatus::INavigationStatusServiceEventHandler,
                                public IService,
                                public std::enable_shared_from_this<NavigationStatusService> {
public:
  NavigationStatusService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger);
  void start() override;
  void stop() override {}
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onChannelError(const aasdk::error::Error &e) override;
  void onStatusUpdate(const aap_protobuf::service::navigationstatus::message::NavigationStatus &navStatus) override;
  void onTurnEvent(const aap_protobuf::service::navigationstatus::message::NavigationNextTurnEvent &turnEvent) override;
  void onDistanceEvent(
      const aap_protobuf::service::navigationstatus::message::NavigationNextTurnDistanceEvent &distanceEvent) override;

private:
  using std::enable_shared_from_this<NavigationStatusService>::shared_from_this;
  boost::asio::io_service::strand strand_;
  std::shared_ptr<aasdk::channel::navigationstatus::NavigationStatusService> channel_;
};

}  // namespace aarecvd::service::navigationstatus
