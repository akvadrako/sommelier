// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAMERA3_TEST_CAMERA3_DEVICE_IMPL_H_
#define CAMERA3_TEST_CAMERA3_DEVICE_IMPL_H_

#include <semaphore.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/threading/thread_checker.h>

#include "camera3_test/camera3_device_fixture.h"

namespace camera3_test {

const uint32_t kInitialFrameNumber = 0;

// This class is thread-safe except the Flush function, which must be called
// after the Initialzie function returns successfully.
class Camera3DeviceImpl : protected camera3_callback_ops {
 public:
  explicit Camera3DeviceImpl(int cam_id);

  int Initialize(Camera3Module* cam_module);

  void RegisterProcessCaptureResultCallback(
      Camera3Device::ProcessCaptureResultCallback cb);

  void RegisterNotifyCallback(Camera3Device::NotifyCallback cb);

  void RegisterResultMetadataOutputBufferCallback(
      Camera3Device::ProcessResultMetadataOutputBuffersCallback cb);

  void RegisterPartialMetadataCallback(
      Camera3Device::ProcessPartialMetadataCallback cb);

  bool IsTemplateSupported(int32_t type);

  void AddOutputStream(int format, int width, int height);

  const camera_metadata_t* ConstructDefaultRequestSettings(int type);

  int ConfigureStreams(std::vector<const camera3_stream_t*>* streams);

  int AllocateOutputStreamBuffers(
      std::vector<camera3_stream_buffer_t>* output_buffers);

  int AllocateOutputBuffersByStreams(
      const std::vector<const camera3_stream_t*>& streams,
      std::vector<camera3_stream_buffer_t>* output_buffers);

  int RegisterOutputBuffer(const camera3_stream_t& stream,
                           BufferHandleUniquePtr unique_buffer);

  int ProcessCaptureRequest(camera3_capture_request_t* request);

  int WaitShutter(const struct timespec& timeout);

  int WaitCaptureResult(const struct timespec& timeout);

  int Flush();

  void Destroy();

  const Camera3Device::StaticInfo* GetStaticInfo() const;

 private:
  void InitializeOnThread(Camera3Module* cam_module, int* result);

  void RegisterProcessCaptureResultCallbackOnThread(
      Camera3Device::ProcessCaptureResultCallback cb);

  void RegisterNotifyCallbackOnThread(Camera3Device::NotifyCallback cb);

  void RegisterResultMetadataOutputBufferCallbackOnThread(
      Camera3Device::ProcessResultMetadataOutputBuffersCallback cb);

  void RegisterPartialMetadataCallbackOnThread(
      Camera3Device::ProcessPartialMetadataCallback cb);

  void IsTemplateSupportedOnThread(int32_t type, bool* result);

  void AddOutputStreamOnThread(int format, int width, int height);

  void ConstructDefaultRequestSettingsOnThread(
      int type,
      const camera_metadata_t** result);

  void ConfigureStreamsOnThread(std::vector<const camera3_stream_t*>* streams,
                                int* result);

  void AllocateOutputStreamBuffersOnThread(
      std::vector<camera3_stream_buffer_t>* output_buffers,
      int32_t* result);

  void AllocateOutputBuffersByStreamsOnThread(
      const std::vector<const camera3_stream_t*>* streams,
      std::vector<camera3_stream_buffer_t>* output_buffers,
      int32_t* result);

  void RegisterOutputBufferOnThread(const camera3_stream_t* stream,
                                    BufferHandleUniquePtr unique_buffer,
                                    int32_t* result);

  void ProcessCaptureRequestOnThread(camera3_capture_request_t* request,
                                     int* result);

  void DestroyOnThread(int* result);

  // Static callback forwarding methods from HAL to instance
  static void ProcessCaptureResultForwarder(
      const camera3_callback_ops* cb,
      const camera3_capture_result* result);

