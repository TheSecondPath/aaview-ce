/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto (src/autoapp/Service/Pinger.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "Pinger.hpp"

namespace aarecvd::service {

Pinger::Pinger(boost::asio::io_service &ioService, time_t durationMs)
    : strand_(ioService), timer_(ioService), duration_(durationMs), cancelled_(false), pingsCount_(0),
      pongsCount_(0) {}

void Pinger::ping(Promise::Pointer promise) {
  strand_.dispatch([this, self = this->shared_from_this(), promise = std::move(promise)]() mutable {
    if (cancelled_) {
      promise->reject(aasdk::error::Error(aasdk::error::ErrorCode::OPERATION_ABORTED));
    } else if (promise_ != nullptr) {
      promise->reject(aasdk::error::Error(aasdk::error::ErrorCode::OPERATION_IN_PROGRESS));
    } else {
      promise_ = std::move(promise);
      timer_.expires_from_now(boost::posix_time::milliseconds(duration_));
      timer_.async_wait(strand_.wrap(std::bind(&Pinger::onTimerExceeded, this->shared_from_this(),
                                               std::placeholders::_1)));
    }
  });
}

void Pinger::pong() { ++pongsCount_; }

void Pinger::onTimerExceeded(const boost::system::error_code &error) {
  if (promise_ == nullptr) return;
  if (error == boost::asio::error::operation_aborted || cancelled_) {
    promise_->reject(aasdk::error::Error(aasdk::error::ErrorCode::OPERATION_ABORTED));
  } else {
    ++pingsCount_;
    promise_->resolve();
  }
  promise_.reset();
}

void Pinger::cancel() {
  strand_.dispatch([this, self = this->shared_from_this()]() {
    cancelled_ = true;
    timer_.cancel();
  });
}

}  // namespace aarecvd::service
