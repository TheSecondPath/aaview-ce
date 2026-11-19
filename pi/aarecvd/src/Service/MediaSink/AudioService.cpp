/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/MediaSink/AudioMediaSinkService.cpp and its three subclasses).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "AudioService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

#include <aasdk/Channel/MediaSink/Audio/Channel/GuidanceAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/MediaAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/SystemAudioChannel.hpp>

#include <algorithm>

namespace aarecvd::service::mediasink {

namespace msg = aap_protobuf::service;
namespace ach = aasdk::channel::mediasink::audio::channel;
using Clock = std::chrono::steady_clock;

namespace {
aasdk::channel::mediasink::audio::IAudioMediaSinkService::Pointer makeChannel(
    AudioService::Kind kind, boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger) {
  switch (kind) {
    case AudioService::Kind::MEDIA:    // channel 4
      return std::make_shared<ach::MediaAudioChannel>(strand, std::move(messenger));
    case AudioService::Kind::GUIDANCE:  // channel 5
      return std::make_shared<ach::GuidanceAudioChannel>(strand, std::move(messenger));
    default:                            // channel 6
      return std::make_shared<ach::SystemAudioChannel>(strand, std::move(messenger));
  }
}
}  // namespace

AudioService::AudioService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                           Kind kind, projection::IAudioOutput::Pointer audioOutput,
                           std::shared_ptr<projection::StreamRing> ring, std::shared_ptr<projection::SessionStats> stats,
                           const Options &options)
    : strand_(ioService), kind_(kind), channel_(makeChannel(kind, strand_, std::move(messenger))),
      audioOutput_(std::move(audioOutput)), ring_(std::move(ring)), stats_(std::move(stats)), options_(options) {}

int AudioService::ch() const { return static_cast<int>(channel_->getId()); }

void AudioService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void AudioService::stop() {
  strand_.dispatch([this, self = this->shared_from_this()]() { audioOutput_->stop(); });
}

void AudioService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(ch());
  auto *audioChannel = service->mutable_media_sink_service();
  audioChannel->set_available_type(msg::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
  audioChannel->set_audio_type(kind_ == Kind::MEDIA      ? msg::media::sink::message::AUDIO_STREAM_MEDIA
                               : kind_ == Kind::GUIDANCE ? msg::media::sink::message::AUDIO_STREAM_GUIDANCE
                                                         : msg::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO);
  auto *audioConfig = audioChannel->add_audio_configs();
  audioConfig->set_sampling_rate(audioOutput_->getSampleRate());
  audioConfig->set_number_of_bits(audioOutput_->getSampleSize());
  audioConfig->set_number_of_channels(audioOutput_->getChannelCount());
}

void AudioService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", ch());
  msg::control::message::ChannelOpenResponse response;
  response.set_status(audioOutput_->open() ? aap_protobuf::shared::STATUS_SUCCESS
                                           : aap_protobuf::shared::STATUS_INTERNAL_ERROR);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&AudioService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void AudioService::onMediaChannelSetupRequest(const msg::media::shared::message::Setup &request) {
  AA_DBG("aa: audio ch %d setup request %s", ch(), brief(request).c_str());
  // With max_unacked 1, as OpenAuto sets it, the phone sends one chunk per round trip (~250 ms over this Wi-Fi), far
  // below real time for 48 kHz stereo (192 KB/s). So let it run ahead (--audio-unacked).
  msg::media::shared::message::Config response;
  response.set_status(msg::media::shared::message::Config::STATUS_READY);
  response.set_max_unacked(static_cast<uint32_t>(options_.audioUnacked));
  response.add_configuration_indices(0);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&AudioService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelSetupResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void AudioService::onMediaChannelStartIndication(const msg::media::shared::message::Start &indication) {
  session_ = indication.session_id();
  peak_ = 0;
  clip_ = 0;
  AA_INFO("aa: audio ch %d start, session %d", ch(), session_);
  if (kind_ == Kind::MEDIA) ring_->mediaStarted();   // aaview switches the head unit's audio source back to USB
  audioOutput_->start();
  channel_->receive(this->shared_from_this());
}

void AudioService::onMediaChannelStopIndication(const msg::media::shared::message::Stop &) {
  AA_INFO("aa: audio ch %d stop (%llu KB sent to the head unit, %u chunks dropped)", ch(),
          static_cast<unsigned long long>(bytes_ / 1024), stats_->audioDrops.load());
  if (kind_ != Kind::MEDIA) {
    const uint64_t rate = audioOutput_->getSampleRate() * audioOutput_->getChannelCount() * 2;
    AA_INFO("aa: audio ch %d: that sound was %llu ms, loudest sample %d of 32767%s", ch(),
            static_cast<unsigned long long>(clip_ * 1000 / rate), peak_, peak_ < 30 ? " (silence)" : "");
  }
  audioOutput_->suspend();
  channel_->receive(this->shared_from_this());
}

void AudioService::onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                                  const aasdk::common::DataConstBuffer &buffer) {
  bytes_ += buffer.size;
  // Track how late chunks come from the phone. A chunk is about 43 ms of sound and aaview buffers 600 ms, so a gap
  // near that is a hiccup on the Wi-Fi side. Gaps over 1.5 s are pauses, not lateness.
  const auto now = Clock::now();
  const double gap = haveLast_ ? std::chrono::duration<double>(now - last_).count() : 0.0;
  last_ = now;
  haveLast_ = true;
  if (kind_ == Kind::MEDIA && gap > 0.15 && gap < 1.5) {
    stats_->audioLate += 1;
    const uint32_t ms = static_cast<uint32_t>(gap * 1000);
    uint32_t worst = stats_->audioWorstMs.load();
    while (ms > worst && !stats_->audioWorstMs.compare_exchange_weak(worst, ms)) {
    }
    if (gap > 0.25) AA_WARN("aa: media audio from the phone stalled for %d ms", static_cast<int>(gap * 1000));
  }
  if (kind_ != Kind::MEDIA) {
    // Keep the loudest sample, to tell silence from the phone (a chime with the phone on vibrate) from sound lost on
    // the way. Speech and system sounds are short, so this costs little.
    const size_t n = buffer.size & ~static_cast<size_t>(1);
    for (size_t i = 0; i < n; i += 2) {
      const int s = static_cast<int16_t>(buffer.cdata[i] | (buffer.cdata[i + 1] << 8));
      peak_ = std::max(peak_, s < 0 ? -s : s);
    }
    clip_ += buffer.size;
  }
  if (!audioOutput_->write(timestamp, buffer)) stats_->audioDrops += 1;
  msg::media::source::message::Ack indication;
  indication.set_session_id(session_);
  indication.set_ack(1);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&AudioService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendMediaAckIndication(indication, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void AudioService::onMediaIndication(const aasdk::common::DataConstBuffer &buffer) {
  this->onMediaWithTimestampIndication(0, buffer);
}

void AudioService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: audio ch %d channel: %s", ch(), e.what());
}

}  // namespace aarecvd::service::mediasink
