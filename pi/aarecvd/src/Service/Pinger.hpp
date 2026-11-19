/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/Service/Pinger.hpp, src/autoapp/Service/Pinger.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The interval timer for our pings. Unlike OpenAuto's, it never gives up on missing answers by itself. The pings only
// make sure a live phone always has something to send back; the session watchdog (nothing received for --timeout s)
// is what ends a dead session.
#pragma once

#include <atomic>
#include <boost/asio.hpp>

#include "IService.hpp"

namespace aarecvd::service {

class Pinger : public IPinger, public std::enable_shared_from_this<Pinger> {
public:
  Pinger(boost::asio::io_service &ioService, time_t durationMs);
  void ping(Promise::Pointer promise) override;
  void pong() override;
  void cancel() override;
  uint32_t pings() const { return pingsCount_; }
  uint32_t pongs() const { return pongsCount_; }

private:
  using std::enable_shared_from_this<Pinger>::shared_from_this;
  void onTimerExceeded(const boost::system::error_code &error);

  boost::asio::io_service::strand strand_;
  boost::asio::deadline_timer timer_;
  time_t duration_;
  bool cancelled_;
  Promise::Pointer promise_;
  std::atomic<uint32_t> pingsCount_, pongsCount_;
};

}  // namespace aarecvd::service
