/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "Transport.hpp"
#include "../Log.hpp"

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

namespace aarecvd::service {

namespace {

// "CN=..., valid 2016-07-07 to 2046-06-30": subject and dates only, with O=... when there is no common name.
std::string describeCert(X509 *x) {
  char cn[256] = "";
  std::string name;
  if (X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof cn) > 0)
    name = std::string("CN=") + cn;
  else if (X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_organizationName, cn, sizeof cn) > 0)
    name = std::string("O=") + cn;
  else
    name = "no name";
  auto day = [](const ASN1_TIME *t) {
    struct tm tmv {};
    if (!t || ASN1_TIME_to_tm(t, &tmv) != 1) return std::string("?");
    char b[48];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return std::string(b);
  };
  std::string out = name + ", valid " + day(X509_get0_notBefore(x)) + " to " +
                    day(X509_get0_notAfter(x));
  if (X509_cmp_current_time(X509_get0_notAfter(x)) < 0) out += " (EXPIRED)";
  return out;
}

bool readText(const std::string &path, std::string &out, std::string &why) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    why = "no " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  if (out.empty() || out.size() > 64 * 1024) {
    why = path + (out.empty() ? " is empty" : " is too big for a PEM file");
    return false;
  }
  return true;
}

X509 *parseCert(const std::string &pem) {
  BIO *b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  X509 *x = b ? PEM_read_bio_X509(b, nullptr, nullptr, nullptr) : nullptr;
  BIO_free(b);
  return x;
}

EVP_PKEY *parseKey(const std::string &pem) {
  BIO *b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr) : nullptr;
  BIO_free(b);
  return k;
}

std::once_flag g_certSaid;

}  // namespace

std::shared_ptr<const HeadUnitCert> loadHeadUnitCert(const std::string &certFile, const std::string &keyFile) {
  if (certFile.empty() || keyFile.empty()) {
    AA_INFO("aa: TLS: aasdk's built-in head-unit certificate (--cert / --key are '')");
    return nullptr;
  }
  auto cert = std::make_shared<HeadUnitCert>();
  std::string why;
  if (!readText(certFile, cert->certPem, why) || !readText(keyFile, cert->keyPem, why)) {
    AA_INFO("aa: TLS: aasdk's built-in head-unit certificate (%s)", why.c_str());
    return nullptr;
  }
  X509 *x = parseCert(cert->certPem);
  EVP_PKEY *k = parseKey(cert->keyPem);
  std::shared_ptr<const HeadUnitCert> out;
  if (!x) {
    AA_WARN("aa: TLS: %s is not a PEM certificate: using aasdk's built-in one", certFile.c_str());
  } else if (!k) {
    AA_WARN("aa: TLS: %s is not a PEM private key: using aasdk's built-in certificate", keyFile.c_str());
  } else if (X509_check_private_key(x, k) != 1) {
    AA_WARN("aa: TLS: %s and %s do not belong together: using aasdk's built-in certificate", certFile.c_str(),
            keyFile.c_str());
  } else {
    AA_INFO("aa: TLS: head-unit certificate from %s and %s: %s", certFile.c_str(), keyFile.c_str(),
            describeCert(x).c_str());
    out = cert;
  }
  X509_free(x);
  EVP_PKEY_free(k);
  return out;
}

X509 *TlsWrapper::readCertificate(const std::string &certificate) {
  X509 *x = aasdk::transport::SSLWrapper::readCertificate(cert_ ? cert_->certPem : certificate);
  if (x && !cert_)
    std::call_once(g_certSaid, [x]() { AA_INFO("aa: TLS: aasdk's built-in certificate: %s", describeCert(x).c_str()); });
  return x;
}

EVP_PKEY *TlsWrapper::readPrivateKey(const std::string &privateKey) {
  return aasdk::transport::SSLWrapper::readPrivateKey(cert_ ? cert_->keyPem : privateKey);
}

SSL_CTX *TlsWrapper::createContext(const SSL_METHOD *method) {
  SSL_CTX *ctx = aasdk::transport::SSLWrapper::createContext(method);
  if (ctx) {
    // TLS 1.2 only, security level 0, and only AES-GCM suites. aasdk's Cryptor::decrypt() takes every record to carry
    // 29 bytes of overhead (5 header + 8 nonce + 16 tag), which is true only of TLS 1.2 AES-GCM. A phone that chose
    // ChaCha20 or a CBC suite would fail mid-session.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(ctx,
                            "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES128-GCM-SHA256:"
                            "ECDHE-ECDSA-AES256-GCM-SHA384:AES128-GCM-SHA256:AES256-GCM-SHA384:@SECLEVEL=0");
  }
  return ctx;
}

SSL *TlsWrapper::createInstance(SSL_CTX *context) {
  ssl_ = aasdk::transport::SSLWrapper::createInstance(context);
  return ssl_;
}

std::string TlsWrapper::describe() const {
  if (!ssl_) return "?";
  const char *c = SSL_get_cipher_name(ssl_);
  return std::string(SSL_get_version(ssl_)) + " " + (c ? c : "?");
}

}  // namespace aarecvd::service
