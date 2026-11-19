/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (src/autoapp/Service/ServiceFactory.cpp, AndroidAutoEntityFactory.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "ServiceFactory.hpp"

#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include "AndroidAutoEntity.hpp"
#include "InputSource/InputSourceService.hpp"
#include "MediaPlaybackStatus/MediaPlaybackStatusService.hpp"
#include "MediaSink/AudioService.hpp"
#include "MediaSink/VideoService.hpp"
#include "MediaSource/MicrophoneService.hpp"
#include "NavigationStatus/NavigationStatusService.hpp"
#include "PhoneStatus/PhoneStatusService.hpp"
#include "Pinger.hpp"
#include "Sensor/SensorService.hpp"
#include "Transport.hpp"

namespace aarecvd::service {

// ---------------- SessionLink ----------------

void SessionLink::onButtonEvent(const projection::ButtonEvent &event) {
  if (input) input->onButtonEvent(event);
}
void SessionLink::onTouchEvent(const projection::TouchEvent &event) {
  if (input) input->onTouchEvent(event);
}
void SessionLink::onNightModeChanged(bool) {
  if (sensor) sensor->onNightModeChanged();
}
void SessionLink::onIconTap(int state) {
  if (video) video->onIconTap(state);
}
void SessionLink::onPoll() {
  if (video) video->keyframeWatch();
  if (microphone) microphone->poll();
}

// ---------------- ServiceFactory ----------------

ServiceFactory::ServiceFactory(boost::asio::io_service &ioService, const Options &options,
                               std::shared_ptr<projection::StreamRing> ring,
                               std::shared_ptr<projection::RingInputDevice> inputDevice)
    : ioService_(ioService), options_(options), ring_(std::move(ring)), inputDevice_(std::move(inputDevice)) {}

ServiceList ServiceFactory::create(aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<SessionLink> &link) {
  using mediasink::AudioService;
  ServiceList list;
  link = std::make_shared<SessionLink>();
  auto stats = std::make_shared<projection::SessionStats>();

  link->input = std::make_shared<inputsource::InputSourceService>(ioService_, messenger, inputDevice_);
  list.push_back(link->input);
  link->sensor = std::make_shared<sensor::SensorService>(ioService_, messenger, inputDevice_);
  list.push_back(link->sensor);
  auto videoOutput = std::make_shared<projection::RingVideoOutput>(ring_, stats, options_.level, options_.dpi, options_.fps);
  link->video = std::make_shared<mediasink::VideoService>(ioService_, messenger, videoOutput, ring_, options_);
  list.push_back(link->video);
  // The audio channels map to aaview's streams: 0 media (48 kHz stereo), 1 guidance and 2 system (16 kHz mono).
  list.push_back(std::make_shared<AudioService>(ioService_, messenger, AudioService::Kind::MEDIA,
                                                std::make_shared<projection::RingAudioOutput>(ring_, 0, 2, 16, 48000),
                                                ring_, stats, options_));
  list.push_back(std::make_shared<AudioService>(ioService_, messenger, AudioService::Kind::GUIDANCE,
                                                std::make_shared<projection::RingAudioOutput>(ring_, 1, 1, 16, 16000),
                                                ring_, stats, options_));
  list.push_back(std::make_shared<AudioService>(ioService_, messenger, AudioService::Kind::SYSTEM,
                                                std::make_shared<projection::RingAudioOutput>(ring_, 2, 1, 16, 16000),
                                                ring_, stats, options_));
  link->microphone = std::make_shared<projection::RingAudioInput>(ring_);
  list.push_back(std::make_shared<mediasource::MicrophoneService>(ioService_, messenger, link->microphone));
  if (options_.statusServices) {
    list.push_back(std::make_shared<phonestatus::PhoneStatusService>(ioService_, messenger));
    list.push_back(std::make_shared<mediaplaybackstatus::MediaPlaybackStatusService>(ioService_, messenger));
    list.push_back(std::make_shared<navigationstatus::NavigationStatusService>(ioService_, messenger));
  }
  return list;
}

// ---------------- AndroidAutoEntityFactory ----------------

AndroidAutoEntityFactory::AndroidAutoEntityFactory(boost::asio::io_service &ioService, const Options &options,
                                                   ServiceFactory &serviceFactory,
                                                   std::shared_ptr<projection::RingInputDevice> inputDevice)
    : ioService_(ioService), options_(options), serviceFactory_(serviceFactory), inputDevice_(std::move(inputDevice)),
      cert_(loadHeadUnitCert(options.certFile, options.keyFile)) {}

IAndroidAutoEntity::Pointer AndroidAutoEntityFactory::create(aasdk::tcp::ITCPEndpoint::Pointer tcpEndpoint) {
  auto activity = std::make_shared<Activity>();
  auto watched = std::make_shared<WatchedTCPEndpoint>(ioService_, std::move(tcpEndpoint), activity);
  auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService_, std::move(watched));
  auto tls = std::make_shared<TlsWrapper>(cert_);
  auto cryptor = std::make_shared<aasdk::messenger::Cryptor>(tls);
  cryptor->init();   // the head-unit certificate and key: from --cert / --key, or aasdk's built-in pair

  auto messenger = std::make_shared<aasdk::messenger::Messenger>(
      ioService_, std::make_shared<aasdk::messenger::MessageInStream>(ioService_, transport, cryptor),
      std::make_shared<aasdk::messenger::MessageOutStream>(ioService_, transport, cryptor));

  std::shared_ptr<SessionLink> link;
  auto serviceList = serviceFactory_.create(messenger, link);
  const double ping = options_.ping > 0 ? options_.ping : 5;
  auto pinger = std::make_shared<Pinger>(ioService_, static_cast<time_t>(ping * 1000));
  return std::make_shared<AndroidAutoEntity>(ioService_, std::move(cryptor), tls, std::move(transport),
                                             std::move(messenger), options_, std::move(serviceList), std::move(pinger),
                                             activity, std::move(link), inputDevice_);
}

}  // namespace aarecvd::service
