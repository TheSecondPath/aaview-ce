/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "BtOnly.hpp"
#include "../Log.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>

namespace aarecvd::bt {

namespace {

struct State {
  bool active = false;
  std::unique_ptr<Bluetooth> bt;
};

int onDatagram(sd_event_source *, int fd, uint32_t, void *u) {
  auto *st = static_cast<State *>(u);
  char buf[257];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
    if (n < 0) break;
    std::string msg(buf, static_cast<size_t>(n));
    for (auto &c : msg)
      if (static_cast<unsigned char>(c) >= 0x80) c = '?';
    auto b = msg.find_first_not_of(" \t\r\n"), e = msg.find_last_not_of(" \t\r\n");
    msg = b == std::string::npos ? "" : msg.substr(b, e - b + 1);
    if (msg == "session up" || msg == "session ended") {
      bool up = msg == "session up";
      if (st->active != up) {
        st->active = up;
        AA_INFO("aa: aarecvd: %s", msg.c_str());
      }
    } else if (msg == "pair on" || msg == "pair off") {
      // A 3 s hold on aaview's icon. It works during a session too, so a second phone can pair while one projects.
      if (!st->bt) {
        AA_INFO("aa: icon held: no Bluetooth (--no-bt), no pairing");
      } else if (msg == "pair on") {
        st->bt->startPairing("the icon was held");
      } else if (st->bt->pairing()) {
        st->bt->stopPairing("the icon was held again");
      }
    } else if (msg.rfind("icon ", 0) == 0 && !st->active) {
      AA_INFO("aa: icon tapped (state %s) with no phone session: %s", msg.substr(5, 4).c_str(),
              st->bt ? "connecting to the phone now" : "no Bluetooth (--no-bt)");
      if (st->bt) st->bt->connectNow();
    } else {
      AA_DBG("aa: from aarecvd: '%s'", msg.substr(0, 40).c_str());
    }
  }
  return 0;
}

}  // namespace

int runBtOnly(const Options &options, const std::string &notifyPath, bool noBluetooth) {
  // SIGTERM and SIGINT end the loop. sd-event wants them blocked first.
  sigset_t ss;
  sigemptyset(&ss);
  sigaddset(&ss, SIGTERM);
  sigaddset(&ss, SIGINT);
  sigprocmask(SIG_BLOCK, &ss, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  sd_event *event = nullptr;
  int r = sd_event_default(&event);
  if (r < 0) {
    AA_ERR("bt: event loop: %s", std::strerror(-r));
    return 1;
  }
  sd_event_add_signal(event, nullptr, SIGTERM, nullptr, nullptr);
  sd_event_add_signal(event, nullptr, SIGINT, nullptr, nullptr);

  State st;
  if (!noBluetooth) {
    try {
      st.bt = std::make_unique<Bluetooth>(event, options, st.active);
    } catch (const std::exception &ex) {
      AA_ERR("bt: %s", ex.what());
      sd_event_unref(event);
      return 1;
    }
  }

  ::unlink(notifyPath.c_str());
  auto slash = notifyPath.rfind('/');
  if (slash != std::string::npos && slash > 0) ::mkdir(notifyPath.substr(0, slash).c_str(), 0755);
  int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, notifyPath.c_str(), sizeof(addr.sun_path) - 1);
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
    AA_ERR("aa: %s: %s", notifyPath.c_str(), std::strerror(errno));
    return 1;
  }
  AA_INFO("aa: --bt-only: the session is aarecvd's; listening to it on %s", notifyPath.c_str());
  r = sd_event_add_io(event, nullptr, fd, EPOLLIN, onDatagram, &st);
  if (r < 0) {
    AA_ERR("aa: watching %s: %s", notifyPath.c_str(), std::strerror(-r));
    return 1;
  }
  r = sd_event_loop(event);
  AA_INFO("aa: stopping");
  // The profiles and the agent are dropped when bluetoothd sees our bus connection close.
  st.bt.reset();
  ::close(fd);
  ::unlink(notifyPath.c_str());
  sd_event_unref(event);
  return r < 0 ? 1 : 0;
}

}  // namespace aarecvd::bt
