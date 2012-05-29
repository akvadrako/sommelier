// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WIMAX_MANAGER_GDM_DEVICE_H_
#define WIMAX_MANAGER_GDM_DEVICE_H_

extern "C" {
#include <gct/gctapi.h>
}  // extern "C"

#include <glib.h>

#include <string>

#include <base/basictypes.h>
#include <base/memory/weak_ptr.h>
#include <gtest/gtest_prod.h>

#include "wimax_manager/device.h"

namespace wimax_manager {

class GdmDriver;

class GdmDevice : public Device {
 public:
  GdmDevice(uint8 index, const std::string &name,
            const base::WeakPtr<GdmDriver> &driver);
  virtual ~GdmDevice();

  virtual bool Enable();
  virtual bool Disable();
  virtual bool ScanNetworks();
  virtual bool UpdateStatus();
  virtual bool Connect(const Network &network,
                       const base::DictionaryValue &parameters);
  virtual bool Disconnect();

 private:
  friend class GdmDriver;
  FRIEND_TEST(GdmDeviceTest, ConstructEAPParameters);
  FRIEND_TEST(GdmDeviceTest, ConstructEAPParametersWithInvalidEAPParameters);
  FRIEND_TEST(GdmDeviceTest, ConstructEAPParametersWithoutAnonymousIdentity);
  FRIEND_TEST(GdmDeviceTest, ConstructEAPParametersWithoutEAPParameters);

  bool Open();
  bool Close();

  static bool ConstructEAPParameters(
      const base::DictionaryValue &connect_parameters,
      GCT_API_EAP_PARAM *eap_parameters);

  void set_mac_address(const uint8 mac_address[6]);
  void set_connection_progress(
      WIMAX_API_CONNECTION_PROGRESS_INFO connection_progress) {
    connection_progress_ = connection_progress;
  }

  base::WeakPtr<GdmDriver> driver_;
  bool open_;
  WIMAX_API_CONNECTION_PROGRESS_INFO connection_progress_;
  uint8 mac_address_[6];
  guint scan_timeout_id_;
  guint status_update_timeout_id_;

  DISALLOW_COPY_AND_ASSIGN(GdmDevice);
};

}  // namespace wimax_manager

#endif  // WIMAX_MANAGER_GDM_DEVICE_H_
