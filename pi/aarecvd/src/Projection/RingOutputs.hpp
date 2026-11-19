/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The stream-ring back ends behind OpenAuto's projection interfaces, in place of its Qt/OMX/RtAudio classes:
//   RingVideoOutput        H.264 access units into the ring. SPS/PPS are kept and put in front of every IDR, and
//                          the SPS level_idc is rewritten.
//   RingAudioOutput        PCM records for one of aaview's three audio streams
//   RingAudioInput         the cabin microphone from aaview's mic slots while the header asks for it
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

#include "Interfaces.hpp"
#include "StreamRing.hpp"

namespace aarecvd::projection {

// Per-session counters that several services add to.
struct SessionStats {
  std::atomic<uint32_t> audioLate{0};       // media audio chunks that came 150 ms..1.5 s after the one before
  std::atomic<uint32_t> audioWorstMs{0};
  std::atomic<uint32_t> audioDrops{0};      // audio chunks the ring had no room for (all channels)
};

class RingVideoOutput : public IVideoOutput {
public:
  RingVideoOutput(std::shared_ptr<StreamRing> ring, std::shared_ptr<SessionStats> stats, int level, size_t dpi,
                  int fps);
  bool open() override { return true; }
  bool init() override { return true; }
  void write(aasdk::messenger::Timestamp::ValueType timestamp, const aasdk::common::DataConstBuffer &buffer) override;
  void stop() override {}
  aap_protobuf::service::media::sink::message::VideoFrameRateType getVideoFPS() const override;
  aap_protobuf::service::media::sink::message::VideoCodecResolutionType getVideoResolution() const override;
  size_t getScreenDPI() const override { return dpi_; }
  uint32_t getWidthMargin() const override { return 0; }
  uint32_t getHeightMargin() const override { return 0; }

private:
  std::shared_ptr<StreamRing> ring_;
  std::shared_ptr<SessionStats> stats_;
  int level_;
  size_t dpi_;
  int fps_;
  std::vector<uint8_t> config_;             // SPS + PPS, sent again in front of every IDR
  std::vector<uint8_t> frame_;
  uint32_t frames_ = 0, idrs_ = 0;
  std::chrono::steady_clock::time_point lastStat_;
};

class RingAudioOutput : public IAudioOutput {
public:
  RingAudioOutput(std::shared_ptr<StreamRing> ring, int stream, uint32_t channels, uint32_t bits, uint32_t rate);
  bool open() override { return true; }
  bool write(aasdk::messenger::Timestamp::ValueType timestamp, const aasdk::common::DataConstBuffer &buffer) override;
  void start() override {}
  void stop() override {}
  void suspend() override {}
  uint32_t getSampleSize() const override { return bits_; }
  uint32_t getChannelCount() const override { return channels_; }
  uint32_t getSampleRate() const override { return rate_; }

private:
  std::shared_ptr<StreamRing> ring_;
  int stream_;
  uint32_t channels_, bits_, rate_;
};

class RingAudioInput : public IAudioInput {
public:
  explicit RingAudioInput(std::shared_ptr<StreamRing> ring);
  bool open() override { return true; }
  bool isActive() const override { return active_; }
  void read(ReadPromise::Pointer promise) override;
  void start(StartPromise::Pointer promise) override;
  void stop() override;
  void poll() override;
  uint32_t getSampleSize() const override { return 16; }
  uint32_t getChannelCount() const override { return 1; }
  uint32_t getSampleRate() const override { return 16000; }
  // for the "microphone closed" line
  uint32_t chunks() const { return chunks_; }
  double seconds() const;
  uint32_t lost() const { return ring_->micLost(); }

private:
  std::shared_ptr<StreamRing> ring_;
  std::atomic<bool> active_{false};
  mutable std::mutex mutex_;
  std::deque<aasdk::common::Data> queue_;
  ReadPromise::Pointer promise_;
  std::atomic<uint32_t> chunks_{0};
  std::chrono::steady_clock::time_point t0_;
  bool warned_ = false;
};

}  // namespace aarecvd::projection
