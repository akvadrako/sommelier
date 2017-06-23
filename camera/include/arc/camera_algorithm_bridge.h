/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_ARC_CAMERA_ALGORITHM_BRIDGE_H_
#define INCLUDE_ARC_CAMERA_ALGORITHM_BRIDGE_H_

#include <cinttypes>

#include "arc/camera_algorithm.h"

namespace arc {

// The class is for a camera HAL to access camera algorithm library.
//
// The class is thread-safe.
//
// Example usage:
//
//  #include <arc/camera_algorithm_bridge.h>
//  CameraAlgorithmBridge* algo = CameraAlgorithmBridge::GetInstance();
//  algo->Initialize(this);
//  std::vector<int32_t> handles[2];
//  handles[0] = algo->RegisterBuffer(buffer_fd0);
//  handles[1] = algo->RegisterBuffer(buffer_fd1);
//  std::vector<uint8_t> req_headers[2];
//  algo->Request(req_headers[0], handles[0]);
//  algo->Request(req_headers[1], handles[1]);
//  /* |return_callback_fn| is called to return buffer */
//  algo->Deregister(handles);

class CameraAlgorithmBridge {
 public:
  // This method returns the CameraAlgorithmBridge singleton.
  //
  // Returns:
  //    Pointer to singleton on success; nullptr on failure.
  static CameraAlgorithmBridge* GetInstance();

  // This method is one-time initialization that registers a callback function
  // for the camera algorithm library to return a buffer handle. It must be
  // called before any other functions.
  //
  // Args:
  //    |callback_ops|: Pointer to callback functions. The callback functions
  //      will be called on a different thread from the one calling
  //      Initialize, and the caller needs to take care of the
  //      synchronization.
  //
  // Returns:
  //    0 on success; corresponding error code on failure.
  virtual int32_t Initialize(
      const camera_algorithm_callback_ops_t* callback_ops) = 0;

  // This method registers a buffer to the camera algorithm library and gets
  // the handle associated with it.
  //
  // Args:
  //    |buffer_fd|: The buffer file descriptor to register.
  //
  // Returns:
  //    A handle on success; corresponding error code on failure.
  virtual int32_t RegisterBuffer(int buffer_fd) = 0;

  // This method posts a request for the camera algorithm library to process the
  // given buffer.
  //
  // Args:
  //    |req_header|: The request header indicating request details. The
  //      interpretation depends on the HAL implementation.
  //    |buffer_handle|: Handle of the buffer to process.
  //
  // Returns:
  //    0 on success; corresponding error code on failure.
  virtual int32_t Request(const std::vector<uint8_t>& req_header,
                          int32_t buffer_handle) = 0;

  // This method deregisters buffers to the camera algorithm library. The camera
  // algorithm shall release all the registered buffers on return of this
  // function.
  //
  // Args:
  //    |buffer_handles|: The buffer handles to deregister.
  //
  // Returns:
  //    A handle on success; -1 on failure.
  virtual void DeregisterBuffers(
      const std::vector<int32_t>& buffer_handles) = 0;

 protected:
  CameraAlgorithmBridge() {}

 private:
  // Disallow copy and assign
  CameraAlgorithmBridge(const CameraAlgorithmBridge&) = delete;
  void operator=(const CameraAlgorithmBridge&) = delete;
};

}  // namespace arc

#endif  // INCLUDE_ARC_CAMERA_ALGORITHM_BRIDGE_H_