// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A simple daemon to detect and access PTP/MTP devices.

#include <glib.h>
#include <glib-object.h>
#include <glib-unix.h>

#include <base/at_exit.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/macros.h>
#include <base/strings/string_number_conversions.h>
#include <brillo/syslog_logging.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus-c++/glib-integration.h>

#include "daemon.h"


using base::CommandLine;
using mtpd::Daemon;

// Messages logged at a level lower than this don't get logged anywhere.
static const char kMinLogLevelSwitch[] = "minloglevel";

void SetupLogging() {
  brillo::InitLog(brillo::kLogToSyslog);

  std::string log_level_str =
      CommandLine::ForCurrentProcess()->GetSwitchValueASCII(kMinLogLevelSwitch);
  int log_level = 0;
  if (base::StringToInt(log_level_str, &log_level) && log_level >= 0)
    logging::SetMinLogLevel(log_level);
}

// This callback will be invoked once there is a new device event that
// should be processed by the Daemon::ProcessDeviceEvents().
gboolean DeviceEventCallback(GIOChannel* /* source */,
                             GIOCondition /* condition */,
                             gpointer data) {
  Daemon* daemon = reinterpret_cast<Daemon*>(data);
  daemon->ProcessDeviceEvents();
  // This function should always return true so that the main loop
  // continues to select on device event file descriptor.
  return true;
}

// This callback will be inovked when this process receives SIGINT or SIGTERM.
gboolean TerminationSignalCallback(gpointer data) {
  LOG(INFO) << "Received a signal to terminate the daemon";
  GMainLoop* loop = reinterpret_cast<GMainLoop*>(data);
  g_main_loop_quit(loop);

  // This function can return false to remove this signal handler as we are
  // quitting the main loop anyway.
  return false;
}

int main(int argc, char** argv) {
  // Needed by base::RandBytes() and other Chromium bits that expects
  // an AtExitManager to exist.
  base::AtExitManager exit_manager;

  CommandLine::Init(argc, argv);
  SetupLogging();

  LOG(INFO) << "Creating a GMainLoop";
  GMainLoop* loop = g_main_loop_new(g_main_context_default(), FALSE);
  CHECK(loop) << "Failed to create a GMainLoop";

  // Set up a signal handler for handling SIGINT and SIGTERM.
  g_unix_signal_add(SIGINT, TerminationSignalCallback, loop);
  g_unix_signal_add(SIGTERM, TerminationSignalCallback, loop);

  LOG(INFO) << "Creating the D-Bus dispatcher";
  DBus::Glib::BusDispatcher dispatcher;
  DBus::default_dispatcher = &dispatcher;
  dispatcher.attach(nullptr);

  LOG(INFO) << "Creating the mtpd server";
  DBus::Connection server_conn = DBus::Connection::SystemBus();
  CHECK(server_conn.acquire_name(mtpd::kMtpdServiceName));
  Daemon daemon(&server_conn);

  // Set up a monitor for handling device events.
  g_io_add_watch_full(g_io_channel_unix_new(daemon.GetDeviceEventDescriptor()),
                      G_PRIORITY_HIGH_IDLE,
                      GIOCondition(G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_NVAL),
                      DeviceEventCallback, &daemon, nullptr);

  g_main_loop_run(loop);

  LOG(INFO) << "Cleaning up and exiting";
  g_main_loop_unref(loop);

  return 0;
}
