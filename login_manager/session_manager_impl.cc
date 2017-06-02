// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/session_manager_impl.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

#include <algorithm>
#include <locale>
#include <memory>
#include <string>
#include <utility>

#include <base/base64.h>
#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/memory/ptr_util.h>
#include <base/memory/ref_counted.h>
#include <base/message_loop/message_loop.h>
#include <base/rand_util.h>
#include <base/run_loop.h>
#include <base/strings/string_tokenizer.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <base/time/time.h>
#include <brillo/cryptohome.h>
#include <chromeos/dbus/service_constants.h>
#include <crypto/scoped_nss_types.h>
#include <dbus/message.h>
#include <dbus/object_proxy.h>
#include <install_attributes/libinstallattributes.h>

#include "bindings/chrome_device_policy.pb.h"
#include "bindings/device_management_backend.pb.h"
#include "login_manager/blob_util.h"
#include "login_manager/crossystem.h"
#include "login_manager/dbus_signal_emitter.h"
#include "login_manager/dbus_util.h"
#include "login_manager/device_local_account_policy_service.h"
#include "login_manager/device_policy_service.h"
#include "login_manager/init_daemon_controller.h"
#include "login_manager/key_generator.h"
#include "login_manager/login_metrics.h"
#include "login_manager/nss_util.h"
#include "login_manager/policy_key.h"
#include "login_manager/policy_service.h"
#include "login_manager/process_manager_service_interface.h"
#include "login_manager/regen_mitigator.h"
#include "login_manager/system_utils.h"
#include "login_manager/user_policy_service_factory.h"
#include "login_manager/vpd_process.h"

using base::FilePath;
using brillo::cryptohome::home::GetRootPath;
using brillo::cryptohome::home::GetUserPath;
using brillo::cryptohome::home::SanitizeUserName;
using brillo::cryptohome::home::kGuestUserName;

namespace login_manager {  // NOLINT

const char SessionManagerImpl::kDemoUser[] = "demouser@";

const char SessionManagerImpl::kStarted[] = "started";
const char SessionManagerImpl::kStopping[] = "stopping";
const char SessionManagerImpl::kStopped[] = "stopped";

const char SessionManagerImpl::kLoggedInFlag[] =
    "/run/session_manager/logged_in";
const char SessionManagerImpl::kResetFile[] =
    "/mnt/stateful_partition/factory_install_reset";

const char SessionManagerImpl::kStartUserSessionSignal[] = "start-user-session";

const char SessionManagerImpl::kArcContainerName[] = "android";
const char SessionManagerImpl::kArcStartForLoginScreenSignal[] =
    "start-arc-instance-for-login-screen";
const char SessionManagerImpl::kArcStartSignal[] = "start-arc-instance";
const char SessionManagerImpl::kArcStopSignal[] = "stop-arc-instance";
const char SessionManagerImpl::kArcNetworkStartSignal[] = "start-arc-network";
const char SessionManagerImpl::kArcNetworkStopSignal[] = "stop-arc-network";
const char SessionManagerImpl::kArcBootedSignal[] = "arc-booted";
const char SessionManagerImpl::kArcRemoveOldDataSignal[] =
    "remove-old-arc-data";

namespace {
// Constants used in email validation.
const char kEmailSeparator = '@';
const char kEmailLegalCharacters[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    ".@1234567890!#$%&'*+-/=?^_`{|}~";

// Should match chromium AccountId::kKeyGaiaIdPrefix .
const char kGaiaIdKeyPrefix[] = "g-";
// Should match chromium AccountId::kKeyAdIdPrefix .
const char kActiveDirectoryPrefix[] = "a-";
const char kAccountIdKeyLegalCharacters[] =
    "-0123456789"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// The flag to pass to chrome to open a named socket for testing.
const char kTestingChannelFlag[] = "--testing-channel=NamedTestingInterface:";

// Device-local account state directory.
const base::FilePath::CharType kDeviceLocalAccountStateDir[] =
    FILE_PATH_LITERAL("/var/lib/device_local_accounts");

#if USE_CHEETS
// To launch ARC, certain amount of free disk space is needed.
// Path and the amount for the check.
constexpr base::FilePath::CharType kArcDiskCheckPath[] = "/home";
constexpr int64_t kArcCriticalDiskFreeBytes = 64 << 20;  // 64MB
constexpr size_t kArcContainerInstanceIdLength = 16;

// Name of android-data directory.
const base::FilePath::CharType kAndroidDataDirName[] =
    FILE_PATH_LITERAL("android-data");

// Name of android-data-old directory which RemoveArcDataInternal uses.
const base::FilePath::CharType kAndroidDataOldDirName[] =
    FILE_PATH_LITERAL("android-data-old");

// To set the CPU limits of the Android container.
const char kCpuSharesFile[] =
    "/sys/fs/cgroup/cpu/session_manager_containers/cpu.shares";
const unsigned int kCpuSharesForeground = 1024;
const unsigned int kCpuSharesBackground = 64;
#endif

// SystemUtils::EnsureJobExit() DCHECKs if the timeout is zero, so this is the
// minimum amount of time we must wait before killing the containers.
constexpr base::TimeDelta kContainerTimeout = base::TimeDelta::FromSeconds(1);

// The interval used to periodically check if time sync was done by tlsdated.
constexpr int kSystemClockLastSyncInfoRetryMs = 1000;

bool IsIncognitoAccountId(const std::string& account_id) {
  const std::string lower_case_id(base::ToLowerASCII(account_id));
  return (lower_case_id == kGuestUserName) ||
         (lower_case_id == SessionManagerImpl::kDemoUser);
}

// Adapter from DBusMethodResponse to PolicyService::Completion callback.
void HandlePolicyServiceCompletion(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    brillo::ErrorPtr error) {
  if (error) {
    response->ReplyWithError(error.get());
    return;
  }

  response->Return();
}

#if USE_CHEETS
bool IsDevMode(SystemUtils* system) {
  // When GetDevModeState() returns UNKNOWN, return true.
  return system->GetDevModeState() != DevModeState::DEV_MODE_OFF;
}

bool IsInsideVm(SystemUtils* system) {
  // When GetVmState() returns UNKNOWN, return false.
  return system->GetVmState() == VmState::INSIDE_VM;
}
#endif

}  // namespace

SessionManagerImpl::Error::Error() : set_(false) {}

SessionManagerImpl::Error::Error(const std::string& name,
                                 const std::string& message)
    : name_(name), message_(message), set_(true) {}

SessionManagerImpl::Error::~Error() {}

void SessionManagerImpl::Error::Set(const std::string& name,
                                    const std::string& message) {
  name_ = name;
  message_ = message;
  set_ = true;
}

struct SessionManagerImpl::UserSession {
 public:
  UserSession(const std::string& username,
              const std::string& userhash,
              bool is_incognito,
              crypto::ScopedPK11Slot slot,
              std::unique_ptr<PolicyService> policy_service)
      : username(username),
        userhash(userhash),
        is_incognito(is_incognito),
        slot(std::move(slot)),
        policy_service(std::move(policy_service)) {}
  ~UserSession() {}

