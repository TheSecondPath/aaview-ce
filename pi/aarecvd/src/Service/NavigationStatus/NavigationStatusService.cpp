/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/NavigationStatus/NavigationStatusService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "NavigationStatusService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

namespace aarecvd::service::navigationstatus {

namespace msg = aap_protobuf::service;

NavigationStatusService::NavigationStatusService(boost::asio::io_service &ioService,
                                                 aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(ioService),
      channel_(std::make_shared<aasdk::channel::navigationstatus::NavigationStatusService>(strand_, std::move(messenger))) {}

void NavigationStatusService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void NavigationStatusService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  auto *nav = service->mutable_navigation_status_service();
  nav->set_minimum_interval_ms(1000);
  nav->set_type(msg::navigationstatus::NavigationStatusService::ENUM);
}

void NavigationStatusService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
  msg::control::message::ChannelOpenResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&NavigationStatusService::onChannelError, this->shared_from_this(),
                                   std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void NavigationStatusService::onStatusUpdate(const msg::navigationstatus::message::NavigationStatus &navStatus) {
  AA_INFO("aa: navigation status %s", brief(navStatus).c_str());
  channel_->receive(this->shared_from_this());
}

void NavigationStatusService::onTurnEvent(const msg::navigationstatus::message::NavigationNextTurnEvent &turnEvent) {
  auto t = turnEvent;
  if (t.has_image()) t.set_image(log::fmt("<%zu bytes>", turnEvent.image().size()));
  AA_INFO("aa: navigation turn %s", brief(t).c_str());
  channel_->receive(this->shared_from_this());
}

void NavigationStatusService::onDistanceEvent(
    const msg::navigationstatus::message::NavigationNextTurnDistanceEvent &distanceEvent) {
  AA_DBG("aa: navigation distance %s", brief(distanceEvent).c_str());
  channel_->receive(this->shared_from_this());
}

void NavigationStatusService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: navigation channel: %s", e.what());
}

}  // namespace aarecvd::service::navigationstatus
