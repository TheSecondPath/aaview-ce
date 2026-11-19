/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/AndroidAutoEntity.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <atomic>

#include <aasdk/Channel/Control/IControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Transport/ITransport.hpp>
#include <boost/asio.hpp>

#include "../Projection/RingInputDevice.hpp"
#include "IService.hpp"
#include "Pinger.hpp"
#include "ServiceFactory.hpp"
#include "Transport.hpp"

namespace aarecvd::service {

class AndroidAutoEntity : public IAndroidAutoEntity,
                          public aasdk::channel::control::IControlServiceChannelEventHandler,
                          public std::enable_shared_from_this<AndroidAutoEntity> {
public:
  AndroidAutoEntity(boost::asio::io_service &ioService, aasdk::messenger::ICryptor::Pointer cryptor,
                    std::shared_ptr<TlsWrapper> tls, aasdk::transport::ITransport::Pointer transport,
                    aasdk::messenger::IMessenger::Pointer messenger, const Options &options, ServiceList serviceList,
                    std::shared_ptr<Pinger> pinger, Activity::Pointer activity, std::shared_ptr<SessionLink> link,
                    std::shared_ptr<projection::RingInputDevice> inputDevice);
  ~AndroidAutoEntity() override;

  void start(IAndroidAutoEntityEventHandler &eventHandler) override;
  void stop() override;

  void onVersionResponse(uint16_t majorCode, uint16_t minorCode, aap_protobuf::shared::MessageStatus status) override;
  void onHandshake(const aasdk::common::DataConstBuffer &payload) override;
  void onServiceDiscoveryRequest(const aap_protobuf::service::control::message::ServiceDiscoveryRequest &request) override;
  void onAudioFocusRequest(const aap_protobuf::service::control::message::AudioFocusRequest &request) override;
  void onByeByeRequest(const aap_protobuf::service::control::message::ByeByeRequest &request) override;
  void onByeByeResponse(const aap_protobuf::service::control::message::ByeByeResponse &response) override;
  void onNavigationFocusRequest(
      const aap_protobuf::service::control::message::NavFocusRequestNotification &request) override;
  void onVoiceSessionRequest(const aap_protobuf::service::control::message::VoiceSessionNotification &request) override;
  void onBatteryStatusNotification(
      const aap_protobuf::service::control::message::BatteryStatusNotification &notification) override;
  void onPingResponse(const aap_protobuf::service::control::message::PingResponse &response) override;
  void onPingRequest(const aap_protobuf::service::control::message::PingRequest &request) override;
  void onChannelError(const aasdk::error::Error &e) override;

private:
  using std::enable_shared_from_this<AndroidAutoEntity>::shared_from_this;
  void triggerQuit(const std::string &reason);
  void schedulePing();
  void sendPing();
  void scheduleWatchdog();
  aasdk::channel::SendPromise::Pointer sendPromise();

  boost::asio::io_service &ioService_;
  boost::asio::io_service::strand strand_;
  aasdk::messenger::ICryptor::Pointer cryptor_;
  std::shared_ptr<TlsWrapper> tls_;
  aasdk::transport::ITransport::Pointer transport_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  aasdk::channel::control::IControlServiceChannel::Pointer controlServiceChannel_;
  Options options_;
  ServiceList serviceList_;
  std::shared_ptr<Pinger> pinger_;
  Activity::Pointer activity_;
  std::shared_ptr<SessionLink> link_;
  std::shared_ptr<projection::RingInputDevice> inputDevice_;
  boost::asio::steady_timer watchdog_;
  IAndroidAutoEntityEventHandler *eventHandler_ = nullptr;
  std::atomic<bool> stopping_{false};
  bool quit_ = false;
  bool warnedPings_ = false;
};

}  // namespace aarecvd::service
