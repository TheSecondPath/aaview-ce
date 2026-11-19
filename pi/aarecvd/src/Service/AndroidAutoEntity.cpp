/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/AndroidAutoEntity.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The control channel of one session: version exchange, TLS handshake, service discovery, audio and navigation
// focus, pings both ways and shutdown. It also runs the session watchdog: nothing received from the phone, or a
// send blocked, for --timeout seconds ends the session.
#include "AndroidAutoEntity.hpp"
#include "../Log.hpp"
#include "Util.hpp"

#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <boost/endian/conversion.hpp>
#include <google/protobuf/unknown_field_set.h>

namespace aarecvd::service {

namespace ctl = aap_protobuf::service::control::message;

AndroidAutoEntity::AndroidAutoEntity(boost::asio::io_service &ioService, aasdk::messenger::ICryptor::Pointer cryptor,
                                     std::shared_ptr<TlsWrapper> tls, aasdk::transport::ITransport::Pointer transport,
                                     aasdk::messenger::IMessenger::Pointer messenger, const Options &options,
                                     ServiceList serviceList, std::shared_ptr<Pinger> pinger, Activity::Pointer activity,
                                     std::shared_ptr<SessionLink> link,
                                     std::shared_ptr<projection::RingInputDevice> inputDevice)
    : ioService_(ioService), strand_(ioService), cryptor_(std::move(cryptor)), tls_(std::move(tls)),
      transport_(std::move(transport)), messenger_(std::move(messenger)),
      controlServiceChannel_(std::make_shared<aasdk::channel::control::ControlServiceChannel>(strand_, messenger_)),
      options_(options), serviceList_(std::move(serviceList)), pinger_(std::move(pinger)),
      activity_(std::move(activity)), link_(std::move(link)), inputDevice_(std::move(inputDevice)),
      watchdog_(ioService) {}

AndroidAutoEntity::~AndroidAutoEntity() = default;

aasdk::channel::SendPromise::Pointer AndroidAutoEntity::sendPromise() {
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&AndroidAutoEntity::onChannelError, this->shared_from_this(), std::placeholders::_1));
  return promise;
}

void AndroidAutoEntity::start(IAndroidAutoEntityEventHandler &eventHandler) {
  strand_.dispatch([this, self = this->shared_from_this(), eventHandler = &eventHandler]() {
    eventHandler_ = eventHandler;
    activity_->received();               // the watchdog's clock starts now
    for (auto &s : serviceList_) s->start();
    inputDevice_->start(link_);          // touches, keys, lights and icon taps go to this session from now on
    AA_DBG("aa: -> VERSION_REQUEST 1.6");
    controlServiceChannel_->sendVersionRequest(sendPromise());
    controlServiceChannel_->receive(this->shared_from_this());
    scheduleWatchdog();
  });
}

void AndroidAutoEntity::stop() {
  stopping_.store(true);
  strand_.dispatch([this, self = this->shared_from_this()]() {
    eventHandler_ = nullptr;
    inputDevice_->stop(link_);
    watchdog_.cancel();
    pinger_->cancel();
    try {
      for (auto &s : serviceList_) s->stop();
      messenger_->stop();
      transport_->stop();
    } catch (...) {
      AA_ERR("aa: exception while stopping the session");
    }
    // The TLS state goes a little later: a frame already read may still be on its way through the in-stream, and
    // aasdk's Cryptor has no guard against use after deinit().
    auto timer = std::make_shared<boost::asio::steady_timer>(ioService_, std::chrono::seconds(5));
    timer->async_wait([timer, cryptor = cryptor_](const boost::system::error_code &) { cryptor->deinit(); });
  });
}

void AndroidAutoEntity::scheduleWatchdog() {
  watchdog_.expires_after(std::chrono::seconds(1));
  watchdog_.async_wait(strand_.wrap([this, self = this->shared_from_this()](const boost::system::error_code &ec) {
    if (ec || stopping_) return;
    // With our pings a live phone is never silent this long.
    if (activity_->secondsSinceReceive() > options_.timeout) {
      triggerQuit(log::fmt("TimeoutError: timed out (nothing from the phone for %.0f s)", options_.timeout));
      return;
    }
    if (activity_->secondsSendBlocked() > options_.timeout) {
      triggerQuit(log::fmt("TimeoutError: timed out (a send to the phone blocked for %.0f s)", options_.timeout));
      return;
    }
    scheduleWatchdog();
  }));
}

