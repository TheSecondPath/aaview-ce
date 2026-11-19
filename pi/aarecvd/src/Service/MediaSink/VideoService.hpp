/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/MediaSink/VideoMediaSinkService.hpp, VideoService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <chrono>

#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../../Projection/Interfaces.hpp"
#include "../../Projection/StreamRing.hpp"
#include "../IService.hpp"

namespace aarecvd::service::mediasink {

class VideoService : public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler,
                     public IService,
                     public std::enable_shared_from_this<VideoService> {
public:
  typedef std::shared_ptr<VideoService> Pointer;
  VideoService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
               projection::IVideoOutput::Pointer videoOutput, std::shared_ptr<projection::StreamRing> ring,
               const Options &options);

  void start() override;
  void stop() override;
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onMediaChannelSetupRequest(const aap_protobuf::service::media::shared::message::Setup &request) override;
  void onMediaChannelStartIndication(const aap_protobuf::service::media::shared::message::Start &indication) override;
  void onMediaChannelStopIndication(const aap_protobuf::service::media::shared::message::Stop &indication) override;
  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer &buffer) override;
  void onMediaIndication(const aasdk::common::DataConstBuffer &buffer) override;
  void onChannelError(const aasdk::error::Error &e) override;
  void onVideoFocusRequest(
      const aap_protobuf::service::media::video::message::VideoFocusRequestNotification &request) override;

  // Not in OpenAuto
  void onIconTap(int state);           // from the ring poller thread
  void keyframeWatch();                // from the ring poller thread, every 5 ms

private:
  using std::enable_shared_from_this<VideoService>::shared_from_this;
  void sendVideoFocusIndication(bool projected, bool unsolicited);

  boost::asio::io_service::strand strand_;
  boost::asio::steady_timer timer_;
  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel_;
  projection::IVideoOutput::Pointer videoOutput_;
  std::shared_ptr<projection::StreamRing> ring_;
  Options options_;
  int32_t session_ = 0;
  // keyframe watch (poller thread only)
  std::chrono::steady_clock::time_point kfChecked_{}, kfAsked_{}, showSince_{};
  bool showing_ = false, asked_ = false;
};

}  // namespace aarecvd::service::mediasink
