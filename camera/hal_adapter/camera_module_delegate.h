/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef HAL_ADAPTER_CAMERA_MODULE_DELEGATE_H_
#define HAL_ADAPTER_CAMERA_MODULE_DELEGATE_H_

#include "hal_adapter/arc_camera3_mojo_utils.h"
#include "hal_adapter/mojo/arc_camera3.mojom.h"

namespace arc {

class CameraHalAdapter;

class CameraModuleDelegate final
    : public internal::MojoBinding<mojom::CameraModule> {
 public:
  CameraModuleDelegate(CameraHalAdapter* camera_hal_adapter,
                       scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                       base::Closure quit_cb);

  ~CameraModuleDelegate();

 private:
  void OpenDevice(int32_t device_id, const OpenDeviceCallback& callback) final;

  void GetNumberOfCameras(const GetNumberOfCamerasCallback& callback) final;

  void GetCameraInfo(int32_t device_id,
                     const GetCameraInfoCallback& callback) final;

  void SetCallbacks(mojom::CameraModuleCallbacksPtr callbacks,
                    const SetCallbacksCallback& callback) final;

  CameraHalAdapter* camera_hal_adapter_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(CameraModuleDelegate);
};

}  // namespace arc

#endif  // HAL_ADAPTER_CAMERA_MODULE_DELEGATE_H_