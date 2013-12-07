// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_COMMON_DBUS_SENDER_H_
#define POWER_MANAGER_COMMON_DBUS_SENDER_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"

namespace google {
namespace protobuf {
class MessageLite;
}  // namespace protobuf
}  // namespace google

namespace power_manager {

// Interface for sending D-Bus messages.  A stub implementation can be
// instantiated by tests to verify behavior without actually communicating with
// D-Bus.
class DBusSenderInterface {
 public:
  virtual ~DBusSenderInterface() {}

  // Emits a signal named |signal_name| without any arguments.
  virtual void EmitBareSignal(const std::string& signal_name) = 0;

  // Emits a signal named |signal_name| and containing a serialized copy of
  // |protobuf| as a single byte array argument.
  virtual void EmitSignalWithProtocolBuffer(
      const std::string& signal_name,
      const google::protobuf::MessageLite& protobuf) = 0;
};

// DBusSenderInterface implementation that actually sends messages to D-Bus.
class DBusSender : public DBusSenderInterface {
 public:
  DBusSender();
  virtual ~DBusSender();

  // |path| and |interface| are used when sending signals.
  void Init(const std::string& path, const std::string& interface);

  // DBusSenderInterface override:
  virtual void EmitBareSignal(const std::string& signal_name) OVERRIDE;
  virtual void EmitSignalWithProtocolBuffer(
      const std::string& signal_name,
      const google::protobuf::MessageLite& protobuf) OVERRIDE;

 private:
  // Emits |signal_name|, serializing |protobuf| and passing it as a byte
  // array argument if non-NULL.
  void EmitSignalInternal(const std::string& signal_name,
                          const google::protobuf::MessageLite* protobuf);

  std::string path_;
  std::string interface_;

  DISALLOW_COPY_AND_ASSIGN(DBusSender);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_COMMON_DBUS_SENDER_H_
