/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/MediaSink/AudioMediaSinkService.hpp, MediaAudioService.hpp,
 *  GuidanceAudioService.hpp, SystemAudioService.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// One class for the three audio channels. OpenAuto has a subclass per channel that only picks the aasdk channel.
//   media    ch 4, 48 kHz stereo, aaview stream 0
//   guidance ch 5, 16 kHz mono (speech), stream 1
//   system   ch 6, 16 kHz mono, stream 2
#pragma once

#include <chrono>

#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <boost/asio.hpp>

#include "../../Projection/RingOutputs.hpp"
#include "../IService.hpp"

namespace aarecvd::service::mediasink {

class AudioService : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler,
                     public IService,
                     public std::enable_shared_from_this<AudioService> {
public:
  enum class Kind { MEDIA, GUIDANCE, SYSTEM };

  AudioService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger, Kind kind,
               projection::IAudioOutput::Pointer audioOutput, std::shared_ptr<projection::StreamRing> ring,
               std::shared_ptr<projection::SessionStats> stats, const Options &options);

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

private:
  using std::enable_shared_from_this<AudioService>::shared_from_this;
  int ch() const;

  boost::asio::io_service::strand strand_;
  Kind kind_;
  aasdk::channel::mediasink::audio::IAudioMediaSinkService::Pointer channel_;
  projection::IAudioOutput::Pointer audioOutput_;
  std::shared_ptr<projection::StreamRing> ring_;
  std::shared_ptr<projection::SessionStats> stats_;
  Options options_;
  int32_t session_ = 0;
  uint64_t bytes_ = 0;                 // sent to the head unit this session
  bool haveLast_ = false;
  std::chrono::steady_clock::time_point last_;
  int peak_ = 0;                       // for speech and system sounds: loudest sample and bytes, per sound
  uint64_t clip_ = 0;
};

}  // namespace aarecvd::service::mediasink
