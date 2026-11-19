/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/MediaPlaybackStatus/MediaPlaybackStatusService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// What the phone is playing, only logged: song changes and play/pause at info, the once-a-second position at debug.
#pragma once

#include <aasdk/Channel/MediaPlaybackStatus/IMediaPlaybackStatusServiceEventHandler.hpp>
#include <aasdk/Channel/MediaPlaybackStatus/MediaPlaybackStatusService.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../IService.hpp"

namespace aarecvd::service::mediaplaybackstatus {

class MediaPlaybackStatusService : public aasdk::channel::mediaplaybackstatus::IMediaPlaybackStatusServiceEventHandler,
                                   public IService,
                                   public std::enable_shared_from_this<MediaPlaybackStatusService> {
public:
  MediaPlaybackStatusService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger);
  void start() override;
  void stop() override {}
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onChannelError(const aasdk::error::Error &e) override;
  void onMetadataUpdate(const aap_protobuf::service::mediaplayback::message::MediaPlaybackMetadata &metadata) override;
  void onPlaybackUpdate(const aap_protobuf::service::mediaplayback::message::MediaPlaybackStatus &playback) override;

private:
  using std::enable_shared_from_this<MediaPlaybackStatusService>::shared_from_this;
  boost::asio::io_service::strand strand_;
  std::shared_ptr<aasdk::channel::mediaplaybackstatus::MediaPlaybackStatusService> channel_;
  int lastState_ = -1;
  std::string lastSource_;
};

}  // namespace aarecvd::service::mediaplaybackstatus