void AndroidAutoEntity::onVersionResponse(uint16_t majorCode, uint16_t minorCode, aap_protobuf::shared::MessageStatus status) {
  // aasdk hands major and minor on in network byte order
  const int st = static_cast<int>(status);
  AA_DBG("aa: <- VERSION_RESPONSE %d.%d status %d", boost::endian::big_to_native(majorCode),
         boost::endian::big_to_native(minorCode), st);
  if (st == -1 || st == 0xffff) {
    triggerQuit("RuntimeError: phone says version mismatch");
    return;
  }
  try {
    cryptor_->doHandshake();
    controlServiceChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), sendPromise());
    controlServiceChannel_->receive(this->shared_from_this());
  } catch (const aasdk::error::Error &e) {
    onChannelError(e);
  }
}

void AndroidAutoEntity::onHandshake(const aasdk::common::DataConstBuffer &payload) {
  try {
    cryptor_->writeHandshakeBuffer(payload);
    if (!cryptor_->doHandshake()) {
      controlServiceChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), sendPromise());
    } else {
      // The last flight of the handshake, if there is one, goes out before AUTH_COMPLETE.
      auto pending = cryptor_->readHandshakeBuffer();
      if (!pending.empty()) controlServiceChannel_->sendHandshake(std::move(pending), sendPromise());
      AA_DBG("aa: TLS up: %s", tls_->describe().c_str());
      ctl::AuthResponse auth;
      auth.set_status(aap_protobuf::shared::STATUS_SUCCESS);   // AUTH_COMPLETE, status OK, sent unencrypted as in aasdk
      controlServiceChannel_->sendAuthComplete(auth, sendPromise());
      if (options_.ping > 0) schedulePing();
    }
    controlServiceChannel_->receive(this->shared_from_this());
  } catch (const aasdk::error::Error &e) {
    onChannelError(e);
  }
}

