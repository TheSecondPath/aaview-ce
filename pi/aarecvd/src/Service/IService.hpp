/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/IService.hpp, IAndroidAutoEntity.hpp, IAndroidAutoEntityEventHandler.hpp,
 *  IPinger.hpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aasdk/Error/Error.hpp>
#include <aasdk/IO/Promise.hpp>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>

namespace aarecvd::service {

class IService {
public:
  typedef std::shared_ptr<IService> Pointer;
  virtual ~IService() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void pause() {}
  virtual void resume() {}
  virtual void fillFeatures(aap_protobuf::service::control::message::ServiceDiscoveryResponse &response) = 0;
};

typedef std::vector<IService::Pointer> ServiceList;

class IAndroidAutoEntity;

class IAndroidAutoEntityEventHandler {
public:
  virtual ~IAndroidAutoEntityEventHandler() = default;
  // Unlike OpenAuto's, this says which session quit, so a late quit from a replaced session cannot end its successor.
  virtual void onAndroidAutoQuit(IAndroidAutoEntity *entity) = 0;
};

class IAndroidAutoEntity {
public:
  typedef std::shared_ptr<IAndroidAutoEntity> Pointer;
  virtual ~IAndroidAutoEntity() = default;
  virtual void start(IAndroidAutoEntityEventHandler &eventHandler) = 0;
  virtual void stop() = 0;
  virtual void pause() {}
  virtual void resume() {}
};

class IPinger {
public:
  typedef std::shared_ptr<IPinger> Pointer;
  typedef aasdk::io::Promise<void> Promise;
  virtual ~IPinger() = default;
  virtual void ping(Promise::Pointer promise) = 0;
  virtual void pong() = 0;
  virtual void cancel() = 0;
};

// Command-line settings the services use.
struct Options {
  int unacked = 8;                 // video frames the phone may send before our ack
  int audioUnacked = 16;           // audio chunks the phone may send before our ack
  int level = 12;                  // rewrite SPS level_idc (0 = leave it)
  int dpi = 140;
  // The phone reads 1 as VIDEO_FPS_60 and DRIVER_POSITION_RIGHT, as this aasdk and aa-proxy-rs do; the older aasdk
  // numbering had 1 as 30 fps and left-hand drive. At 60 fps the phone sends 20+ frames/s even over a weak link, and
  // music waits behind them: gaps of about 2 s, then a rush. Hence 30 by default.
  int fps = 30;                    // 30 or 60
  int driverPosition = 0;          // 0 left, 1 right, 2 centre
  double ping = 5;                // seconds between our pings to the phone (0 = none)
  double timeout = 15;             // seconds of silence (or of a blocked send) after which the phone is gone
  bool statusServices = false;     // offer PhoneStatus, MediaPlaybackStatus and NavigationStatus (logged only)
  std::string keysConf = "/etc/aarecv-keys.conf";
  // The head unit's TLS certificate and key (PEM). They are used when both exist and belong together; otherwise
  // aasdk's built-in pair is used (the JVC Kenwood one compiled into libaasdk). Empty paths mean always the built-in
  // pair.
  std::string certFile = "/etc/aarecv/hu.crt";
  std::string keyFile = "/etc/aarecv/hu.key";
};

}  // namespace aarecvd::service
