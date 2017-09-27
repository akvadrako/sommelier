// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <memory>
#include <string>
#include <utility>

#include <base/json/json_writer.h>
#include <base/values.h>
#include <chromeos/dbus/service_constants.h>

#include "debugd/src/helpers/shill_proxy.h"

namespace debugd {
namespace {

std::unique_ptr<base::Value> CollectNetworkStatus() {
  auto result = std::make_unique<base::DictionaryValue>();

  auto proxy = ShillProxy::Create();
  if (!proxy)
    return std::move(result);

  // Gets the manager properties from which we can identify the list of device
  // and service object paths.
  auto manager_properties =
      proxy->GetProperties(shill::kFlimflamManagerInterface,
                           dbus::ObjectPath(shill::kFlimflamServicePath));
  if (!manager_properties)
    return std::move(result);

  // Gets the device properties of all listed devices.
  auto device_paths =
      proxy->GetObjectPaths(*manager_properties, shill::kDevicesProperty);
  auto devices = proxy->BuildObjectPropertiesMap(
      shill::kFlimflamDeviceInterface, device_paths);

  // If a list of IP config object paths is found in the properties of a
  // device, expands the IP config object paths into IP config properties.
  for (const auto& device_path : device_paths) {
    base::DictionaryValue* device_properties = nullptr;
    CHECK(devices->GetDictionary(device_path.value(), &device_properties));
    auto ipconfig_paths =
        proxy->GetObjectPaths(*device_properties, shill::kIPConfigsProperty);
    auto ipconfigs = proxy->BuildObjectPropertiesMap(
        shill::kFlimflamIPConfigInterface, ipconfig_paths);
    device_properties->SetWithoutPathExpansion(shill::kIPConfigsProperty,
                                               std::move(ipconfigs));
  }

  // Gets the device properties of all listed services.
  auto service_paths =
      proxy->GetObjectPaths(*manager_properties, shill::kServicesProperty);
  auto services = proxy->BuildObjectPropertiesMap(
      shill::kFlimflamServiceInterface, service_paths);

  result->SetWithoutPathExpansion("devices", std::move(devices));
  result->SetWithoutPathExpansion("services", std::move(services));

  return std::move(result);
}

}  // namespace
}  // namespace debugd

int main() {
  auto result = debugd::CollectNetworkStatus();
  std::string json;
  base::JSONWriter::WriteWithOptions(
      *result, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
  printf("%s\n", json.c_str());
  return 0;
}
