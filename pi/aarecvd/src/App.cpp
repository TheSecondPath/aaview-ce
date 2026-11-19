/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto (src/autoapp/App.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "App.hpp"
#include "Config.hpp"
#include "Log.hpp"

#include <aasdk/TCP/TCPEndpoint.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace aarecvd {

// ---------------- Notifier ----------------

Notifier::Notifier(std::string path) : path_(std::move(path)) {
  if (!path_.empty()) fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

Notifier::~Notifier() {
  if (fd_ >= 0) ::close(fd_);
}

void Notifier::send(const std::string &line) {
  if (fd_ < 0) return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
  if (::sendto(fd_, line.data(), line.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
    if (!warned_) {   // said once; aarecvd --bt-only may simply not be running
      warned_ = true;
      AA_DBG("aa: nobody listening on %s (%s)", path_.c_str(), std::strerror(errno));
    }
  } else {
    warned_ = false;
  }
}

// ---------------- App ----------------

App::App(boost::asio::io_service &ioService, const AppOptions &options, service::AndroidAutoEntityFactory &factory,
         std::shared_ptr<projection::StreamRing> ring, std::shared_ptr<Notifier> notifier)
    : ioService_(ioService), strand_(ioService), options_(options),
      acceptor_(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), options.port), true),
      factory_(factory), ring_(std::move(ring)), notifier_(std::move(notifier)), stateTimer_(ioService) {}

void App::start() {
  strand_.dispatch([this, self = shared_from_this()]() {
    AA_INFO("aa: listening on TCP %d", static_cast<int>(options_.port));
    accept();
    heartbeat();
  });
}

void App::heartbeat() {
  // Repeat the session state every 10 s so an aarecvd --bt-only that restarted learns it without waiting for a change.
  notifier_->send(entity_ ? "session up" : "session ended");
  stateTimer_.expires_after(std::chrono::seconds(10));
  stateTimer_.async_wait(strand_.wrap([this, self = shared_from_this()](const boost::system::error_code &ec) {
    if (!ec) heartbeat();
  }));
}

void App::accept() {
  auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService_);
  acceptor_.async_accept(*socket, strand_.wrap(std::bind(&App::handleNewClient, shared_from_this(), socket,
                                                         std::placeholders::_1)));
}

void App::handleNewClient(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code &err) {
  if (err) {
    AA_ERR("aa: accept: %s", err.message().c_str());
    accept();
    return;
  }
  boost::system::error_code ec;
  auto peer = socket->remote_endpoint(ec);
  const std::string ip = ec ? "?" : peer.address().to_string();
  const int port = ec ? 0 : peer.port();
  if (!(options_.anyClient || config::apPeer(ip, options_.apConf))) {
    AA_WARN("aa: connection from %s:%d refused: not on the access point (see --any-client)", ip.c_str(), port);
    socket->close(ec);
    accept();
    return;
  }
  socket->set_option(boost::asio::ip::tcp::no_delay(true), ec);
  socket->set_option(boost::asio::socket_base::keep_alive(true), ec);
  AA_INFO("aa: phone connected from %s:%d", ip.c_str(), port);
  if (entity_) {
    // The phone gave up on the old connection and started again: drop the old one.
    AA_WARN("aa: replacing the previous session");
    entity_->stop();
    entity_.reset();
  }
  try {
    auto endpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper_, std::move(socket));
    entity_ = factory_.create(std::move(endpoint));
    ring_->setPhone(true);
    notifier_->send("session up");
    entity_->start(*this);
  } catch (const std::exception &e) {
    AA_ERR("aa: session could not start: %s", e.what());
    entity_.reset();
    ring_->setPhone(false);
    notifier_->send("session ended");
  }
  accept();
}

void App::onAndroidAutoQuit(service::IAndroidAutoEntity *entity) {
  strand_.dispatch([this, self = shared_from_this(), entity]() {
    if (!entity_ || entity_.get() != entity) return;   // already replaced by a newer connection
    entity_->stop();
    entity_.reset();
    ring_->setProjecting(false);   // aaview hides and gives the screen back to the LG shell
    ring_->setPhone(false);
    ring_->setMic(false);          // in case the session ended while the assistant was listening
    notifier_->send("session ended");
  });
}

}  // namespace aarecvd
