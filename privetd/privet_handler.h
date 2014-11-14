// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PRIVETD_PRIVET_HANDLER_H_
#define PRIVETD_PRIVET_HANDLER_H_

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <base/callback_forward.h>

namespace base {
class Value;
class DictionaryValue;
}  // namespace base

namespace privetd {

class CloudDelegate;
class DeviceDelegate;
class SecurityDelegate;
class WifiDelegate;

enum class AuthScope;

// Privet V3 HTTP/HTTPS requests handler.
// API details at https://developers.google.com/cloud-devices/
class PrivetHandler {
 public:
  // Callback to handle requests asynchronously.
  // |status| is HTTP status code.
  // |output| is result returned in HTTP response. Contains result of
  // successfully request of information about error.
  using RequestCallback =
      base::Callback<void(int status, const base::DictionaryValue& output)>;

  PrivetHandler(CloudDelegate* cloud,
                DeviceDelegate* device,
                SecurityDelegate* pairing,
                WifiDelegate* wifi);
  ~PrivetHandler();

  // Handles HTTP/HTTPS Privet request.
  // |api| is the path from the HTTP request, e.g /privet/info.
  // |auth_header| is the Authentication header from HTTP request.
  // |input| is the the POST data from HTTP request. If nullptr, data format is
  // not valid JSON.
  // |callback| will be called exactly once during or after |HandleRequest|
  // call.
  void HandleRequest(const std::string& api,
                     const std::string& auth_header,
                     const base::DictionaryValue* input,
                     const RequestCallback& callback);

 private:
  using ApiHandler = base::Callback<void(const base::DictionaryValue&,
                                         const RequestCallback&)>;

  void HandleInfo(const base::DictionaryValue&,
                  const RequestCallback& callback);
  void HandleAuth(const base::DictionaryValue& input,
                  const RequestCallback& callback);
  void HandleSetupStart(const base::DictionaryValue& input,
                        const RequestCallback& callback);
  void HandleSetupStatus(const base::DictionaryValue& input,
                         const RequestCallback& callback);

  std::unique_ptr<base::DictionaryValue> CreateEndpointsSection() const;
  std::unique_ptr<base::DictionaryValue> CreateInfoAuthSection() const;
  std::unique_ptr<base::DictionaryValue> CreateWifiSection() const;
  std::unique_ptr<base::DictionaryValue> CreateGcdSection() const;

  CloudDelegate* cloud_ = nullptr;
  DeviceDelegate* device_ = nullptr;
  SecurityDelegate* security_ = nullptr;
  WifiDelegate* wifi_ = nullptr;

  std::map<std::string, std::pair<AuthScope, ApiHandler>> handlers_;
};

}  // namespace privetd

#endif  // PRIVETD_PRIVET_HANDLER_H_
