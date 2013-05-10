// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHAPS_ISOLATE_H_
#define CHAPS_ISOLATE_H_

#include <string>

#include <chromeos/secure_blob.h>

#include "chaps/chaps.h"

namespace chaps {

const size_t kIsolateCredentialBytes = 16;

// Manages a user's isolate credentials, including saving and retrieval of
// isolate credentials. Sample usage:
//   IsolateCredentialManager isolate_manager;
//   SecureBlob isolate_credential;
//   isolate_manager.GetCurrentUserIsolateCredential(&isolate_credential);
//
// Only virtual to enable mocking in tests.
class IsolateCredentialManager {
 public:
  IsolateCredentialManager();
  virtual ~IsolateCredentialManager();

  // Get the well known credential for the default isolate.
  static chromeos::SecureBlob GetDefaultIsolateCredential() {
    // Default isolate credential is all zeros.
    return chromeos::SecureBlob(kIsolateCredentialBytes);
  }

  // Get the isolate credential for the current user, returning true if it
  // exists.
  virtual bool GetCurrentUserIsolateCredential(
      chromeos::SecureBlob* isolate_credential);

  // Get the isolate credential for the given user name, returning true if it
  // exists.
  virtual bool GetUserIsolateCredential(
      const std::string& user,
      chromeos::SecureBlob* isolate_credential);

  // Save the isolate credential such that it can be retrieved with
  // GetUserIsolateCredential. Return true on success and false on failure.
  virtual bool SaveIsolateCredential(
      const std::string& user,
      const chromeos::SecureBlob& isolate_credential);

 private:
  DISALLOW_COPY_AND_ASSIGN(IsolateCredentialManager);
};

}  // namespace chaps

#endif  // CHAPS_ISOLATE_H_

