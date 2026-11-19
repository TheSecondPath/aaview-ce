/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "RingOutputs.hpp"
#include "../Log.hpp"

#include <algorithm>
#include <set>

namespace aarecvd::projection {

using Clock = std::chrono::steady_clock;

// ---------------- video ----------------

RingVideoOutput::RingVideoOutput(std::shared_ptr<StreamRing> ring, std::shared_ptr<SessionStats> stats, int level,
                                 size_t dpi, int fps)
    : ring_(std::move(ring)), stats_(std::move(stats)), level_(level), dpi_(dpi), fps_(fps),
      lastStat_(Clock::now()) {}

aap_protobuf::service::media::sink::message::VideoFrameRateType RingVideoOutput::getVideoFPS() const {
  // In the enum, 1 is VIDEO_FPS_60 and 2 is VIDEO_FPS_30.
  return fps_ == 60 ? aap_protobuf::service::media::sink::message::VIDEO_FPS_60
                    : aap_protobuf::service::media::sink::message::VIDEO_FPS_30;
}

aap_protobuf::service::media::sink::message::VideoCodecResolutionType RingVideoOutput::getVideoResolution() const {
  return aap_protobuf::service::media::sink::message::VIDEO_800x480;   // 1 ("480p")
}

void RingVideoOutput::write(aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer &buffer) {
  const uint8_t *data = buffer.cdata;
  size_t size = buffer.size;
  auto types = nalTypes(data, size);
  if (types.empty()) return;
  std::set<int> ts;
  for (auto &t : types) ts.insert(t.first);
  const bool slice = ts.count(1) || ts.count(5);
  if (ts.count(7)) {                   // codec config, or a frame carrying its own SPS/PPS
    size_t firstSlice = size;
    if (slice) {
      for (auto &t : types)
        if (t.first == 1 || t.first == 5) firstSlice = std::min(firstSlice, t.second);
      if (firstSlice && data[firstSlice - 1] == 0) firstSlice -= 1;   // 4-byte start code
    }
    std::vector<uint8_t> spsPps(data, data + firstSlice);
    if (level_) {
      for (auto &t : nalTypes(spsPps.data(), spsPps.size())) {
        if (t.first == 7 && t.second + 6 < spsPps.size()) {
          if (config_.empty())
            AA_DBG("aa: SPS profile %d level %d -> %d", spsPps[t.second + 4], spsPps[t.second + 6], level_);
          spsPps[t.second + 6] = static_cast<uint8_t>(level_);
        }
      }
    }
    config_ = std::move(spsPps);
    if (!slice) return;
    data += firstSlice;
    size -= firstSlice;
  }
  frames_ += 1;
  if (ts.count(5)) {
    idrs_ += 1;
    frame_.assign(config_.begin(), config_.end());
    frame_.insert(frame_.end(), data, data + size);
    ring_->put(frame_.data(), frame_.size(), true);
  } else {
    ring_->put(data, size, false);
  }
  if (Clock::now() - lastStat_ > std::chrono::seconds(10)) {
    lastStat_ = Clock::now();
    auto hu = ring_->hu();
    const bool alive = ring_->huAlive();
    const uint32_t late = stats_->audioLate.exchange(0), worst = stats_->audioWorstMs.exchange(0);
    AA_DBG("aa: video %u frames (%u IDR), ring dropped %u, aaview %s%s", frames_, idrs_, ring_->dropped(),
           alive && hu ? log::fmt("drawn %u", hu->decoded).c_str() : "not running",
           late ? log::fmt("; media audio late %u times, worst gap %u ms", late, worst).c_str() : "");
  }
}

// ---------------- audio out ----------------

RingAudioOutput::RingAudioOutput(std::shared_ptr<StreamRing> ring, int stream, uint32_t channels, uint32_t bits,
                                 uint32_t rate)
    : ring_(std::move(ring)), stream_(stream), channels_(channels), bits_(bits), rate_(rate) {}

bool RingAudioOutput::write(aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer &buffer) {
  return ring_->put(buffer.cdata, buffer.size, false, StreamRing::REC_AUDIO, stream_);
}

// ---------------- microphone ----------------

RingAudioInput::RingAudioInput(std::shared_ptr<StreamRing> ring) : ring_(std::move(ring)) {}

void RingAudioInput::start(StartPromise::Pointer promise) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    queue_.clear();
    chunks_ = 0;
    t0_ = Clock::now();
    warned_ = false;
  }
  ring_->setMic(true);
  active_ = true;
  promise->resolve();
}

void RingAudioInput::stop() {
  active_ = false;
  ring_->setMic(false);
  std::lock_guard<std::mutex> l(mutex_);
  queue_.clear();
  if (promise_) {
    promise_->reject();
    promise_.reset();
  }
}

double RingAudioInput::seconds() const {
  std::lock_guard<std::mutex> l(mutex_);
  return std::chrono::duration<double>(Clock::now() - t0_).count();
}

void RingAudioInput::read(ReadPromise::Pointer promise) {
  std::lock_guard<std::mutex> l(mutex_);
  if (!active_) {
    promise->reject();
    return;
  }
  if (!queue_.empty()) {
    auto d = std::move(queue_.front());
    queue_.pop_front();
    promise->resolve(std::move(d));
  } else {
    promise_ = std::move(promise);
  }
}

void RingAudioInput::poll() {
  // The car's microphone, recorded by aaview (62.5 ms chunks), on to the phone as it comes.
  if (!active_) return;
  auto chunks = ring_->pollMic();
  std::lock_guard<std::mutex> l(mutex_);
  for (auto &c : chunks) {
    chunks_ += 1;
    queue_.emplace_back(c.begin(), c.end());
  }
  if (promise_ && !queue_.empty()) {
    auto d = std::move(queue_.front());
    queue_.pop_front();
    promise_->resolve(std::move(d));
    promise_.reset();
  }
  if (!chunks_ && !warned_ && Clock::now() - t0_ > std::chrono::seconds(2)) {
    warned_ = true;
    AA_WARN("aa: microphone: nothing from the head unit after 2 s (%s)",
            ring_->huCanRecord() ? "aaview is recording-capable; see its log"
                                 : "aaview is not running, or is older than 0.7");
  }
}

}  // namespace aarecvd::projection
