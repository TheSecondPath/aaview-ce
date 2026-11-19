/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto (src/autoapp/Service/Sensor/SensorService.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "SensorService.hpp"
#include "../../Log.hpp"

namespace aarecvd::service::sensor {

namespace msg = aap_protobuf::service;

SensorService::SensorService(boost::asio::io_service &ioService, aasdk::messenger::IMessenger::Pointer messenger,
                             std::shared_ptr<projection::RingInputDevice> lights)
    : strand_(ioService),
      channel_(std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(strand_, std::move(messenger))),
      lights_(std::move(lights)) {}

void SensorService::start() {
  strand_.dispatch([this, self = this->shared_from_this()]() { channel_->receive(this->shared_from_this()); });
}

void SensorService::fillFeatures(msg::control::message::ServiceDiscoveryResponse &response) {
  auto *service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel_->getId()));
  auto *sensorChannel = service->mutable_sensor_source_service();
  sensorChannel->add_sensors()->set_sensor_type(msg::sensorsource::message::SENSOR_DRIVING_STATUS_DATA);   // 13
  sensorChannel->add_sensors()->set_sensor_type(msg::sensorsource::message::SENSOR_NIGHT_MODE);            // 10
}

void SensorService::onChannelOpenRequest(const msg::control::message::ChannelOpenRequest &) {
  AA_DBG("aa: <- CHANNEL_OPEN_REQUEST on ch %d", static_cast<int>(channel_->getId()));
  msg::control::message::ChannelOpenResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&SensorService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendChannelOpenResponse(response, std::move(promise));
  channel_->receive(this->shared_from_this());
}

void SensorService::onSensorStartRequest(const msg::sensorsource::message::SensorRequest &request) {
  AA_DBG("aa: sensor start request type %d", static_cast<int>(request.type()));
  msg::sensorsource::message::SensorStartResponseMessage response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&SensorService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendSensorStartResponse(response, std::move(promise));
  if (request.type() == msg::sensorsource::message::SENSOR_DRIVING_STATUS_DATA) {
    msg::sensorsource::message::SensorBatch batch;
    batch.add_driving_status_data()->set_status(0);   // unrestricted
    auto p = aasdk::channel::SendPromise::defer(strand_);
    p->then([]() {}, std::bind(&SensorService::onChannelError, this->shared_from_this(), std::placeholders::_1));
    channel_->sendSensorEventIndication(batch, std::move(p));
  } else if (request.type() == msg::sensorsource::message::SENSOR_NIGHT_MODE) {
    nightOn_ = true;
    sendNight();
  }
  channel_->receive(this->shared_from_this());
}

void SensorService::sendNight() {
  const bool night = lights_->night();
  AA_INFO("aa: night mode %s", night ? "on" : "off");
  msg::sensorsource::message::SensorBatch batch;
  batch.add_night_mode_data()->set_night_mode(night);
  auto promise = aasdk::channel::SendPromise::defer(strand_);
  promise->then([]() {}, std::bind(&SensorService::onChannelError, this->shared_from_this(), std::placeholders::_1));
  channel_->sendSensorEventIndication(batch, std::move(promise));
}

void SensorService::onNightModeChanged() {
  strand_.dispatch([this, self = this->shared_from_this()]() {
    if (nightOn_) sendNight();
  });
}

void SensorService::onChannelError(const aasdk::error::Error &e) {
  if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) return;   // the session is stopping
  AA_DBG("aa: sensor channel: %s", e.what());
}

}  // namespace aarecvd::service::sensor
