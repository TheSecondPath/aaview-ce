/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/MediaSource/MediaSourceService.hpp, MicrophoneMediaSourceService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <boost/asio.hpp>

#include "../../Projection/RingOutputs.hpp"
#include "../IService.hpp"

namespace aarecvd::service::mediasource {

class MicrophoneService : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler,
                          public IService,
                          public std::enable_shared_from_this<MicrophoneService> {
public:
  static constexpr int CHANNEL = 7;    // aasdk's own MicrophoneAudioChannel would be 9

  MicrophoneService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                    std::shared_ptr<projection::RingAudioInput> audioInput);

  void start() override;
  void stop() override;
  void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) override;

  void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
  void onMediaChannelSetupRequest(const aap_protobuf::service::media::shared::message::Setup &request) override;
  void onMediaSourceOpenRequest(const aap_protobuf::service::media::source::message::MicrophoneRequest &request) override;
  void onMediaChannelAckIndication(const aap_protobuf::service::media::source::message::Ack &indication) override;
  void onChannelError(const aasdk::error::Error &e) override;

private:
  using std::enable_shared_from_this<MicrophoneService>::shared_from_this;
  void readMediaSource();
  void onMediaSourceDataReady(aasdk::common::Data data);
  void sendMicrophoneResponse();

  boost::asio::io_service::strand strand_;
  std::shared_ptr<aasdk::channel::mediasource::MediaSourceService> channel_;
  std::shared_ptr<projection::RingAudioInput> audioInput_;
};

}  // namespace aarecvd::service::mediasource
