# The Chromium OS Platform

This repo holds (most) of the custom code that makes up the Chromium OS
platform.  That largely covers daemons, programs, and libraries that were
written specifically for Chromium OS.

We moved from multiple separate repos in platform/ to a single repo in
platform2/ for a number of reasons:

* Make it easier to work across multiple projects simultaneously
* Increase code re-use (via common libs) rather than duplicate utility
  functions multiple items over
* Share the same build system

While most projects were merged, not all of them were.  Some projects were
standalone already (such as vboot), or never got around to being folded in
(such as imageloader).  Some day those extra projects might get merged in.

Similarly, some projects that were merged in, were then merged back out.
This was due to the evolution of the Brillo project and collaboration with
Android.  That means the AOSP repos are the upstream and Chromium OS carries
copies.

# Local Project Directory

| Project | Description |
|---------|-------------|
| [arc](./arc/) | Tools/deamons/init-scripts to run ARC |
| [attestation](./attestation/) | Daemon and client for managing remote attestation |
| [authpolicy](./authpolicy/) | Daemon for integrating with Microsoft Active Directory (AD) domains |
| [avtest_label_detect](./avtest_label_detect/) | Test tool for OCRing device labels |
| [biod](./biod/) | Biometrics daemon |
| [bluetooth](./bluetooth/) | Bluetooth Service and tools |
| [bootstat](./bootstat/) | Tools for tracking points in the overall boot process (for metrics) |
| [buffet](./buffet/) | Daemon for reacting to cloud messages |
| [cecservice](./cecservice/) | Service for switching CEC enabled TVs on and off |
| [chaps](./chaps/) | PKCS #11 implementation for TPM 1 devices |
| [chromeos-common-script](./chromeos-common-script/) | Shared scripts for partitions and basic disk information |
| [chromeos-config](./chromeos-config/) | CrOS unified build runtime config manager |
| [chromeos-dbus-bindings](./chromeos-dbus-bindings/) | Simplifies the implementation of D-Bus daemons and proxies |
| [common-mk](./common-mk/) | Common build & test logic for platform2 projects |
| [container_utils](./container_utils/) | Support tools for containers (e.g. device-jail) |
| [crash-reporter](./crash-reporter/) | The system crash handler & reporter |
| [cromo](./cromo/) | modemmanager compatible D-Bus interface to support closed source modem drivers |
| [cros-disks](./cros-disks/) | Daemon for mounting removable media (e.g. USB sticks and SD cards) |
| [cros_component](./cros_component/) ||
| [crosdns](./crosdns/) | Hostname resolution service for Chrome OS |
| [crosh](./crosh/) | The Chromium OS shell |
| [cryptohome](./cryptohome/) | Daemon and tools for managing encrypted /home and /var directories |
| [debugd](./debugd/) | Centralized debug daemon for random tools |
| [disk_updater](./disk_updater/) | Utility for updating root disk firmware (e.g. SSDs and eMMC) |
| [easy-unlock](./easy-unlock/) | Daemon for handling Easy Unlock requests (e.g. unlocking Chromebooks with an Android device) |
| [feedback](./feedback/) | Daemon for headless systems that want to gather feedback (normally Chrome manages it) |
| [fitpicker](./fitpicker/) ||
| [gobi-cromo-plugin](./gobi-cromo-plugin/) | plugin for supporting gobi3k modems with cromo |
| [goldfishd](./goldfishd/) | Android Emulator Daemon |
| [hammerd](./hammerd/) | Firmware updater utility for hammer hardware |
| [image-burner](./image-burner/) | Daemon for writing disk images (e.g. recovery) to USB sticks & SD cards |
| [imageloader](./imageloader/) | Daemon for mounting signed disk images |
| [init](./init/) | CrOS common startup init scripts and boot time helpers |
| [installer](./installer/) | CrOS installer utility (for AU/recovery/etc...) |
| [ippusb_manager](./ippusb_manager/) | "Service" for ipp-over-usb printing |
| [libchromeos-ui](./libchromeos-ui/) ||
| [libcontainer](./libcontainer/) ||
| [libpasswordprovider](./libpasswordprovider/) | Password Provider library for securely managing credentials with system services |
| [libweave](./libweave/) | Library for cloud messaging |
| [login_manager](./login_manager/) | Session manager for handling the life cycle of the main session (e.g. Chrome) |
| [lorgnette](./lorgnette/) | Daemon for managing attached USB scanners via [SANE](https://en.wikipedia.org/wiki/Scanner_Access_Now_Easy) |
| [metrics](./metrics/) | Client side user metrics collection |
| [midis](./midis/) | [MIDI](https://en.wikipedia.org/wiki/MIDI) service |
| [mist](./mist/) | Modem USB Interface Switching Tool |
| [ml](./ml/) | Machine learning service |
| [modem-utilities](./modem-utilities/) ||
| [modemfwd](./modemfwd/) | Daemon for managing modem firmware updaters |
| [mtpd](./mtpd/) | Daemon for handling Media Transfer Protocol (MTP) with devices (e.g. phones) |
| [p2p](./p2p/) | Service for sharing files between CrOS devices (e.g. updates) |
| [peerd](./peerd/) | Daemon for communicating with local peers |
| [permission_broker](./permission_broker/) ||
| [power_manager](./power_manager/) | Userspace power management daemon and associated tools |
| [regions](./regions/) ||
| [run_oci](./run_oci/) | Minimalistic container runtime |
| [salsa](./salsa/) | Touchpad experimentation framework |
| [secure_erase_file](./secure_erase_file/) | Helper tools for securely erasing files from storage (e.g. keys and PII data) |
| [smbprovider](./smbprovider/) | Daemon for connecting Samba / Windows networking shares to the Files.app |
| [smogcheck](./smogcheck/) | Developer library for working with raw I2C devices |
| [st_flash](./st_flash/) ||
| [storage_info](./storage_info/) | Helper shell functions for retrieving disk information) |
| [thd](./thd/) | Thermal daemon to help keep systems running cool |
| [timberslide](./timberslide/) | Tool for working with EC crashes for reporting purposes |
| [touch_firmware_calibration](./touch_firmware_calibration/) ||
| [touch_keyboard](./touch_keyboard/) | Utilities for a touch based virtual keyboard |
| [tpm2-simulator](./tpm2-simulator/) | A software TPM 2.0 implementation (for testing/debugging) |
| [tpm_manager](./tpm_manager/) | Daemon and client for managing TPM setup and operations |
| [trim](./trim/) | Service to manage filesystem trim operations in the background |
| [trunks](./trunks/) | Middleware and resource manager for interfacing with TPM 2.0 hardware |
| [u2fd](./u2fd/) | U2FHID emulation daemon for systems with secure elements (not TPMs) |
| [userfeedback](./userfeedback/) | Various utilities to gather extended data for user feedback reports |
| [userspace_touchpad](./userspace_touchpad/) ||
| [virtual_file_provider](./virtual_file_provider/) ||
| [vm_tools](./vm_tools/) | Utilities for Virtual Machine (VM) orchestration |
| [vpn-manager](./vpn-manager/) ||
| [webserver](./webserver/) | Small web server with D-Bus client backends |
| [wifi-testbed](./wifi-testbed/) | Tools for creating a WiFi testbed image |
| [wimax_manager](./wimax_manager/) ||

# AOSP Project Directory

These projects can be found here:
https://chromium.googlesource.com/aosp/platform/
