/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_ARC_FUTURE_H_
#define INCLUDE_ARC_FUTURE_H_

#include <set>
#include <utility>

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/memory/ref_counted.h>

#include "arc/common.h"
#include "arc/future_internal.h"

namespace internal {

class CancellationRelay {
 public:
  CancellationRelay();

  /* Upon destruction the CancellationRelay cancels all the FutureLocks still in
   * the observer set. */
  ~CancellationRelay();

  /* Registers a FutureLock to listen to cancel signal. */
  bool AddObserver(future_internal::FutureLock* future_lock);

  /* Removes a FutureLock from the observer set. */
  void RemoveObserver(future_internal::FutureLock* future_lock);

  /* Cancells all the futures currently in the observer set. */
  void CancelAllFutures();

 private:
  /* Used to serialize all member access. */
  base::Lock lock_;

  /* Stores all the FutureLock observers. */
  std::set<future_internal::FutureLock*> observers_;

  /* Used to indicate that a cancelled signal is already set. */
  bool cancelled_;
};

// Future templates and helper functions.

template <typename T>
class Future : public base::RefCountedThreadSafe<Future<T>> {
 public:
  static scoped_refptr<Future<T>> Create(CancellationRelay* relay) {
    return make_scoped_refptr(new Future<T>(relay));
  }

  /* Waits until the value to be ready and then return the value through
   * std::move(). */
  T Get() {
    VLOGF_ENTER();
    lock_.Wait(-1);  // Wait indefinitely until the value is set.
    return std::move(value_);
  }

  /* Sets the value and then wake up the waiter. */
  void Set(T&& value) {
    VLOGF_ENTER();
    value_ = std::move(value);
    lock_.Signal();
  }

  /* Default timeout is set to 5 seconds.  Setting the timeout to a value less
   * than or equal to 0 will wait indefinitely until the value is set.
   */
  bool Wait(int timeout_ms = 5000) {
    VLOGF_ENTER();
    return lock_.Wait(timeout_ms);
  }

 private:
  friend class base::RefCountedThreadSafe<Future<T>>;

  explicit Future(CancellationRelay* relay) : lock_(relay) {}

  ~Future() = default;

  future_internal::FutureLock lock_;

  T value_;

  DISALLOW_COPY_AND_ASSIGN(Future);
};

template <>
class Future<void> : public base::RefCountedThreadSafe<Future<void>> {
 public:
  static scoped_refptr<Future<void>> Create(CancellationRelay* relay) {
    return make_scoped_refptr(new Future<void>(relay));
  }

  /* Wakes up the waiter. */
  void Set() {
    VLOGF_ENTER();
    lock_.Signal();
  }

  /* Default timeout is set to 5 seconds.  Setting the timeout to a value less
   * than or equal to 0 will wait indefinitely until the value is set.
   */
  bool Wait(int timeout_ms = 5000) {
    VLOGF_ENTER();
    return lock_.Wait(timeout_ms);
  }

 private:
  friend class base::RefCountedThreadSafe<Future<void>>;

  explicit Future(CancellationRelay* relay) : lock_(relay) {}

  ~Future() = default;

  future_internal::FutureLock lock_;

  DISALLOW_COPY_AND_ASSIGN(Future);
};

template <typename T>
void FutureCallback(scoped_refptr<Future<T>> future, T ret) {
  future->Set(std::move(ret));
}

template <typename T>
base::Callback<void(T)> GetFutureCallback(
    const scoped_refptr<Future<T>>& future) {
  return base::Bind(&FutureCallback<T>,
                    base::RetainedRef(scoped_refptr<Future<T>>(future)));
}

base::Callback<void()> GetFutureCallback(
    const scoped_refptr<Future<void>>& future);

}  // namespace internal

#endif  // INCLUDE_ARC_FUTURE_H_