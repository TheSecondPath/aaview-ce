/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// Two small additions under aasdk's own interfaces, without changing aasdk itself:
//
// WatchedTCPEndpoint  aasdk's TCP transport has no read timeout (only its USB one has), and a phone that walks out
//                     of range never closes the socket, so a dead session can hang for good (once for 52 minutes).
//                     This wraps aasdk's TCPEndpoint and notes when bytes last arrived and how long a send has been
//                     blocked, for the session's 15 s timeout on reads and sends.
// TlsWrapper          aasdk's SSLWrapper limited to TLS 1.2 at security level 0, offering only AES-GCM suites
//                     (aasdk's decrypt assumes their record overhead). It also keeps the SSL object for the
//                     "TLS up" log line.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include <aasdk/TCP/ITCPEndpoint.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>

namespace aarecvd::service {

class Activity {
public:
  typedef std::shared_ptr<Activity> Pointer;
  Activity() { received(); }
  void received() { lastReceive_ = now(); }
  void sendStarted() { sendSince_ = now(); }
  void sendDone() { sendSince_ = 0; }
  double secondsSinceReceive() const { return (now() - lastReceive_) / 1e3; }
  double secondsSendBlocked() const {
    auto s = sendSince_.load();
    return s ? (now() - s) / 1e3 : 0;
  }

private:
  static int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::atomic<int64_t> lastReceive_{0}, sendSince_{0};
};

class WatchedTCPEndpoint : public aasdk::tcp::ITCPEndpoint {
public:
  WatchedTCPEndpoint(boost::asio::io_service &ioService, aasdk::tcp::ITCPEndpoint::Pointer inner, Activity::Pointer activity)
      : ioService_(ioService), inner_(std::move(inner)), activity_(std::move(activity)) {}

  void send(aasdk::common::DataConstBuffer buffer, Promise::Pointer promise) override {
    activity_->sendStarted();
    auto p = Promise::defer(ioService_);
    p->then(
        [activity = activity_, promise](size_t n) {
          activity->sendDone();
          promise->resolve(n);
        },
        [activity = activity_, promise](const aasdk::error::Error &e) {
          activity->sendDone();
          promise->reject(e);
        });
    inner_->send(buffer, std::move(p));
  }

  void receive(aasdk::common::DataBuffer buffer, Promise::Pointer promise) override {
    auto p = Promise::defer(ioService_);
    p->then(
        [activity = activity_, promise](size_t n) {
          activity->received();
          promise->resolve(n);
        },
        [promise](const aasdk::error::Error &e) { promise->reject(e); });
    inner_->receive(buffer, std::move(p));
  }

  void stop() override { inner_->stop(); }

private:
  boost::asio::io_service &ioService_;
  aasdk::tcp::ITCPEndpoint::Pointer inner_;
  Activity::Pointer activity_;
};

// The head unit's certificate and key from files (PEM text), read and checked once at start-up.
struct HeadUnitCert {
  std::string certPem, keyPem;
};

// --cert / --key: the pair from those files, or nullptr (meaning aasdk's built-in pair) with the reason logged. Only
// the certificate's subject and dates are ever logged, never the key or the PEM text.
std::shared_ptr<const HeadUnitCert> loadHeadUnitCert(const std::string &certFile, const std::string &keyFile);

class TlsWrapper : public aasdk::transport::SSLWrapper {
public:
  // cert is the pair from files, or nullptr for aasdk's built-in one. aasdk's Cryptor::init() passes its built-in PEM
  // text to readCertificate() and readPrivateKey(); with files, those parse the files' text instead.
  explicit TlsWrapper(std::shared_ptr<const HeadUnitCert> cert = nullptr) : cert_(std::move(cert)) {}
  X509 *readCertificate(const std::string &certificate) override;
  EVP_PKEY *readPrivateKey(const std::string &privateKey) override;
  SSL_CTX *createContext(const SSL_METHOD *method) override;
  SSL *createInstance(SSL_CTX *context) override;
  std::string describe() const;      // "TLSv1.2 ECDHE-RSA-AES128-GCM-SHA256"

private:
  std::shared_ptr<const HeadUnitCert> cert_;
  SSL *ssl_ = nullptr;
};

}  // namespace aarecvd::service
