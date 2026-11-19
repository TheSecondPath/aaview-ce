/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/MediaSink/VideoMediaSinkService.cpp, VideoService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The video channel (3): frames into the stream ring, acks with --unacked, and video focus. Two extras: a tap on
// aaview's icon gives the phone video focus again, and a picture that never starts (aaview showing, nothing decoded
// for 3 s) gets a new keyframe by taking video focus away and giving it back.
#include "VideoService.hpp"
#include "../../Log.hpp"
#include "../Util.hpp"

#include <aasdk/Channel/MediaSink/Video/Channel/VideoChannel.hpp>

namespace aarecvd::service::mediasink {

namespace msg = aap_protobuf::service;
using Clock = std::chrono::steady_clock;

VideoService::VideoService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                           projection::IVideoOutput::Pointer videoOutput, std::shared_ptr<projection::StreamRing> ring,
                           const Options &options)
    : strand_(ioService), timer_(ioService),
      channel_(std::make_shared<aasdk::channel::mediasink::video::channel::VideoChannel>(strand_, std::move(messenger))),
      videoOutput_(std::move(videoOutput)), ring_(std::move(ring)), options_(options) {}

void VideoService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void VideoService::stop() {
  strand_.dispatch([this, self = this->shared_from_this()]() {
    timer_.cancel();
    videoOutput_->stop();
  });
}

void VideoService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  auto *videoChannel = service->mutable_media_sink_service();
  videoChannel->set_available_type(msg::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
  auto *videoConfig = videoChannel->add_video_configs();
  videoConfig->set_codec_resolution(videoOutput_->getVideoResolution());
  videoConfig->set_frame_rate(videoOutput_->getVideoFPS());
  videoConfig->set_width_margin(videoOutput_->getWidthMargin());
  videoConfig->set_height_margin(videoOutput_->getHeightMargin());
  videoConfig->set_density(static_cast<uint32_t>(videoOutput_->getScreenDPI()));
  videoChannel->set_available_while_in_call(true);
}

void VideoService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &request) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
  msg::control::message::ChannelOpenResponse response;
  response.set_status(videoOutput_->open() ? aap_protobuf::shared::STATUS_SUCCESS
                                           : aap_protobuf::shared::STATUS_INTERNAL_ERROR);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&VideoService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void VideoService::onMediaChannelSetupRequest(const msg::media::shared::message::Setup &request) {
  AA_DBG("aa: video setup request %s", brief(request).c_str());
  msg::media::shared::message::Config response;
  response.set_status(videoOutput_->init() ? msg::media::shared::message::Config::STATUS_READY
                                           : msg::media::shared::message::Config::STATUS_WAIT);
  response.set_max_unacked(static_cast<uint32_t>(options_.unacked));
  response.add_configuration_indices(0);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&VideoService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelSetupResponse(response, std::move(promise));
  sendVideoFocusIndication(true, false);   // video focus: projected
  channel_->receive(this->shared_from_this());
}

void VideoService::onMediaChannelStartIndication(const msg::media::shared::message::Start &indication) {
  session_ = indication.session_id();
  AA_INFO("aa: video start, session %d config %u", session_, indication.configuration_index());
  ring_->setProjecting(true);
  channel_->receive(this->shared_from_this());
}

void VideoService::onMediaChannelStopIndication(const msg::media::shared::message::Stop &) {
  AA_INFO("aa: video stop");
  ring_->setProjecting(false);
  channel_->receive(this->shared_from_this());
}

void VideoService::onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                                  const aasdk::common::DataConstBuffer &buffer) {
  videoOutput_->write(timestamp, buffer);
  msg::media::source::message::Ack indication;
  indication.set_session_id(session_);
  indication.set_ack(1);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&VideoService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendMediaAckIndication(indication, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void VideoService::onMediaIndication(const aasdk::common::DataConstBuffer &buffer) {
  this->onMediaWithTimestampIndication(0, buffer);   // codec config, acked like a frame as aasdk does
}

void VideoService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: video channel: %s", e.what());
}

void VideoService::onVideoFocusRequest(const msg::media::video::message::VideoFocusRequestNotification &request) {
  const bool projected = !request.has_mode() || request.mode() == msg::media::video::message::VIDEO_FOCUS_PROJECTED;
  AA_INFO("aa: video focus request %s (%s)", brief(request).c_str(),
          projected ? "projected" : "back to the car screen");
  sendVideoFocusIndication(projected, false);
  ring_->setProjecting(projected);
  channel_->receive(this->shared_from_this());
}

void VideoService::sendVideoFocusIndication(bool projected, bool unsolicited) {
  msg::media::video::message::VideoFocusNotification indication;
  indication.set_focus(projected ? msg::media::video::message::VIDEO_FOCUS_PROJECTED
                                 : msg::media::video::message::VIDEO_FOCUS_NATIVE);
  indication.set_unsolicited(unsolicited);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&VideoService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendVideoFocusIndication(indication, std::move(promise));
}

void VideoService::onIconTap(int state) {
  // aaview's floating icon was tapped while the phone is connected but not projecting (for example after Android
  // Auto's own "exit" button): give the phone video focus again, as a car's Android Auto button does.
  AA_INFO("aa: icon tapped (state %d): asking the phone to project", state);
  strand_.dispatch([this, self = this->shared_from_this()]() { sendVideoFocusIndication(true, true); });
  ring_->setProjecting(true);
}

void VideoService::keyframeWatch() {
  // aaview is showing but has decoded nothing for 3 s, so it has no keyframe to start from. Ask the phone for one by
  // taking video focus away and giving it back, which makes the phone restart its encoder.
  const auto now = Clock::now();
  if (now - kfChecked_ < std::chrono::milliseconds(500)) return;
  kfChecked_ = now;
  auto hu = ring_->hu();
  if (!((ring_->flags() & projection::StreamRing::FLAG_PROJECTING) && hu && ring_->huAlive() && (hu->flags & 1))) {
    showing_ = false;
    return;
  }
  if (!showing_) {
    showing_ = true;
    showSince_ = now;
  } else if (hu->decoded == 0 && now - showSince_ > std::chrono::seconds(3) &&
             (!asked_ || now - kfAsked_ > std::chrono::seconds(10))) {
    asked_ = true;
    kfAsked_ = now;
    AA_WARN("aa: aaview has no picture; asking the phone for a new keyframe (video focus off and on)");
    strand_.dispatch([this, self = this->shared_from_this()]() {
      sendVideoFocusIndication(false, true);
      timer_.expires_after(std::chrono::milliseconds(300));
      timer_.async_wait(strand_.wrap([this, self](const boost::system::error_code &ec) {
        if (!ec) sendVideoFocusIndication(true, true);
      }));
    });
  }
}

}  // namespace aarecvd::service::mediasink
