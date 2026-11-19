/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/MediaPlaybackStatus/MediaPlaybackStatusService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "MediaPlaybackStatusService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

namespace aarecvd::service::mediaplaybackstatus {

namespace msg = aap_protobuf::service;

MediaPlaybackStatusService::MediaPlaybackStatusService(boost::asio::io_service &ioService,
                                                       aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(ioService),
      channel_(std::make_shared<aasdk::channel::mediaplaybackstatus::MediaPlaybackStatusService>(strand_,
                                                                                                 std::move(messenger))) {}

void MediaPlaybackStatusService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void MediaPlaybackStatusService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  service->mutable_media_playback_service();
}

void MediaPlaybackStatusService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
  msg::control::message::ChannelOpenResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&MediaPlaybackStatusService::onChannelError, this->shared_from_this(),
                                   std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void MediaPlaybackStatusService::onMetadataUpdate(const msg::mediaplayback::message::MediaPlaybackMetadata &metadata) {
  auto m = metadata;
  if (m.has_album_art()) m.set_album_art(log::fmt("<%zu bytes>", metadata.album_art().size()));
  AA_INFO("aa: media playing %s", brief(m).c_str());
  channel_->receive(this->shared_from_this());
}

void MediaPlaybackStatusService::onPlaybackUpdate(const msg::mediaplayback::message::MediaPlaybackStatus &playback) {
  const int state = playback.has_state() ? static_cast<int>(playback.state()) : 0;
  if (state != lastState_ || playback.media_source() != lastSource_) {
    lastState_ = state;
    lastSource_ = playback.media_source();
    AA_INFO("aa: media playback %s", brief(playback).c_str());
  } else {
    AA_DBG("aa: media playback %s", brief(playback).c_str());
  }
  channel_->receive(this->shared_from_this());
}

void MediaPlaybackStatusService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: media playback channel: %s", e.what());
}

}  // namespace aarecvd::service::mediaplaybackstatus
