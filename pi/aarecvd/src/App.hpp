/*
 *  This file is part of aarecvd (veloster-aa), adapted from openauto
 *  (include/f1x/openauto/autoapp/App.hpp, src/autoapp/App.cpp).
 *  openauto: Copyright (C) 2018 f1x.studio (Michal Szwaj)
 *  aarecvd changes: Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Accepts the phone on TCP 5288 (OpenAuto uses 5000, plus USB), only from the access point's own subnet unless
// --any-client. One session at a time: a new connection replaces the old session, because it means the phone gave
// up on it and started again. Tells aarecvd --bt-only "session up" / "session ended" over a local datagram
// socket, and passes on taps on aaview's icon while there is no session.
#pragma once

#include <memory>
#include <string>

#include <aasdk/TCP/TCPWrapper.hpp>
#include <boost/asio.hpp>

#include "Projection/StreamRing.hpp"
#include "Service/IService.hpp"
#include "Service/ServiceFactory.hpp"

namespace aarecvd {

// The local datagram socket to aarecvd --bt-only (/run/veloaa/aarecv.sock). One line per datagram: "session up",
// "session ended", "icon <state>". Nothing is waited for; a missing listener is not an error.
class Notifier {
public:
  explicit Notifier(std::string path);
  ~Notifier();
  void send(const std::string &line);

private:
  std::string path_;
  int fd_ = -1;
  bool warned_ = false;
};

struct AppOptions {
  uint16_t port = 5288;
  bool anyClient = false;
  std::string apConf = "/etc/aarecv.conf";
};

class App : public service::IAndroidAutoEntityEventHandler, public std::enable_shared_from_this<App> {
public:
  App(boost::asio::io_service &ioService, const AppOptions &options, service::AndroidAutoEntityFactory &factory,
      std::shared_ptr<projection::StreamRing> ring, std::shared_ptr<Notifier> notifier);
  void start();
  void onAndroidAutoQuit(service::IAndroidAutoEntity *entity) override;

private:
  void accept();
  void handleNewClient(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code &err);
  void heartbeat();

  boost::asio::io_service &ioService_;
  boost::asio::io_service::strand strand_;
  AppOptions options_;
  boost::asio::ip::tcp::acceptor acceptor_;
  aasdk::tcp::TCPWrapper tcpWrapper_;
  service::AndroidAutoEntityFactory &factory_;
  std::shared_ptr<projection::StreamRing> ring_;
  std::shared_ptr<Notifier> notifier_;
  service::IAndroidAutoEntity::Pointer entity_;
  boost::asio::steady_timer stateTimer_;
};

}  // namespace aarecvd
