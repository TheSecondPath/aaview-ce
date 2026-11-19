/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/MediaSource/MediaSourceService.cpp, MicrophoneMediaSourceService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The assistant's ear (channel 7). The phone opens the microphone when it starts listening and closes it when it has
// heard enough; meanwhile aaview records the car's microphone into the mic slots and RingAudioInput passes it on
// (16 kHz, 16 bit, mono). Phone calls never come this way: they stay on the car's Bluetooth.
//
// aasdk workaround: aasdk's MediaSourceService sends its setup response with message id 0x8000 (MEDIA_MESSAGE_SETUP)
// instead of 0x8003 (CONFIG), and its microphone response with 0x8005 (MICROPHONE_REQUEST) instead of 0x8006
// (MICROPHONE_RESPONSE). Both replies are built here with the right ids and sent through the channel's own send().
// aasdk is still used for receiving, parsing and the data messages.
#include "MicrophoneService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

#include <aap_protobuf/service/media/sink/MediaMessageId.pb.h>

namespace aarecvd::service::mediasource {

namespace msg = aap_protobuf::service;

MicrophoneService::MicrophoneService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                                     std::shared_ptr<projection::RingAudioInput> audioInput)
    : strand_(ioService),
      channel_(std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
          strand_, std::move(messenger), static_cast<aasdk::messenger::ChannelId>(CHANNEL))),
      audioInput_(std::move(audioInput)) {}

void MicrophoneService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void MicrophoneService::stop() {
  strand_.dispatch([this, self = this->shared_from_this()]() { audioInput_->stop(); });
}

void MicrophoneService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(CHANNEL);
  auto *avInputChannel = service->mutable_media_source_service();
  avInputChannel->set_available_type(msg::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
  auto *audioConfig = avInputChannel->mutable_audio_config();
  audioConfig->set_sampling_rate(audioInput_->getSampleRate());
  audioConfig->set_number_of_bits(audioInput_->getSampleSize());
  audioConfig->set_number_of_channels(audioInput_->getChannelCount());
}

void MicrophoneService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", CHANNEL);
  msg::control::message::ChannelOpenResponse response;
  response.set_status(audioInput_->open() ? aap_protobuf::shared::STATUS_SUCCESS
                                          : aap_protobuf::shared::STATUS_INTERNAL_ERROR);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&MicrophoneService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void MicrophoneService::onMediaChannelSetupRequest(const msg::media::shared::message::Setup &request) {
  AA_DBG("aa: microphone setup request %s", brief(request).c_str());
  msg::media::shared::message::Config response;
  response.set_status(msg::media::shared::message::Config::STATUS_READY);
  response.set_max_unacked(1);
  response.add_configuration_indices(0);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&MicrophoneService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  sendSpecific(*channel_, msg::media::sink::MEDIA_MESSAGE_CONFIG, response, std::move(promise));   // see the top
  channel_->receive(this->shared_from_this());
}

void MicrophoneService::onMediaSourceOpenRequest(const msg::media::source::message::MicrophoneRequest &request) {
  if (request.open()) {
    auto startPromise = projection::IAudioInput::StartPromise::defer(strand_);
    startPromise->then([]() {}, []() {});
    audioInput_->start(std::move(startPromise));    // header flag bit 3 on, new mic_gen, slots wiped
    AA_INFO("aa: microphone opened by the phone %s", brief(request).c_str());
    sendMicrophoneResponse();
    readMediaSource();
  } else {
    const bool was = audioInput_->isActive();
    const double secs = audioInput_->seconds();
    audioInput_->stop();
    if (was)
      AA_INFO("aa: microphone closed by the phone after %.1f s: %u chunks (%.1f s of sound) passed on, %u lost", secs,
              audioInput_->chunks(), audioInput_->chunks() * 0.0625, audioInput_->lost());
    sendMicrophoneResponse();
  }
  channel_->receive(this->shared_from_this());
}

void MicrophoneService::sendMicrophoneResponse() {
  msg::media::source::message::MicrophoneResponse response;
  response.set_status(0);
  response.set_session_id(0);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&MicrophoneService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  sendSpecific(*channel_, msg::media::sink::MEDIA_MESSAGE_MICROPHONE_RESPONSE, response, std::move(promise));
}

void MicrophoneService::onMediaChannelAckIndication(const msg::media::source::message::Ack &) {
  // The phone acknowledges our chunks; nothing to do.
  channel_->receive(this->shared_from_this());
}

void MicrophoneService::readMediaSource() {
  if (!audioInput_->isActive()) return;
  auto readPromise = projection::IAudioInput::ReadPromise::defer(strand_);
  readPromise->then(std::bind(&MicrophoneService::onMediaSourceDataReady, this->shared_from_this(), std::placeholders::_1),
                    []() {});
  audioInput_->read(std::move(readPromise));
}

void MicrophoneService::onMediaSourceDataReady(aasdk::common::Data data) {
  auto sendPromise = aasdk::channel::SendPromise::defer(strand_);
  sendPromise->then(std::bind(&MicrophoneService::readMediaSource, this->shared_from_this()),
                    std::bind(&MicrophoneService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendMediaSourceWithTimestampIndication(monotonicMicros(), data, std::move(sendPromise));
}

void MicrophoneService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: microphone channel: %s", e.what());
}

}  // namespace aarecvd::service::mediasource
