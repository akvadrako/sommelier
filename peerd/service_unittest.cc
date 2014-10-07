// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peerd/service.h"

#include <string>

#include <chromeos/errors/error.h>
#include <dbus/mock_bus.h>
#include <dbus/mock_exported_object.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "peerd/test_util.h"

using IpAddresses = peerd::Service::IpAddresses;
using ServiceInfo = peerd::Service::ServiceInfo;
using dbus::Bus;
using dbus::MockBus;
using dbus::MockExportedObject;
using dbus::ObjectPath;
using peerd::errors::service::kInvalidServiceId;
using peerd::errors::service::kInvalidServiceInfo;
using peerd::test_util::MakeMockCompletionAction;
using std::string;
using std::unique_ptr;
using testing::AnyNumber;
using testing::Invoke;
using testing::Return;
using testing::_;

namespace {

IpAddresses MakeValidIpAddresses() {
  // TODO(wiley) return a non-trivial list here.
  return {};
}

const char kServicePath[] = "/a/path";
const char kValidServiceId[] = "valid-id";

}  // namespace


namespace peerd {

class ServiceTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Ignore threading concerns.
    EXPECT_CALL(*bus_, AssertOnOriginThread()).Times(AnyNumber());
    EXPECT_CALL(*bus_, AssertOnDBusThread()).Times(AnyNumber());
    // Unless we expect to create a Service object, we won't.
    EXPECT_CALL(*bus_, GetExportedObject(_)).Times(0);
    // Just immediately call callbacks on ExportMethod calls.
    EXPECT_CALL(*service_object_, ExportMethod(_, _, _, _))
        .WillRepeatedly(Invoke(&test_util::HandleMethodExport));
    // Ignore Unregister calls.
    EXPECT_CALL(*service_object_, Unregister()).Times(AnyNumber());
  }

  void AssertMakeServiceFails(const string& service_id,
                              const IpAddresses& addresses,
                              const ServiceInfo& service_info,
                              const string& error_code) {
    chromeos::ErrorPtr error;
    EXPECT_FALSE(service_.RegisterAsync(
          &error, service_id, addresses, service_info,
          MakeMockCompletionAction()));
    ASSERT_NE(nullptr, error.get());
    EXPECT_TRUE(error->HasError(kPeerdErrorDomain, error_code));
  }

  void AssertMakeServiceSuccess(
      const string& service_id,
      const IpAddresses& addresses,
      const ServiceInfo& service_info) {
    chromeos::ErrorPtr error;
    EXPECT_CALL(*bus_, GetExportedObject(_))
        .WillOnce(Return(service_object_.get()));
    EXPECT_TRUE(service_.RegisterAsync(
          &error, service_id, addresses, service_info,
          MakeMockCompletionAction()));
    EXPECT_EQ(nullptr, error.get());
  }

  scoped_refptr<MockBus> bus_{new MockBus{Bus::Options{}}};
  scoped_refptr<dbus::MockExportedObject> service_object_{
      new MockExportedObject{bus_.get(), ObjectPath{kServicePath}}};
  Service service_{bus_, nullptr, ObjectPath{kServicePath}};
};

TEST_F(ServiceTest, ShouldRejectZeroLengthServiceId) {
  AssertMakeServiceFails("",
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectLongServiceId) {
  AssertMakeServiceFails(string(Service::kMaxServiceIdLength + 1, 'a'),
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectInvalidCharInServiceId) {
  AssertMakeServiceFails("not*allowed",
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectHyphenPrefix) {
  AssertMakeServiceFails("-not-allowed",
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectHyphenSuffix) {
  AssertMakeServiceFails("not-allowed-",
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectAdjacentHyphens) {
  AssertMakeServiceFails("not--allowed",
                         MakeValidIpAddresses(),
                         ServiceInfo(),
                         kInvalidServiceId);
}

TEST_F(ServiceTest, ShouldRejectInvalidCharInServiceInfoKey) {
  const ServiceInfo info = {{"spaces are illegal", "valid value"}};
  AssertMakeServiceFails(kValidServiceId,
                         MakeValidIpAddresses(),
                         info,
                         kInvalidServiceInfo);
}

TEST_F(ServiceTest, ShouldRejectServiceInfoPairTooLong) {
  const ServiceInfo info = {
      {"k", string(Service::kMaxServiceInfoPairLength, 'v')},
  };
  AssertMakeServiceFails(kValidServiceId,
                         MakeValidIpAddresses(),
                         info,
                         kInvalidServiceInfo);
}

TEST_F(ServiceTest, RegisterWhenInputIsValid) {
  AssertMakeServiceSuccess(kValidServiceId,
                           MakeValidIpAddresses(),
                           ServiceInfo());
}

TEST_F(ServiceTest, RegisterWhenInputIsValidBoundaryCases) {
  const ServiceInfo service_info = {
      {"a", string(Service::kMaxServiceInfoPairLength - 1, 'b')},
      {"", ""},
      {"b", ""},
  };
  AssertMakeServiceSuccess(
      string(Service::kMaxServiceIdLength, 'a'),
      MakeValidIpAddresses(),
      service_info);
}

}  // namespace peerd
