// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBCHROMEOS_CHROMEOS_SYSLOG_LOGGING_H_
#define LIBCHROMEOS_CHROMEOS_SYSLOG_LOGGING_H_

#include <string>

namespace chromeos {

enum InitFlags {
  kLogToSyslog = 1,
  kLogToStderr = 2,
  kLogHeader = 4,
};

// Initialize logging subsystem.  |init_flags| is a bitfield, with bits defined
// in InitFlags above.
void InitLog(int init_flags);
// Gets the current logging flags.
int GetLogFlags();
// Sets the current logging flags.
void SetLogFlags(int log_flags);
// Convenience function for configuring syslog via openlog.  Users
// could call openlog directly except for naming collisions between
// base/logging.h and syslog.h.  Similarly users cannot pass the
// normal parameters so we pick a representative set.  |log_pid|
// causes pid to be logged with |ident|.
void OpenLog(const char* ident, bool log_pid);
// Start accumulating the logs to a string.  This is inefficient, so
// do not set to true if large numbers of log messages are coming.
// Accumulated logs are only ever cleared when the clear function ings
// called.
void LogToString(bool enabled);
// Get the accumulated logs as a string.
std::string GetLog();
// Clear the accumulated logs.
void ClearLog();
// Returns true if the accumulated log contains the given string.  Useful
// for testing.
bool FindLog(const char* string);

}  // namespace chromeos

#endif  // LIBCHROMEOS_CHROMEOS_SYSLOG_LOGGING_H_
