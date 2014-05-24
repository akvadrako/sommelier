// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/util.h"

#include <pwd.h>
#include <unistd.h>

#include <base/file_util.h>
#include <base/files/file_path.h>
#include <base/logging.h>

namespace util {

base::FilePath GetReparentedPath(const std::string& path,
                                 const base::FilePath& parent) {
  if (parent.empty())
    return base::FilePath(path);

  CHECK(!path.empty() && path[0] == '/');
  base::FilePath relative_path(path.substr(1));
  CHECK(!relative_path.IsAbsolute());
  return parent.Append(relative_path);
}

bool SetPermissions(const base::FilePath& path,
                    uid_t uid,
                    gid_t gid,
                    mode_t mode) {
  if (getuid() == 0) {
    if (chown(path.value().c_str(), uid, gid) != 0) {
      PLOG(ERROR) << "Couldn't chown " << path.value() << " to "
                  << uid << ":" << gid;
      return false;
    }
  }
  if (chmod(path.value().c_str(), mode) != 0) {
    PLOG(ERROR) << "Unable to chmod " << path.value() << " to "
                << std::oct << mode;
    return false;
  }
  return true;
}

bool EnsureDirectoryExists(const base::FilePath& path,
                           uid_t uid,
                           gid_t gid,
                           mode_t mode) {
  if (!base::CreateDirectory(path)) {
    PLOG(ERROR) << "Unable to create " << path.value();
    return false;
  }
  return SetPermissions(path, uid, gid, mode);
}

bool GetUserInfo(const std::string& user, uid_t* uid, gid_t* gid) {
  const struct passwd* user_info = getpwnam(user.c_str());
  endpwent();
  if (!user_info) {
    PLOG(ERROR) << "Unable to find user " << user;
    return false;
  }
  if (uid)
    *uid = user_info->pw_uid;
  if (gid)
    *gid = user_info->pw_gid;
  return true;
}

}  // namespace util