  const std::string username;
  const std::string userhash;
  const bool is_incognito;
  crypto::ScopedPK11Slot slot;
  std::unique_ptr<PolicyService> policy_service;
};

SessionManagerImpl::SessionManagerImpl(
    std::unique_ptr<InitDaemonController> init_controller,
    DBusSignalEmitterInterface* dbus_emitter,
    base::Closure lock_screen_closure,
    base::Closure restart_device_closure,
    base::Closure start_arc_instance_closure,
    base::Closure stop_arc_instance_closure,
    KeyGenerator* key_gen,
    ServerBackedStateKeyGenerator* state_key_generator,
    ProcessManagerServiceInterface* manager,
    LoginMetrics* metrics,
    NssUtil* nss,
    SystemUtils* utils,
    Crossystem* crossystem,
    VpdProcess* vpd_process,
    PolicyKey* owner_key,
    ContainerManagerInterface* android_container,
    InstallAttributesReader* install_attributes_reader,
    dbus::ObjectProxy* system_clock_proxy)
    : session_started_(false),
      session_stopping_(false),
      screen_locked_(false),
      supervised_user_creation_ongoing_(false),
      system_clock_synchronized_(false),
      init_controller_(std::move(init_controller)),
      lock_screen_closure_(lock_screen_closure),
      restart_device_closure_(restart_device_closure),
      start_arc_instance_closure_(start_arc_instance_closure),
      stop_arc_instance_closure_(stop_arc_instance_closure),
      dbus_emitter_(dbus_emitter),
      key_gen_(key_gen),
      state_key_generator_(state_key_generator),
      manager_(manager),
      login_metrics_(metrics),
      nss_(nss),
      system_(utils),
      crossystem_(crossystem),
      vpd_process_(vpd_process),
      owner_key_(owner_key),
      android_container_(android_container),
      install_attributes_reader_(install_attributes_reader),
      system_clock_proxy_(system_clock_proxy),
      mitigator_(key_gen),
      weak_ptr_factory_(this) {}

SessionManagerImpl::~SessionManagerImpl() {
  device_policy_->set_delegate(NULL);  // Could use WeakPtr instead?
}

// static
bool SessionManagerImpl::ValidateAccountIdKey(const std::string& account_id) {
  if (account_id.find_first_not_of(kAccountIdKeyLegalCharacters) !=
      std::string::npos)
    return false;

  return base::StartsWith(account_id, kGaiaIdKeyPrefix,
                          base::CompareCase::SENSITIVE) ||
         base::StartsWith(account_id, kActiveDirectoryPrefix,
                          base::CompareCase::SENSITIVE);
}

// static
bool SessionManagerImpl::ValidateEmail(const std::string& email_address) {
  if (email_address.find_first_not_of(kEmailLegalCharacters) !=
      std::string::npos) {
    return false;
  }

  size_t at = email_address.find(kEmailSeparator);
  // it has NO @.
  if (at == std::string::npos)
    return false;

  // it has more than one @.
  if (email_address.find(kEmailSeparator, at + 1) != std::string::npos)
    return false;

  return true;
}

#if USE_CHEETS
// static
base::FilePath SessionManagerImpl::GetAndroidDataDirForUser(
    const std::string& normalized_account_id) {
  return GetRootPath(normalized_account_id).Append(kAndroidDataDirName);
}

// static
base::FilePath SessionManagerImpl::GetAndroidDataOldDirForUser(
    const std::string& normalized_account_id) {
  return GetRootPath(normalized_account_id).Append(kAndroidDataOldDirName);
}
#endif  // USE_CHEETS

void SessionManagerImpl::SetPolicyServicesForTest(
    std::unique_ptr<DevicePolicyService> device_policy,
    std::unique_ptr<UserPolicyServiceFactory> user_policy_factory,
    std::unique_ptr<DeviceLocalAccountPolicyService>
        device_local_account_policy) {
  device_policy_ = std::move(device_policy);
  user_policy_factory_ = std::move(user_policy_factory);
  device_local_account_policy_ = std::move(device_local_account_policy);
}

void SessionManagerImpl::AnnounceSessionStoppingIfNeeded() {
  if (session_started_) {
    session_stopping_ = true;
    DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:" << kStopping;
    dbus_emitter_->EmitSignalWithString(kSessionStateChangedSignal, kStopping);
  }
}

void SessionManagerImpl::AnnounceSessionStopped() {
  session_stopping_ = session_started_ = false;
  DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:" << kStopped;
  dbus_emitter_->EmitSignalWithString(kSessionStateChangedSignal, kStopped);
}

bool SessionManagerImpl::ShouldEndSession() {
  return screen_locked_ || supervised_user_creation_ongoing_;
}

bool SessionManagerImpl::Initialize() {
  key_gen_->set_delegate(this);

  system_clock_proxy_->WaitForServiceToBeAvailable(
      base::Bind(&SessionManagerImpl::OnSystemClockServiceAvailable,
                 weak_ptr_factory_.GetWeakPtr()));

  if (!device_policy_) {
    device_policy_.reset(
        DevicePolicyService::Create(login_metrics_, owner_key_, &mitigator_,
                                    nss_, crossystem_, vpd_process_));
    device_policy_->set_delegate(this);

    user_policy_factory_.reset(
        new UserPolicyServiceFactory(getuid(), nss_, system_));
    device_local_account_policy_.reset(new DeviceLocalAccountPolicyService(
        base::FilePath(kDeviceLocalAccountStateDir), owner_key_));

    if (!device_policy_->Initialize()) {
      return false;
    }
    device_local_account_policy_->UpdateDeviceSettings(
        device_policy_->GetSettings());
    if (device_policy_->MayUpdateSystemSettings()) {
      device_policy_->UpdateSystemSettings(PolicyService::Completion());
    }
  }
  return true;
}

void SessionManagerImpl::Finalize() {
  device_policy_->PersistPolicy(PolicyService::Completion());
  for (UserSessionMap::const_iterator it = user_sessions_.begin();
       it != user_sessions_.end(); ++it) {
    if (it->second)
      it->second->policy_service->PersistPolicy(PolicyService::Completion());
  }
  // We want to stop any running containers.  Containers are per-session and
  // cannot persist across sessions.
  android_container_->RequestJobExit();
  android_container_->EnsureJobExit(kContainerTimeout);
}

void SessionManagerImpl::EmitLoginPromptVisible() {
  login_metrics_->RecordStats("login-prompt-visible");
  dbus_emitter_->EmitSignal(kLoginPromptVisibleSignal);
  init_controller_->TriggerImpulse("login-prompt-visible", {},
                                   InitDaemonController::TriggerMode::ASYNC);
}

bool SessionManagerImpl::EnableChromeTesting(
    brillo::ErrorPtr* error,
    bool in_force_relaunch,
    const std::vector<std::string>& in_extra_arguments,
    std::string* out_filepath) {
  // Check to see if we already have Chrome testing enabled.
  bool already_enabled = !chrome_testing_path_.empty();

  if (!already_enabled) {
    base::FilePath temp_file_path;  // So we don't clobber chrome_testing_path_;
    if (!system_->GetUniqueFilenameInWriteOnlyTempDir(&temp_file_path)) {
      *error = CreateError(
          dbus_error::kTestingChannelError,
          "Could not create testing channel filename.");
      return false;
    }
    chrome_testing_path_ = temp_file_path;
  }

  if (!already_enabled || in_force_relaunch) {
    // Delete testing channel file if it already exists.
    system_->RemoveFile(chrome_testing_path_);

    // Add testing channel argument to extra arguments.
    std::string testing_argument = kTestingChannelFlag;
    testing_argument.append(chrome_testing_path_.value());
    std::vector<std::string> extra_args = in_extra_arguments;
    extra_args.push_back(testing_argument);
    manager_->RestartBrowserWithArgs(extra_args, true);
  }
  *out_filepath = chrome_testing_path_.value();
  return true;
}

bool SessionManagerImpl::StartSession(brillo::ErrorPtr* error,
                                      const std::string& in_account_id,
                                      const std::string& in_unique_identifier) {
  std::string actual_account_id;
  if (!NormalizeAccountId(in_account_id, &actual_account_id, error)) {
    DCHECK(*error);
    return false;
  }

  // Check if this user already started a session.
  if (user_sessions_.count(actual_account_id) > 0) {
    constexpr char kMessage[] = "Provided user id already started a session.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSessionExists, kMessage);
    return false;
  }

  // Create a UserSession object for this user.
  const bool is_incognito = IsIncognitoAccountId(actual_account_id);
  auto user_session =
      CreateUserSession(actual_account_id, is_incognito, error);
  if (!user_session) {
    DCHECK(*error);
    return false;
  }

  // Check whether the current user is the owner, and if so make sure they are
  // whitelisted and have an owner key.
  bool user_is_owner = false;
  if (!device_policy_->CheckAndHandleOwnerLogin(
          user_session->username, user_session->slot.get(), &user_is_owner,
          error)) {
    DCHECK(*error);
    return false;
  }

  // If all previous sessions were incognito (or no previous sessions exist).
  bool is_first_real_user = AllSessionsAreIncognito() && !is_incognito;

  // Send each user login event to UMA (right before we start session
  // since the metrics library does not log events in guest mode).
  const DevModeState dev_mode_state = system_->GetDevModeState();
  if (dev_mode_state != DevModeState::DEV_MODE_UNKNOWN) {
    login_metrics_->SendLoginUserType(
        dev_mode_state != DevModeState::DEV_MODE_OFF, is_incognito,
        user_is_owner);
  }

  init_controller_->TriggerImpulse(kStartUserSessionSignal,
                                   {"CHROMEOS_USER=" + actual_account_id},
                                   InitDaemonController::TriggerMode::ASYNC);
  LOG(INFO) << "Starting user session";
  manager_->SetBrowserSessionForUser(actual_account_id, user_session->userhash);
  session_started_ = true;
  user_sessions_[actual_account_id] = std::move(user_session);
  DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:" << kStarted;
  dbus_emitter_->EmitSignalWithString(kSessionStateChangedSignal, kStarted);

  // Active Directory managed devices are not expected to have a policy key.
  // Don't create one for them.
  const bool is_active_directory =
      install_attributes_reader_->GetAttribute(
          InstallAttributesReader::kAttrMode) ==
      InstallAttributesReader::kDeviceModeEnterpriseAD;
  if (device_policy_->KeyMissing() && !is_active_directory &&
      !device_policy_->Mitigating() && is_first_real_user) {
    // This is the first sign-in on this unmanaged device.  Take ownership.
    key_gen_->Start(actual_account_id);
  }

  // Record that a login has successfully completed on this boot.
  system_->AtomicFileWrite(base::FilePath(kLoggedInFlag), "1");
  return true;
}

void SessionManagerImpl::StopSession(const std::string& in_unique_identifier) {
  LOG(INFO) << "Stopping all sessions";
  // Most calls to StopSession() will log the reason for the call.
  // If you don't see a log message saying the reason for the call, it is
  // likely a DBUS message. See session_manager_dbus_adaptor.cc for that call.
  manager_->ScheduleShutdown();
  // TODO(cmasone): re-enable these when we try to enable logout without exiting
  //                the session manager
  // browser_.job->StopSession();
  // user_policy_.reset();
  // session_started_ = false;
}

void SessionManagerImpl::StorePolicy(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const std::vector<uint8_t>& in_policy_blob) {
  StorePolicyInternal(
      in_policy_blob, SignatureCheck::kEnabled, std::move(response));
}

void SessionManagerImpl::StoreUnsignedPolicy(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const std::vector<uint8_t>& in_policy_blob) {
  brillo::ErrorPtr error = VerifyUnsignedPolicyStore();
  if (error) {
    response->ReplyWithError(error.get());
    return;
  }

  StorePolicyInternal(
      in_policy_blob, SignatureCheck::kDisabled, std::move(response));
}

bool SessionManagerImpl::RetrievePolicy(
    brillo::ErrorPtr* error,
    std::vector<uint8_t>* out_policy_blob) {
  if (!device_policy_->Retrieve(out_policy_blob)) {
    constexpr char kMessage[] = "Failed to retrieve policy data.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSigEncodeFail, kMessage);
    return false;
  }

  return true;
}

void SessionManagerImpl::StorePolicyForUser(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const std::string& in_account_id,
    const std::vector<uint8_t>& in_policy_blob) {
  StorePolicyForUserInternal(
      in_account_id, in_policy_blob, SignatureCheck::kEnabled,
      std::move(response));
}

void SessionManagerImpl::StoreUnsignedPolicyForUser(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const std::string& in_account_id,
    const std::vector<uint8_t>& in_policy_blob) {
  brillo::ErrorPtr error = VerifyUnsignedPolicyStore();
  if (error) {
    response->ReplyWithError(error.get());
    return;
  }

  StorePolicyForUserInternal(
      in_account_id, in_policy_blob, SignatureCheck::kDisabled,
      std::move(response));
}

bool SessionManagerImpl::RetrievePolicyForUser(
    brillo::ErrorPtr* error,
    const std::string& in_account_id,
    std::vector<uint8_t>* out_policy_blob) {
  PolicyService* policy_service = GetPolicyService(in_account_id);
  if (!policy_service) {
    constexpr char kMessage[] =
        "Cannot retrieve user policy before session is started.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSessionDoesNotExist, kMessage);
    return false;
  }
  if (!policy_service->Retrieve(out_policy_blob)) {
    constexpr char kMessage[] = "Failed to retrieve policy data.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSigEncodeFail, kMessage);
    return false;
  }

  return true;
}

void SessionManagerImpl::StoreDeviceLocalAccountPolicy(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const std::string& in_account_id,
    const std::vector<uint8_t>& in_policy_blob) {
  device_local_account_policy_->Store(
      in_account_id, in_policy_blob,
      base::Bind(HandlePolicyServiceCompletion, base::Passed(&response)));
}

bool SessionManagerImpl::RetrieveDeviceLocalAccountPolicy(
      brillo::ErrorPtr* error,
      const std::string& in_account_id,
      std::vector<uint8_t>* out_policy_blob) {
  if (!device_local_account_policy_->Retrieve(
          in_account_id, out_policy_blob)) {
    constexpr char kMessage[] = "Failed to retrieve policy data.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSigEncodeFail, kMessage);
    return false;
  }

  return true;
}

std::string SessionManagerImpl::RetrieveSessionState() {
  if (!session_started_)
    return kStopped;
  if (session_stopping_)
    return kStopping;
  return kStarted;
}

std::map<std::string, std::string>
SessionManagerImpl::RetrieveActiveSessions() {
  std::map<std::string, std::string> result;
  for (const auto& entry : user_sessions_) {
    if (!entry.second)
      continue;
    result[entry.second->username] = entry.second->userhash;
  }
  return result;
}

void SessionManagerImpl::HandleSupervisedUserCreationStarting() {
  supervised_user_creation_ongoing_ = true;
}

void SessionManagerImpl::HandleSupervisedUserCreationFinished() {
  supervised_user_creation_ongoing_ = false;
}

bool SessionManagerImpl::LockScreen(brillo::ErrorPtr* error) {
  if (!session_started_) {
    constexpr char kMessage[] =
        "Attempt to lock screen outside of user session.";
    LOG(WARNING) << kMessage;
    *error = CreateError(dbus_error::kSessionDoesNotExist, kMessage);
    return false;
  }
  // If all sessions are incognito, then locking is not allowed.
  if (AllSessionsAreIncognito()) {
    constexpr char kMessage[] = "Attempt to lock screen during Guest session.";
    LOG(WARNING) << kMessage;
    *error = CreateError(dbus_error::kSessionExists, kMessage);
    return false;
  }
  if (!screen_locked_) {
    screen_locked_ = true;
    lock_screen_closure_.Run();
  }
  LOG(INFO) << "LockScreen() method called.";
  return true;
}

void SessionManagerImpl::HandleLockScreenShown() {
  LOG(INFO) << "HandleLockScreenShown() method called.";
  dbus_emitter_->EmitSignal(kScreenIsLockedSignal);
}

void SessionManagerImpl::HandleLockScreenDismissed() {
  screen_locked_ = false;
  LOG(INFO) << "HandleLockScreenDismissed() method called.";
  dbus_emitter_->EmitSignal(kScreenIsUnlockedSignal);
}

bool SessionManagerImpl::RestartJob(brillo::ErrorPtr* error,
                                    const dbus::FileDescriptor& in_cred_fd,
                                    const std::vector<std::string>& in_argv) {
  struct ucred ucred = {0};
  socklen_t len = sizeof(struct ucred);
  if (!in_cred_fd.is_valid() ||
      getsockopt(
          in_cred_fd.value(), SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
    PLOG(ERROR) << "Can't get peer creds";
    *error = CreateError("GetPeerCredsFailed", strerror(errno));
    return false;
  }

  if (!manager_->IsBrowser(ucred.pid)) {
    constexpr char kMessage[] = "Provided pid is unknown.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kUnknownPid, kMessage);
    return false;
  }

  // To set "logged-in" state for BWSI mode.
  if (!StartSession(error, kGuestUserName, "")) {
    DCHECK(*error);
    return false;
  }

  manager_->RestartBrowserWithArgs(in_argv, false);
  return true;
}

bool SessionManagerImpl::StartDeviceWipe(brillo::ErrorPtr* error) {
  if (system_->Exists(base::FilePath(kLoggedInFlag))) {
    constexpr char kMessage[] = "A user has already logged in this boot.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kSessionExists, kMessage);
    return false;
  }
  InitiateDeviceWipe("session_manager_dbus_request");
  return true;
}

void SessionManagerImpl::SetFlagsForUser(
    const std::string& in_account_id,
    const std::vector<std::string>& in_flags) {
  manager_->SetFlagsForUser(in_account_id, in_flags);
}

void SessionManagerImpl::RequestServerBackedStateKeys(
    const ServerBackedStateKeyGenerator::StateKeyCallback& callback) {
  if (system_clock_synchronized_) {
    state_key_generator_->RequestStateKeys(callback);
  } else {
    pending_state_key_callbacks_.push_back(callback);
  }
}

void SessionManagerImpl::OnSystemClockServiceAvailable(bool service_available) {
  if (!service_available) {
    LOG(ERROR) << "Failed to listen for tlsdated service start";
    return;
  }

  GetSystemClockLastSyncInfo();
}

void SessionManagerImpl::GetSystemClockLastSyncInfo() {
  dbus::MethodCall method_call(system_clock::kSystemClockInterface,
                               system_clock::kSystemLastSyncInfo);
  dbus::MessageWriter writer(&method_call);

  system_clock_proxy_->CallMethod(
      &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
      base::Bind(&SessionManagerImpl::OnGotSystemClockLastSyncInfo,
                 weak_ptr_factory_.GetWeakPtr()));
}

void SessionManagerImpl::OnGotSystemClockLastSyncInfo(
    dbus::Response* response) {
  if (!response) {
    LOG(ERROR) << system_clock::kSystemClockInterface << "."
               << system_clock::kSystemLastSyncInfo << " request failed.";
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE, base::Bind(&SessionManagerImpl::GetSystemClockLastSyncInfo,
                              weak_ptr_factory_.GetWeakPtr()),
        base::TimeDelta::FromMilliseconds(kSystemClockLastSyncInfoRetryMs));
    return;
  }

  dbus::MessageReader reader(response);
  bool network_synchronized = false;
  if (!reader.PopBool(&network_synchronized)) {
    LOG(ERROR) << system_clock::kSystemClockInterface << "."
               << system_clock::kSystemLastSyncInfo
               << " response lacks network-synchronized argument";
    return;
  }

  if (network_synchronized) {
    system_clock_synchronized_ = true;
    for (auto it = pending_state_key_callbacks_.begin();
         it != pending_state_key_callbacks_.end(); ++it) {
      state_key_generator_->RequestStateKeys(*it);
    }
    pending_state_key_callbacks_.clear();
  } else {
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE, base::Bind(&SessionManagerImpl::GetSystemClockLastSyncInfo,
                              weak_ptr_factory_.GetWeakPtr()),
        base::TimeDelta::FromMilliseconds(kSystemClockLastSyncInfoRetryMs));
  }
}

bool SessionManagerImpl::InitMachineInfo(brillo::ErrorPtr* error,
                                         const std::string& in_data) {
  std::map<std::string, std::string> params;
  if (!ServerBackedStateKeyGenerator::ParseMachineInfo(in_data, &params)) {
    *error = CreateError(
        dbus_error::kInitMachineInfoFail, "Parse failure.");
    return false;
  }

  if (!state_key_generator_->InitMachineInfo(params)) {
    *error = CreateError(
        dbus_error::kInitMachineInfoFail, "Missing parameters.");
    return false;
  }
  return true;
}

void SessionManagerImpl::StartArcInstanceForLoginScreen(
    std::string* container_instance_id_out,
    Error* error) {
#if USE_CHEETS
  const std::vector<std::string> keyvals = {
      base::StringPrintf("CHROMEOS_DEV_MODE=%d", IsDevMode(system_)),
      base::StringPrintf("CHROMEOS_INSIDE_VM=%d", IsInsideVm(system_)),
  };

  brillo::ErrorPtr error_ptr;
  std::string container_instance_id =
      StartArcContainer(kArcStartForLoginScreenSignal, keyvals, &error_ptr);
  if (container_instance_id.empty()) {
    DCHECK(error_ptr);
    error->Set(error_ptr->GetCode(), error_ptr->GetMessage());
    return;
  }

  *container_instance_id_out = container_instance_id;
  start_arc_instance_closure_.Run();
#else
  error->Set(dbus_error::kNotAvailable, "ARC not supported.");
#endif  // !USE_CHEETS
}

void SessionManagerImpl::StartArcInstance(
    const std::string& account_id,
    bool skip_boot_completed_broadcast,
    bool scan_vendor_priv_app,
    std::string* container_instance_id_out,
    Error* error) {
#if USE_CHEETS
  arc_start_time_ = base::TimeTicks::Now();

  // To boot ARC instance, certain amount of disk space is needed under the
  // home. We first check it.
  if (system_->AmountOfFreeDiskSpace(base::FilePath(kArcDiskCheckPath)) <
      kArcCriticalDiskFreeBytes) {
    static const char msg[] = "Low free disk under /home";
    LOG(ERROR) << msg;
    error->Set(dbus_error::kLowFreeDisk, msg);
    return;
  }

  brillo::ErrorPtr error_ptr;
  std::string actual_account_id;
  if (!NormalizeAccountId(account_id, &actual_account_id, &error_ptr)) {
    DCHECK(error_ptr.get());
    error->Set(error_ptr->GetCode(), error_ptr->GetMessage());
    return;
  }
  if (user_sessions_.count(actual_account_id) == 0) {
    static const char msg[] = "Provided user ID does not have a session.";
    LOG(ERROR) << msg;
    error->Set(dbus_error::kSessionDoesNotExist, msg);
    return;
  }

  const base::FilePath android_data_dir =
      GetAndroidDataDirForUser(actual_account_id);
  const base::FilePath android_data_old_dir =
      GetAndroidDataOldDirForUser(actual_account_id);

  const std::vector<std::string> keyvals = {
      base::StringPrintf("ANDROID_DATA_DIR=%s",
                         android_data_dir.value().c_str()),
      base::StringPrintf("ANDROID_DATA_OLD_DIR=%s",
                         android_data_old_dir.value().c_str()),
      base::StringPrintf("CHROMEOS_USER=%s", actual_account_id.c_str()),
      base::StringPrintf("CHROMEOS_DEV_MODE=%d", IsDevMode(system_)),
      base::StringPrintf("CHROMEOS_INSIDE_VM=%d", IsInsideVm(system_)),
      base::StringPrintf("DISABLE_BOOT_COMPLETED_BROADCAST=%d",
                         skip_boot_completed_broadcast),
      base::StringPrintf("ENABLE_VENDOR_PRIVILEGED=%d",
                         scan_vendor_priv_app),
  };

  std::string container_instance_id =
      StartArcContainer(kArcStartSignal, keyvals, &error_ptr);
  if (container_instance_id.empty()) {
    DCHECK(error_ptr);
    error->Set(error_ptr->GetCode(), error_ptr->GetMessage());
    return;
  }

  if (!StartArcNetwork(&error_ptr)) {
    // Asking the container to exit will result in OnAndroidContainerStopped()
    // being called, which will handle any necessary cleanup.
    android_container_->RequestJobExit();
    android_container_->EnsureJobExit(kContainerTimeout);

    DCHECK(error_ptr);
    error->Set(error_ptr->GetCode(), error_ptr->GetMessage());
    return;
  }

  login_metrics_->StartTrackingArcUseTime();
  *container_instance_id_out = container_instance_id;
  start_arc_instance_closure_.Run();
#else
  error->Set(dbus_error::kNotAvailable, "ARC not supported.");
#endif  // !USE_CHEETS
}

bool SessionManagerImpl::StopArcInstance(brillo::ErrorPtr* error) {
#if USE_CHEETS
  pid_t pid;
  if (!android_container_->GetContainerPID(&pid)) {
    constexpr char kMessage[] = "Error getting Android container pid.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kContainerShutdownFail, kMessage);
    return false;
  }

  android_container_->RequestJobExit();
  android_container_->EnsureJobExit(kContainerTimeout);
  return true;
#else
  *error = CreateError(dbus_error::kNotAvailable, "ARC not supported.");
  return false;
#endif  // USE_CHEETS
}

bool SessionManagerImpl::SetArcCpuRestriction(brillo::ErrorPtr* error,
                                              uint32_t in_restriction_state) {
#if USE_CHEETS
  std::string shares_out;
  switch (static_cast<ContainerCpuRestrictionState>(in_restriction_state)) {
    case CONTAINER_CPU_RESTRICTION_FOREGROUND:
      shares_out = std::to_string(kCpuSharesForeground);
      break;
    case CONTAINER_CPU_RESTRICTION_BACKGROUND:
      shares_out = std::to_string(kCpuSharesBackground);
      break;
    default:
      constexpr char kMessage[] = "Invalid CPU restriction state specified.";
      LOG(ERROR) << kMessage;
      *error = CreateError(dbus_error::kArcCpuCgroupFail, kMessage);
      return false;
  }
  if (base::WriteFile(base::FilePath(kCpuSharesFile), shares_out.c_str(),
                      shares_out.length()) != shares_out.length()) {
    constexpr char kMessage[] = "Error updating Android container's cgroups.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kArcCpuCgroupFail, kMessage);
    return false;
  }
  return true;
#else
  *error = CreateError(dbus_error::kNotAvailable, "ARC not supported.");
  return false;
#endif
}

bool SessionManagerImpl::EmitArcBooted(brillo::ErrorPtr* error,
                                       const std::string& in_account_id) {
#if USE_CHEETS
  std::vector<std::string> keyvals;
  if (!in_account_id.empty()) {
    std::string actual_account_id;
    if (!NormalizeAccountId(in_account_id, &actual_account_id, error)) {
      DCHECK(error);
      return false;
    }
    const base::FilePath android_data_old_dir =
        GetAndroidDataOldDirForUser(actual_account_id);
    keyvals.emplace_back("ANDROID_DATA_OLD_DIR=" +
                         android_data_old_dir.value());
  }

  if (!init_controller_->TriggerImpulse(
          kArcBootedSignal, keyvals, InitDaemonController::TriggerMode::SYNC)) {
    constexpr char kMessage[] = "Emitting arc-booted init signal failed.";
    LOG(ERROR) << kMessage;
    *error = CreateError(dbus_error::kEmitFailed, kMessage);
    return false;
  }

  return true;
#else
  *error = CreateError(dbus_error::kNotAvailable, "ARC not supported.");
  return false;
#endif
}

bool SessionManagerImpl::GetArcStartTimeTicks(brillo::ErrorPtr* error,
                                              int64_t* out_start_time) {
#if USE_CHEETS
  if (arc_start_time_.is_null()) {
    *error = CreateError(dbus_error::kNotStarted, "ARC is not started yet.");
    return false;
  }
  *out_start_time = arc_start_time_.ToInternalValue();
  return true;
#else
  *error = CreateError(dbus_error::kNotAvailable, "ARC not supported.");
  return false;
#endif  // !USE_CHEETS
}

bool SessionManagerImpl::StartContainer(brillo::ErrorPtr* error,
                                        const std::string& in_name) {
  // TODO(dgreid): Add support for other containers.
  constexpr char kMessage[] = "Container not found.";
  LOG(ERROR) << kMessage;
  *error = CreateError(dbus_error::kContainerStartupFail, kMessage);
  return false;
}

bool SessionManagerImpl::StopContainer(brillo::ErrorPtr* error,
                                       const std::string& in_name) {
  // TODO(dgreid): Add support for other containers.
  constexpr char kMessage[] = "Container not found.";
  LOG(ERROR) << kMessage;
  *error = CreateError(dbus_error::kContainerShutdownFail, kMessage);
  return false;
}

bool SessionManagerImpl::RemoveArcData(brillo::ErrorPtr* error,
                                       const std::string& in_account_id) {
#if USE_CHEETS
  pid_t pid = 0;
  if (android_container_->GetContainerPID(&pid)) {
    *error = CreateError(
        dbus_error::kArcInstanceRunning, "ARC is currently running.");
    return false;
  }

  std::string actual_account_id;
  if (!NormalizeAccountId(in_account_id, &actual_account_id, error)) {
    DCHECK(error);
    return false;
  }
  const base::FilePath android_data_dir =
      GetAndroidDataDirForUser(actual_account_id);
  const base::FilePath android_data_old_dir =
      GetAndroidDataOldDirForUser(actual_account_id);

  if (RemoveArcDataInternal(android_data_dir, android_data_old_dir))
    return true;  // all done.

  PLOG(WARNING) << "Failed to rename " << android_data_dir.value()
                << "; directly deleting it instead";
  // As a last resort, directly delete the directory although it's not always
  // safe to do. If session_manager is killed or the device is shut down while
  // doing the removal, the directory will have an unusual set of files which
  // may confuse ARC and prevent it from booting.
  system_->RemoveDirTree(android_data_dir);
  LOG(INFO) << "Finished removing " << android_data_dir.value();
  return true;
#else
  *error = CreateError(dbus_error::kNotAvailable, "ARC not supported.");
  return false;
#endif  // USE_CHEETS
}

#if USE_CHEETS
bool SessionManagerImpl::RemoveArcDataInternal(
    const base::FilePath& android_data_dir,
    const base::FilePath& android_data_old_dir) {
  // It should never happen, but in case |android_data_old_dir| is a file,
  // remove it. RemoveFile() immediately returns false (i.e. no-op) when
  // |android_data_old_dir| is a directory.
  system_->RemoveFile(android_data_old_dir);

  // Create |android_data_old_dir| if it doesn't exist.
  if (!system_->DirectoryExists(android_data_old_dir)) {
    if (!system_->CreateDir(android_data_old_dir)) {
      PLOG(ERROR) << "Failed to create " << android_data_old_dir.value();
      return false;
    }
  }

  if (!system_->DirectoryExists(android_data_dir) &&
      system_->IsDirectoryEmpty(android_data_old_dir)) {
    return true;  // nothing to do.
  }

  // Create a random temporary directory in |android_data_old_dir|.
  // Note: Renaming a directory to an existing empty directory works.
  base::FilePath target_dir_name;
  if (!system_->CreateTemporaryDirIn(android_data_old_dir, &target_dir_name)) {
    LOG(WARNING) << "Failed to create a temporary directory in "
                 << android_data_old_dir.value();
    return false;
  }
  LOG(INFO) << "Renaming " << android_data_dir.value() << " to "
            << target_dir_name.value();

  // Does the actual renaming here with rename(2). Note that if the process
  // (or the device itself) is killed / turned off right before the rename(2)
  // operation, both |android_data_dir| and |android_data_old_dir| will remain
  // while ARC is disabled in the browser side. In that case, the browser will
  // call RemoveArcData() later as needed, and both directories will disappear.
  if (system_->DirectoryExists(android_data_dir)) {
    if (!system_->RenameDir(android_data_dir, target_dir_name)) {
      LOG(WARNING) << "Failed to rename " << android_data_dir.value() << " to "
                   << target_dir_name.value();
      return false;
    }
  }

  // Ask init to remove all files and directories in |android_data_old_dir|.
  // Note that the init job never deletes |android_data_old_dir| itself so the
  // rename() operation above never fails.
  LOG(INFO) << "Removing contents in " << android_data_old_dir.value();
  if (!init_controller_->TriggerImpulse(
          kArcRemoveOldDataSignal,
          {"ANDROID_DATA_OLD_DIR=" + android_data_old_dir.value()},
          InitDaemonController::TriggerMode::SYNC)) {
    LOG(ERROR) << "Failed to emit " << kArcRemoveOldDataSignal
               << " init signal";
  }
  return true;
}
#endif  // USE_CHEETS

void SessionManagerImpl::OnPolicyPersisted(bool success) {
  device_local_account_policy_->UpdateDeviceSettings(
      device_policy_->GetSettings());
  dbus_emitter_->EmitSignalWithSuccessFailure(kPropertyChangeCompleteSignal,
                                              success);
}

void SessionManagerImpl::OnKeyPersisted(bool success) {
  dbus_emitter_->EmitSignalWithSuccessFailure(kOwnerKeySetSignal, success);
}

void SessionManagerImpl::OnKeyGenerated(const std::string& username,
                                        const base::FilePath& temp_key_file) {
  ImportValidateAndStoreGeneratedKey(username, temp_key_file);
}

void SessionManagerImpl::ImportValidateAndStoreGeneratedKey(
    const std::string& username,
    const base::FilePath& temp_key_file) {
  DLOG(INFO) << "Processing generated key at " << temp_key_file.value();
  std::string key;
  base::ReadFileToString(temp_key_file, &key);
  PLOG_IF(WARNING, !base::DeleteFile(temp_key_file, false))
      << "Can't delete " << temp_key_file.value();
  device_policy_->ValidateAndStoreOwnerKey(
      username, StringToBlob(key), user_sessions_[username]->slot.get());
}

void SessionManagerImpl::InitiateDeviceWipe(const std::string& reason) {
  // The log string must not be confused with other clobbers-state parameters.
  // Sanitize by replacing all non-alphanumeric characters with underscores and
  // clamping size to 50 characters.
  std::string sanitized_reason(reason.substr(0, 50));
  std::locale locale("C");
  std::replace_if(sanitized_reason.begin(), sanitized_reason.end(),
                  [&locale](const std::string::value_type character) {
                    return !std::isalnum(character, locale);
                  },
                  '_');
  const base::FilePath reset_path(kResetFile);
  system_->AtomicFileWrite(reset_path,
                           "fast safe keepimg reason=" + sanitized_reason);
  restart_device_closure_.Run();
}

// static
bool SessionManagerImpl::NormalizeAccountId(const std::string& account_id,
                                            std::string* actual_account_id_out,
                                            brillo::ErrorPtr* error_out) {
  // Validate the |account_id|.
  if (IsIncognitoAccountId(account_id) || ValidateAccountIdKey(account_id)) {
    *actual_account_id_out = account_id;
    return true;
  }

  // Support legacy email addresses.
  // TODO(alemate): remove this after ChromeOS will stop using email as
  // cryptohome identifier.
  const std::string& lower_email = base::ToLowerASCII(account_id);
  if (!ValidateEmail(lower_email)) {
    constexpr char kMessage[] =
        "Provided email address is not valid.  ASCII only.";
    LOG(ERROR) << kMessage;
    *error_out = CreateError(dbus_error::kInvalidAccount, kMessage);
    return false;
  }

  *actual_account_id_out = lower_email;
  return true;
}

bool SessionManagerImpl::AllSessionsAreIncognito() {
  size_t incognito_count = 0;
  for (UserSessionMap::const_iterator it = user_sessions_.begin();
       it != user_sessions_.end(); ++it) {
    if (it->second)
      incognito_count += it->second->is_incognito;
  }
  return incognito_count == user_sessions_.size();
}

std::unique_ptr<SessionManagerImpl::UserSession>
SessionManagerImpl::CreateUserSession(const std::string& username,
                                      bool is_incognito,
                                      brillo::ErrorPtr* error) {
  std::unique_ptr<PolicyService> user_policy(
      user_policy_factory_->Create(username));
  if (!user_policy) {
    LOG(ERROR) << "User policy failed to initialize.";
    *error = CreateError(dbus_error::kPolicyInitFail, "Can't create session.");
    return nullptr;
  }

  crypto::ScopedPK11Slot slot(nss_->OpenUserDB(GetUserPath(username)));
  if (!slot) {
    LOG(ERROR) << "Could not open the current user's NSS database.";
    *error = CreateError(dbus_error::kNoUserNssDb, "Can't create session.");
    return nullptr;
  }

  return base::MakeUnique<UserSession>(
      username, SanitizeUserName(username), is_incognito, std::move(slot),
      std::move(user_policy));
}

brillo::ErrorPtr SessionManagerImpl::VerifyUnsignedPolicyStore() {
  // Unsigned policy store D-Bus call is allowed only in enterprise_ad mode.
  const std::string& mode = install_attributes_reader_->GetAttribute(
      InstallAttributesReader::kAttrMode);
  if (mode != InstallAttributesReader::kDeviceModeEnterpriseAD) {
    constexpr char kMessage[] =
        "Device mode doesn't permit unsigned policy.";
    LOG(ERROR) << kMessage;
    return CreateError(dbus_error::kPolicySignatureRequired, kMessage);
  }

  return nullptr;
}

PolicyService* SessionManagerImpl::GetPolicyService(const std::string& user) {
  UserSessionMap::const_iterator it = user_sessions_.find(user);
  return it == user_sessions_.end() ? NULL : it->second->policy_service.get();
}

void SessionManagerImpl::StorePolicyInternal(
    const std::vector<uint8_t>& policy_blob,
    SignatureCheck signature_check,
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response) {
  int flags = PolicyService::KEY_ROTATE;
  if (!session_started_)
    flags |= PolicyService::KEY_INSTALL_NEW | PolicyService::KEY_CLOBBER;
  device_policy_->Store(
      policy_blob, flags, signature_check,
      base::Bind(HandlePolicyServiceCompletion, base::Passed(&response)));
}

void SessionManagerImpl::StorePolicyForUserInternal(
    const std::string& account_id,
    const std::vector<uint8_t>& policy_blob,
    SignatureCheck signature_check,
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response) {
  PolicyService* policy_service = GetPolicyService(account_id);
  if (!policy_service) {
    constexpr char kMessage[] =
        "Cannot store user policy before session is started.";
    LOG(ERROR) << kMessage;
    auto error = CreateError(dbus_error::kSessionDoesNotExist, kMessage);
    response->ReplyWithError(error.get());
    return;
  }

  policy_service->Store(
      policy_blob, PolicyService::KEY_INSTALL_NEW | PolicyService::KEY_ROTATE,
      signature_check,
      base::Bind(HandlePolicyServiceCompletion, base::Passed(&response)));
}

#if USE_CHEETS
std::string SessionManagerImpl::StartArcContainer(
    const std::string& init_signal,
    const std::vector<std::string>& init_keyvals,
    brillo::ErrorPtr* error_out) {
  if (!init_controller_->TriggerImpulse(
          init_signal, init_keyvals,
          InitDaemonController::TriggerMode::SYNC)) {
    const std::string message =
        "Emitting " + init_signal + " init signal failed.";
    LOG(ERROR) << message;
    *error_out = CreateError(dbus_error::kEmitFailed, message);
    return std::string();
  }

  // Container instance id needs to be valid ASCII/UTF-8, so encode as base64.
  std::string container_instance_id =
      base::RandBytesAsString(kArcContainerInstanceIdLength);
  base::Base64Encode(container_instance_id, &container_instance_id);

  if (!android_container_->StartContainer(
          base::Bind(&SessionManagerImpl::OnAndroidContainerStopped,
                     weak_ptr_factory_.GetWeakPtr(), container_instance_id))) {
    // Failed to start container. Thus, trigger stop-arc-instance init signal
    // manually for cleanup.
    init_controller_->TriggerImpulse(kArcStopSignal, {},
                                     InitDaemonController::TriggerMode::SYNC);
    constexpr char kMessage[] = "Starting Android container failed.";
    LOG(ERROR) << kMessage;
    *error_out = CreateError(dbus_error::kContainerStartupFail, kMessage);
    return std::string();
  }

  pid_t pid = 0;
  android_container_->GetContainerPID(&pid);
  LOG(INFO) << "Started Android container with PID " << pid;
  return container_instance_id;
}

bool SessionManagerImpl::StartArcNetwork(brillo::ErrorPtr* error_out) {
  base::FilePath root_path;
  pid_t pid = 0;
  if (!android_container_->GetRootFsPath(&root_path) ||
      !android_container_->GetContainerPID(&pid)) {
    constexpr char kMessage[] = "Getting Android container info failed.";
    LOG(ERROR) << kMessage;
    *error_out = CreateError(dbus_error::kContainerStartupFail, kMessage);
    return false;
  }

  // Tell init to configure the network.
  if (!init_controller_->TriggerImpulse(
          kArcNetworkStartSignal,
          {"CONTAINER_NAME=" + std::string(kArcContainerName),
           "CONTAINER_PATH=" + root_path.value(),
           "CONTAINER_PID=" + std::to_string(pid)},
          InitDaemonController::TriggerMode::SYNC)) {
    constexpr char kMessage[] =
        "Emitting start-arc-network init signal failed.";
    LOG(ERROR) << kMessage;
    *error_out = CreateError(dbus_error::kEmitFailed, kMessage);
    return false;
  }

  return true;
}

void SessionManagerImpl::OnAndroidContainerStopped(
    const std::string& container_instance_id, pid_t pid, bool clean) {
  if (clean) {
    LOG(INFO) << "Android Container with pid " << pid << " stopped";
  } else {
    LOG(ERROR) << "Android Container with pid " << pid << " crashed";
  }

  stop_arc_instance_closure_.Run();

  login_metrics_->StopTrackingArcUseTime();
  if (!init_controller_->TriggerImpulse(
          kArcStopSignal, {}, InitDaemonController::TriggerMode::SYNC)) {
    LOG(ERROR) << "Emitting stop-arc-instance init signal failed.";
  }

  if (!init_controller_->TriggerImpulse(
          kArcNetworkStopSignal, {}, InitDaemonController::TriggerMode::SYNC)) {
    LOG(ERROR) << "Emitting stop-arc-network init signal failed.";
  }

  dbus_emitter_->EmitSignalWithBoolAndString(
      kArcInstanceStopped, clean, container_instance_id);
}
#endif  // USE_CHEETS

}  // namespace login_manager