  // Static callback forwarding methods from HAL to instance
  static void NotifyForwarder(const camera3_callback_ops* cb,
                              const camera3_notify_msg* msg);

  // Callback functions from HAL device
  void ProcessCaptureResult(const camera3_capture_result* result);

  void ProcessCaptureResultOnThread(const camera3_capture_result* result);

  // Callback functions from HAL device
  void Notify(const camera3_notify_msg* msg);

  void NotifyOnThread(const camera3_notify_msg* msg);

  // Get the buffers out of the given stream buffers |output_buffers|. The
  // buffers are return in the container |unique_buffers|, and the caller of
  // the function is expected to take the buffer ownership.
  int GetOutputStreamBufferHandles(
      const std::vector<camera3_stream_buffer_t>& output_buffers,
      std::vector<BufferHandleUniquePtr>* unique_buffers);

  // Whether or not partial result is used
  bool UsePartialResult() const;

  // Process and handle partial result of one callback.
  void ProcessPartialResult(const camera3_capture_result& result);

  const std::string GetThreadName(int cam_id);

  const int cam_id_;

  // This thread is needed because of the ARC++ HAL assumption that all the
  // camera3_device_ops functions, except dump, should be called on the same
  // thread. Each device is accessed through a different thread.
  Camera3TestThread hal_thread_;

  base::ThreadChecker thread_checker_;

  bool initialized_;

  camera3_device* cam_device_;

  std::unique_ptr<Camera3Device::StaticInfo> static_info_;

  // Two bins of streams for swapping while configuring new streams
  std::vector<camera3_stream_t> cam_stream_[2];

  // Index of active streams
  int cam_stream_idx_;

  Camera3TestGralloc* gralloc_;

  // Store allocated buffers with streams as the key
  std::unordered_map<const camera3_stream_t*,
                     std::vector<BufferHandleUniquePtr>>
      stream_buffer_map_;

  uint32_t request_frame_number_;

  uint32_t result_frame_number_;

  // Store created capture requests with frame number as the key
  std::unordered_map<uint32_t, camera3_capture_request_t> capture_request_map_;

  // Store the frame numbers of capture requests that HAL has finished
  // processing
  std::set<uint32_t> completed_request_set_;

  class CaptureResultInfo {
   public:
    CaptureResultInfo();

    // Allocate and copy into partial metadata
    void AllocateAndCopyMetadata(const camera_metadata_t& src);

    // Determine whether or not the key is available
    bool IsMetadataKeyAvailable(int32_t key) const;

    // Find and get key value from partial metadata
    int32_t GetMetadataKeyValue(int32_t key) const;

    // Find and get key value in int64_t from partial metadata
    int64_t GetMetadataKeyValue64(int32_t key) const;

    // Merge partial metadata into one.
    CameraMetadataUniquePtr MergePartialMetadata();

    uint32_t num_output_buffers_;

    bool have_result_metadata_;

    std::vector<CameraMetadataUniquePtr> partial_metadata_;

    std::vector<camera3_stream_buffer_t> output_buffers_;

   private:
    bool GetMetadataKeyEntry(int32_t key,
                             camera_metadata_ro_entry_t* entry) const;

    base::ThreadChecker thread_checker_;
  };

  // Store capture result information with frame number as the key
  std::unordered_map<uint32_t, CaptureResultInfo> capture_result_info_map_;

  sem_t shutter_sem_;

  sem_t capture_result_sem_;

  Camera3Device::ProcessCaptureResultCallback process_capture_result_cb_;

  Camera3Device::NotifyCallback notify_cb_;

  Camera3Device::ProcessResultMetadataOutputBuffersCallback
      process_result_metadata_output_buffers_cb_;

  Camera3Device::ProcessPartialMetadataCallback process_partial_metadata_cb_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Camera3DeviceImpl);
};

}  // namespace camera3_test

#endif  // CAMERA3_TEST_CAMERA3_DEVICE_IMPL_H_