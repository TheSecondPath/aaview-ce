/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/ServiceFactory.hpp, AndroidAutoEntityFactory.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/TCP/ITCPEndpoint.hpp>
#include <boost/asio.hpp>

#include "../Projection/RingInputDevice.hpp"
#include "../Projection/RingOutputs.hpp"
#include "IService.hpp"

namespace aarecvd::service {

namespace mediasink {
class VideoService;
}
namespace sensor {
class SensorService;
}
namespace inputsource {
class InputSourceService;
}

// Passes what the ring poller thread reads on to the session's services. In OpenAuto the Qt input device calls
// InputSourceService directly.
class SessionLink : public projection::IInputDeviceEventHandler {
public:
  void onButtonEvent(const projection::ButtonEvent &event) override;
  void onTouchEvent(const projection::TouchEvent &event) override;
  void onNightModeChanged(bool night) override;
  void onIconTap(int state) override;
  void onPoll() override;

  std::shared_ptr<inputsource::InputSourceService> input;
  std::shared_ptr<sensor::SensorService> sensor;
  std::shared_ptr<mediasink::VideoService> video;
  std::shared_ptr<projection::RingAudioInput> microphone;
};

class ServiceFactory {
public:
  ServiceFactory(boost::asio::io_service &ioService, const Options &options, std::shared_ptr<projection::StreamRing> ring,
                 std::shared_ptr<projection::RingInputDevice> inputDevice);
  // The services, in service discovery order: input, sensor, video, media, speech, system, microphone, and with
  // --status-services also phone status, media playback and navigation.
  ServiceList create(aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<SessionLink> &link);

private:
  boost::asio::io_service &ioService_;
  Options options_;
  std::shared_ptr<projection::StreamRing> ring_;
  std::shared_ptr<projection::RingInputDevice> inputDevice_;
};

struct HeadUnitCert;   // defined in Transport.hpp

class AndroidAutoEntityFactory {
public:
  AndroidAutoEntityFactory(boost::asio::io_service &ioService, const Options &options, ServiceFactory &serviceFactory,
                           std::shared_ptr<projection::RingInputDevice> inputDevice);
  IAndroidAutoEntity::Pointer create(aasdk::tcp::ITCPEndpoint::Pointer tcpEndpoint);

private:
  boost::asio::io_service &ioService_;
  Options options_;
  ServiceFactory &serviceFactory_;
  std::shared_ptr<projection::RingInputDevice> inputDevice_;
  std::shared_ptr<const HeadUnitCert> cert_;   // from --cert / --key, or nullptr for aasdk's built-in one
};

}  // namespace aarecvd::service
