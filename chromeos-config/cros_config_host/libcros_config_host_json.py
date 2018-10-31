# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Crome OS Configuration access library.

Provides build-time access to the master configuration on the host. It is used
for reading from the master configuration. Consider using cros_config_host.py
for CLI access to this library.
"""

from __future__ import print_function

from collections import OrderedDict
import copy
import json

from cros_config_schema import TransformConfig
from libcros_config_host_base import BaseFile, CrosConfigBaseImpl, DeviceConfig
from libcros_config_host_base import FirmwareInfo, TouchFile

UNIBOARD_JSON_INSTALL_PATH = 'usr/share/chromeos-config/config.json'


class DeviceConfigJson(DeviceConfig):
  """JSON specific impl of DeviceConfig

  Properties:
    _config: Root dictionary element for a given config.
  """

  def __init__(self, config):
    self._config = config
    self.firmware_info = OrderedDict()

  def GetName(self):
    return str(self._config['name'])

  def GetProperties(self, path):
    result = self._config
    if path != '/':
      for path_token in path[1:].split('/'):  # Burn the first '/' char
        if path_token in result:
          result = result[path_token]
        else:
          return {}
    return result

  def GetProperty(self, path, name):
    props = self.GetProperties(path)
    if props and name in props:
      return str(props[name])
    return ''

  def GetValue(self, source, name):
    if name in source:
      val = source[name]
      if isinstance(val, basestring):
        return str(val)
      return source[name]
    return None

  def _GetFiles(self, path):
    result = []
    file_region = self.GetProperties(path)
    if file_region and 'files' in file_region:
      for item in file_region['files']:
        result.append(BaseFile(item['source'], item['destination']))
    return result

  def GetFirmwareConfig(self):
    firmware = self.GetProperties('/firmware')
    if not firmware or self.GetValue(firmware, 'no-firmware'):
      return {}
    return firmware

  def GetFirmwareInfo(self):
    return self.firmware_info

  def GetTouchFirmwareFiles(self):
    result = []
    touch = self.GetProperties('/touch')
    if touch and 'files' in touch:
      for item in touch['files']:
        result.append(
            TouchFile(item['source'], item['destination'], item['symlink']))

    return result

  def GetArcFiles(self):
    return self._GetFiles('/arc')

  def GetAudioFiles(self):
    return self._GetFiles('/audio/main')

  def GetThermalFiles(self):
    return self._GetFiles('/thermal')

  def GetWallpaperFiles(self):
    result = set()
    wallpaper = self.GetValue(self._config, 'wallpaper')
    if wallpaper:
      result.add(wallpaper)
    return result


class CrosConfigJson(CrosConfigBaseImpl):
  """JSON specific impl of CrosConfig

  Properties:
    _json: Root json for the entire config.
    _configs: List of DeviceConfigJson instances
  """

  def __init__(self, infile, model_filter_regex=None):
    """
    Args:
      model_filter_regex: Only returns configs that match the filter.
    """
    self._json = json.loads(
        TransformConfig(infile.read(), model_filter_regex=model_filter_regex))
    self._configs = []
    for config in self._json['chromeos']['configs']:
      self._configs.append(DeviceConfigJson(config))

    sorted(self._configs, key=lambda x: str(x.GetProperties('/identity')))

    # TODO(shapiroc): This is mess and needs considerable rework on the fw
    # side to cleanup, but for now, we're sticking with it in order to
    # finish migration to YAML.
    fw_by_model = {}
    processed = set()
    for config in self._configs:
      fw = config.GetFirmwareConfig()
      identity = str(config.GetProperties('/identity'))
      if fw and identity not in processed:
        fw_str = str(fw)
        shared_model = None
        if fw_str not in fw_by_model:
          # Use the explict name of the firmware, else use the device name
          # This supports equivalence testing with DT since it allowed
          # naming firmware images.
          fw_by_model[fw_str] = fw.get('name', config.GetName())

        shared_model = fw_by_model[fw_str]

        build_config = config.GetProperties('/firmware/build-targets')
        if build_config:
          bios_build_target = config.GetValue(build_config, 'coreboot')
          ec_build_target = config.GetValue(build_config, 'ec')
        else:
          bios_build_target, ec_build_target = None, None
        create_bios_rw_image = False

        main_image_uri = config.GetValue(fw, 'main-image') or ''
        main_rw_image_uri = config.GetValue(fw, 'main-rw-image') or ''
        ec_image_uri = config.GetValue(fw, 'ec-image') or ''
        pd_image_uri = config.GetValue(fw, 'pd-image') or ''
        extra = config.GetValue(fw, 'extra') or []
        tools = config.GetValue(fw, 'tools') or []

        fw_signer_config = config.GetProperties('/firmware-signing')
        key_id = config.GetValue(fw_signer_config, 'key-id')
        sig_in_customization_id = config.GetValue(fw_signer_config,
                                                  'sig-id-in-customization-id')

        have_image = True
        name = config.GetName()

        if sig_in_customization_id:
          sig_id = 'sig-id-in-customization-id'
        else:
          sig_id = config.GetValue(fw_signer_config, 'signature-id')
          processed.add(identity)

        info = FirmwareInfo(name, shared_model, key_id, have_image,
                            bios_build_target, ec_build_target, main_image_uri,
                            main_rw_image_uri, ec_image_uri, pd_image_uri,
                            extra, create_bios_rw_image, tools, sig_id)
        config.firmware_info[name] = info

        if sig_in_customization_id:
          for wl_config in self._configs:
            if wl_config.GetName() == name:
              wl_identity = str(wl_config.GetProperties('/identity'))
              processed.add(wl_identity)
              fw_signer_config = wl_config.GetProperties('/firmware-signing')
              wl_key_id = wl_config.GetValue(fw_signer_config, 'key-id')
              wl_sig_id = wl_config.GetValue(fw_signer_config, 'signature-id')
              wl_fw_info = copy.deepcopy(info)
              wl_config.firmware_info[wl_sig_id] = wl_fw_info._replace(
                  model=wl_sig_id,
                  key_id=wl_key_id,
                  have_image=False,
                  sig_id=wl_sig_id)

  def GetDeviceConfigs(self):
    return self._configs
