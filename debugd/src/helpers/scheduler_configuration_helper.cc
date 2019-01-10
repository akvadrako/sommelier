// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debugd/src/helpers/scheduler_configuration_utils.h"

#include <libminijail.h>
#include <scoped_minijail.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/logging.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>
#include <build/build_config.h>
#include <build/buildflag.h>
#include <chromeos/dbus/service_constants.h>

using debugd::scheduler_configuration::kConservativeScheduler;
using debugd::scheduler_configuration::kPerformanceScheduler;

namespace {

constexpr char kCPUPathPrefix[] = "/sys";
constexpr char kSeccompFilterPath[] =
    "/usr/share/policy/scheduler-configuration-helper.policy";
constexpr char kDebugdUser[] = "debugd";
constexpr char kDebugdGroup[] = "debugd";

// Enters a minijail sandbox.
void EnterSandbox() {
  ScopedMinijail jail(minijail_new());
  minijail_no_new_privs(jail.get());
  minijail_use_seccomp_filter(jail.get());
  minijail_parse_seccomp_filters(jail.get(), kSeccompFilterPath);
  minijail_reset_signal_mask(jail.get());
  minijail_namespace_ipc(jail.get());
  minijail_namespace_net(jail.get());
  minijail_remount_proc_readonly(jail.get());
  minijail_change_user(jail.get(), kDebugdUser);
  minijail_change_group(jail.get(), kDebugdGroup);
  minijail_namespace_vfs(jail.get());
  minijail_bind(jail.get(), "/", "/", 0);
  minijail_bind(jail.get(), "/proc", "/proc", 0);
  minijail_mount_dev(jail.get());
  minijail_remount_proc_readonly(jail.get());
  minijail_enter_pivot_root(jail.get(), "/var/empty");
  minijail_bind(jail.get(), "/sys", "/sys", 1);
  minijail_enter(jail.get());
}

}  // namespace

int main(int argc, char* argv[]) {
  brillo::InitLog(brillo::kLogToStderr);

  std::string policy_flag = std::string("Set to either ") +
                            kConservativeScheduler + " or " +
                            kPerformanceScheduler + ".";
  DEFINE_string(policy, "", policy_flag.c_str());
  brillo::FlagHelper::Init(argc, argv, "scheduler_configuration_helper");

  if (FLAGS_policy != kConservativeScheduler &&
      FLAGS_policy != kPerformanceScheduler) {
    LOG(ERROR) << "Usage: " << argv[0] << " --policy=["
               << kConservativeScheduler << "|" << kPerformanceScheduler << "]";
    return 1;
  }

  // The CPU control files must be opened as root.
  base::FilePath base_path(kCPUPathPrefix);
  debugd::SchedulerConfigurationUtils utils(base_path);
  if (!utils.GetControlFDs()) {
    LOG(ERROR) << "Failed to open CPU control files.";
    return 1;
  }

  EnterSandbox();

  int status = 1;
  if (FLAGS_policy == kPerformanceScheduler) {
    status = utils.EnablePerformanceConfiguration() ? 0 : 1;
  } else if (FLAGS_policy == kConservativeScheduler) {
    status = utils.EnableConservativeConfiguration() ? 0 : 1;
  }

  return status;
}