void AndroidAutoEntity::onServiceDiscoveryRequest(const ctl::ServiceDiscoveryRequest &request) {
  AA_DBG("aa: <- SERVICE_DISCOVERY_REQUEST on ch 0");
  AA_INFO("aa: phone \"%s\" / \"%s\"", request.label_text().c_str(), request.device_name().c_str());
  ctl::ServiceDiscoveryResponse response;
  for (auto &s : serviceList_) s->fillFeatures(response);
  // The head unit's names. This aasdk's .proto marks these fields deprecated in favour of headunit_info, but this
  // set is what works in the car, so it is what is sent.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  response.set_make("velosterAA");
  response.set_model("Veloster");
  response.set_year("2014");
  response.set_vehicle_id("0001");
  // Field 6, driver_position: 0 left (the Veloster), 1 right.
  response.set_driver_position(static_cast<ctl::DriverPosition>(options_.driverPosition));
  response.set_head_unit_make("veloster-aa");
  response.set_head_unit_model("aaview");
  response.set_head_unit_software_build("1");
  response.set_head_unit_software_version("0.1");
  response.set_can_play_native_media_during_vr(true);
#pragma GCC diagnostic pop
  // Field 12 (hide_clock in older protos) = 0. This .proto no longer has it, so it goes as an unknown field.
  response.GetReflection()->MutableUnknownFields(&response)->AddVarint(12, 0);
  controlServiceChannel_->sendServiceDiscoveryResponse(response, sendPromise());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onAudioFocusRequest(const ctl::AudioFocusRequest &request) {
  AA_DBG("aa: <- AUDIO_FOCUS_REQUEST on ch 0");
  // Request types: 1 gain, 2 gain transient, 3 transient may duck, 4 release. States: 1 gain, 2 gain transient,
  // 3 loss. As in OpenAuto, every request gets GAIN except a release, which gets LOSS. The head unit plays speech
  // over the music, so it never needs the phone to stop the music for it.
  const int t = static_cast<int>(request.audio_focus_type());
  const bool release = request.audio_focus_type() == ctl::AUDIO_FOCUS_RELEASE;
  const char *names[] = {"?", "gain", "gain transient", "transient may duck", "release"};
  AA_INFO("aa: audio focus request %s -> %s", t >= 1 && t <= 4 ? names[t] : std::to_string(t).c_str(),
          release ? "loss" : "gain");
  ctl::AudioFocusNotification response;
  response.set_focus_state(release ? ctl::AUDIO_FOCUS_STATE_LOSS : ctl::AUDIO_FOCUS_STATE_GAIN);
  controlServiceChannel_->sendAudioFocusResponse(response, sendPromise());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onByeByeRequest(const ctl::ByeByeRequest &request) {
  AA_DBG("aa: <- SHUTDOWN_REQUEST on ch 0");
  ctl::ByeByeResponse response;
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  const std::string reason =
      log::fmt("EOFError: phone shut the session down (reason %d)", static_cast<int>(request.reason()));
  promise->then([this, self = this->shared_from_this(), reason]() { triggerQuit(reason); },
                [this, self = this->shared_from_this(), reason](const aasdk::error::Error &) { triggerQuit(reason); });
  controlServiceChannel_->sendShutdownResponse(response, std::move(promise));
}

void AndroidAutoEntity::onByeByeResponse(const ctl::ByeByeResponse &) {
  triggerQuit("the phone answered our shutdown request");
}

void AndroidAutoEntity::onNavigationFocusRequest(const ctl::NavFocusRequestNotification &) {
  AA_DBG("aa: <- NAV_FOCUS_REQUEST on ch 0");
  ctl::NavFocusNotification response;
  response.set_focus_type(ctl::NAV_FOCUS_PROJECTED);
  controlServiceChannel_->sendNavigationFocusResponse(response, sendPromise());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onVoiceSessionRequest(const ctl::VoiceSessionNotification &request) {
  AA_DBG("aa: <- VOICE_SESSION_REQUEST on ch 0 %s", brief(request).c_str());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onBatteryStatusNotification(const ctl::BatteryStatusNotification &notification) {
  AA_DBG("aa: <- BATTERY_STATUS on ch 0 %s", brief(notification).c_str());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onPingRequest(const ctl::PingRequest &request) {
  ctl::PingResponse response;
  response.set_timestamp(request.timestamp());
  controlServiceChannel_->sendPingResponse(response, sendPromise());
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onPingResponse(const ctl::PingResponse &response) {
  pinger_->pong();
  const double ms = (static_cast<double>(monotonicMicros()) - static_cast<double>(response.timestamp())) / 1000;
  if (pinger_->pongs() == 1 || (ms > 1000 && ms < 60000))
    AA_LOG(ms > 1000 ? 4 : 6, "aa: ping answered in %d ms", static_cast<int>(ms));
  controlServiceChannel_->receive(this->shared_from_this());
}

void AndroidAutoEntity::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED || stopping_) return;
  triggerQuit(e.what());
}

void AndroidAutoEntity::triggerQuit(const std::string &reason) {
  strand_.dispatch([this, self = this->shared_from_this(), reason]() {
    if (quit_ || stopping_) return;
    quit_ = true;
    AA_WARN("aa: session ended: %s", reason.c_str());
    if (eventHandler_ != nullptr) eventHandler_->onAndroidAutoQuit(this);
  });
}

void AndroidAutoEntity::schedulePing() {
  // Our own ping every few seconds, as OpenAuto does. The phone answers each one, so a phone that is still there
  // always has something to send, and one that has gone out of range or dropped off Wi-Fi shows up as the
  // watchdog's timeout. Without pings a dead session once hung for 52 minutes.
  auto promise = IPinger::Promise::defer(strand_);
  promise->then(
      [this, self = this->shared_from_this()]() {
        sendPing();
        schedulePing();
      },
      [](const aasdk::error::Error &) {});
  pinger_->ping(std::move(promise));
}

void AndroidAutoEntity::sendPing() {
  ctl::PingRequest request;
  request.set_timestamp(static_cast<int64_t>(monotonicMicros()));
  controlServiceChannel_->sendPingRequest(request, sendPromise());
  if (pinger_->pings() == 4 && pinger_->pongs() == 0 && !warnedPings_) {
    warnedPings_ = true;
    AA_WARN("aa: the phone has not answered our first pings");
  }
}

}  // namespace aarecvd::service
