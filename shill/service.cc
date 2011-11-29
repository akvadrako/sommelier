// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/service.h"

#include <time.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include <base/logging.h>
#include <base/memory/scoped_ptr.h>
#include <base/string_number_conversions.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/control_interface.h"
#include "shill/error.h"
#include "shill/manager.h"
#include "shill/profile.h"
#include "shill/property_accessor.h"
#include "shill/refptr_types.h"
#include "shill/service_dbus_adaptor.h"
#include "shill/store_interface.h"

using std::map;
using std::string;
using std::vector;

namespace shill {

const char Service::kCheckPortalAuto[] = "auto";
const char Service::kCheckPortalFalse[] = "false";
const char Service::kCheckPortalTrue[] = "true";

const int Service::kPriorityNone = 0;

const char Service::kStorageAutoConnect[] = "AutoConnect";
const char Service::kStorageCheckPortal[] = "CheckPortal";
const char Service::kStorageEapAnonymousIdentity[] = "EAP.AnonymousIdentity";
const char Service::kStorageEapCACert[] = "EAP.CACert";
const char Service::kStorageEapCACertID[] = "EAP.CACertID";
const char Service::kStorageEapCertID[] = "EAP.CertID";
const char Service::kStorageEapClientCert[] = "EAP.ClientCert";
const char Service::kStorageEapEap[] = "EAP.EAP";
const char Service::kStorageEapIdentity[] = "EAP.Identity";
const char Service::kStorageEapInnerEap[] = "EAP.InnerEAP";
const char Service::kStorageEapKeyID[] = "EAP.KeyID";
const char Service::kStorageEapKeyManagement[] = "EAP.KeyMgmt";
const char Service::kStorageEapPIN[] = "EAP.PIN";
const char Service::kStorageEapPassword[] = "EAP.Password";
const char Service::kStorageEapPrivateKey[] = "EAP.PrivateKey";
const char Service::kStorageEapPrivateKeyPassword[] = "EAP.PrivateKeyPassword";
const char Service::kStorageEapUseSystemCAs[] = "EAP.UseSystemCAs";
const char Service::kStorageFavorite[] = "Favorite";
const char Service::kStorageName[] = "Name";
const char Service::kStoragePriority[] = "Priority";
const char Service::kStorageProxyConfig[] = "ProxyConfig";
const char Service::kStorageSaveCredentials[] = "SaveCredentials";

// static
unsigned int Service::serial_number_ = 0;

Service::Service(ControlInterface *control_interface,
                 EventDispatcher *dispatcher,
                 Manager *manager,
                 Technology::Identifier technology)
    : state_(kStateUnknown),
      failure_(kFailureUnknown),
      auto_connect_(false),
      check_portal_(kCheckPortalAuto),
      connectable_(false),
      favorite_(false),
      priority_(kPriorityNone),
      security_level_(0),
      strength_(0),
      save_credentials_(true),
      technology_(technology),
      dispatcher_(dispatcher),
      unique_name_(base::UintToString(serial_number_++)),
      friendly_name_(unique_name_),
      available_(false),
      configured_(false),
      configuration_(NULL),
      connection_(NULL),
      adaptor_(control_interface->CreateServiceAdaptor(this)),
      manager_(manager) {

  store_.RegisterBool(flimflam::kAutoConnectProperty, &auto_connect_);

  // flimflam::kActivationStateProperty: Registered in CellularService
  // flimflam::kCellularApnProperty: Registered in CellularService
  // flimflam::kCellularLastGoodApnProperty: Registered in CellularService
  // flimflam::kNetworkTechnologyProperty: Registered in CellularService
  // flimflam::kOperatorNameProperty: DEPRECATED
  // flimflam::kOperatorCodeProperty: DEPRECATED
  // flimflam::kRoamingStateProperty: Registered in CellularService
  // flimflam::kServingOperatorProperty: Registered in CellularService
  // flimflam::kPaymentURLProperty: Registered in CellularService

  store_.RegisterString(flimflam::kCheckPortalProperty, &check_portal_);
  store_.RegisterConstBool(flimflam::kConnectableProperty, &connectable_);
  HelpRegisterDerivedString(flimflam::kDeviceProperty,
                            &Service::GetDeviceRpcId,
                            NULL);

  store_.RegisterString(flimflam::kEapIdentityProperty, &eap_.identity);
  store_.RegisterString(flimflam::kEAPEAPProperty, &eap_.eap);
  store_.RegisterString(flimflam::kEapPhase2AuthProperty, &eap_.inner_eap);
  store_.RegisterString(flimflam::kEapAnonymousIdentityProperty,
                        &eap_.anonymous_identity);
  store_.RegisterString(flimflam::kEAPClientCertProperty, &eap_.client_cert);
  store_.RegisterString(flimflam::kEAPCertIDProperty, &eap_.cert_id);
  store_.RegisterString(flimflam::kEapPrivateKeyProperty, &eap_.private_key);
  store_.RegisterString(flimflam::kEapPrivateKeyPasswordProperty,
                        &eap_.private_key_password);
  store_.RegisterString(flimflam::kEAPKeyIDProperty, &eap_.key_id);
  store_.RegisterString(flimflam::kEapCaCertProperty, &eap_.ca_cert);
  store_.RegisterString(flimflam::kEapCaCertIDProperty, &eap_.ca_cert_id);
  store_.RegisterString(flimflam::kEAPPINProperty, &eap_.pin);
  store_.RegisterString(flimflam::kEapPasswordProperty, &eap_.password);
  store_.RegisterString(flimflam::kEapKeyMgmtProperty, &eap_.key_management);
  store_.RegisterBool(flimflam::kEapUseSystemCAsProperty, &eap_.use_system_cas);

  store_.RegisterConstString(flimflam::kErrorProperty, &error_);
  store_.RegisterConstBool(flimflam::kFavoriteProperty, &favorite_);
  HelpRegisterDerivedBool(flimflam::kIsActiveProperty,
                          &Service::IsActive,
                          NULL);
  // flimflam::kModeProperty: Registered in WiFiService
  store_.RegisterConstString(flimflam::kNameProperty, &friendly_name_);
  // flimflam::kPassphraseProperty: Registered in WiFiService
  // flimflam::kPassphraseRequiredProperty: Registered in WiFiService
  store_.RegisterInt32(flimflam::kPriorityProperty, &priority_);
  HelpRegisterDerivedString(flimflam::kProfileProperty,
                            &Service::GetProfileRpcId,
                            NULL);
  store_.RegisterString(flimflam::kProxyConfigProperty, &proxy_config_);
  // TODO(cmasone): Create VPN Service with this property
  // store_.RegisterConstStringmap(flimflam::kProviderProperty, &provider_);

  store_.RegisterBool(flimflam::kSaveCredentialsProperty, &save_credentials_);
  HelpRegisterDerivedString(flimflam::kTypeProperty,
                            &Service::GetTechnologyString,
                            NULL);
  // flimflam::kSecurityProperty: Registered in WiFiService
  HelpRegisterDerivedString(flimflam::kStateProperty,
                            &Service::CalculateState,
                            NULL);
  // flimflam::kSignalStrengthProperty: Registered in WiFi/CellularService
  // flimflam::kWifiAuthMode: Registered in WiFiService
  // flimflam::kWifiHiddenSsid: Registered in WiFiService
  // flimflam::kWifiFrequency: Registered in WiFiService
  // flimflam::kWifiPhyMode: Registered in WiFiService
  // flimflam::kWifiHexSsid: Registered in WiFiService
  VLOG(2) << "Service initialized.";
}

Service::~Service() {}

void Service::ActivateCellularModem(const string &/*carrier*/, Error *error) {
  const string kMessage = "Service doesn't support cellular modem activation.";
  LOG(ERROR) << kMessage;
  CHECK(error);
  error->Populate(Error::kInvalidArguments, kMessage);
}

bool Service::TechnologyIs(const Technology::Identifier /*type*/) const {
  return false;
}

bool Service::IsActive(Error */*error*/) {
  return state_ != kStateUnknown &&
    state_ != kStateIdle &&
    state_ != kStateFailure;
}

void Service::SetState(ConnectState state) {
  LOG(INFO) << "In " << __func__ << "(): Service " << friendly_name_
            << " state " << ConnectStateToString(state_) << " -> "
            << ConnectStateToString(state);

  if (state == state_) {
    return;
  }

  state_ = state;
  if (state != kStateFailure) {
    failure_ = kFailureUnknown;
  }
  manager_->UpdateService(this);
  Error error;
  if (state == kStateConnected) {
    // TODO(quiche): After we have portal detection in place, CalculateState
    // should map kStateConnected to kStateReady. At that point, we should
    // remove this. crosbug.com/23318.
    adaptor_->EmitStringChanged(
        flimflam::kStateProperty, flimflam::kStateReady);
  }
  adaptor_->EmitStringChanged(flimflam::kStateProperty, CalculateState(&error));
}

void Service::SetFailure(ConnectFailure failure) {
  failure_ = failure;
  SetState(kStateFailure);
}

string Service::GetRpcIdentifier() const {
  return adaptor_->GetRpcIdentifier();
}

bool Service::IsLoadableFrom(StoreInterface *storage) const {
  return storage->ContainsGroup(GetStorageIdentifier());
}

bool Service::Load(StoreInterface *storage) {
  const string id = GetStorageIdentifier();
  if (!storage->ContainsGroup(id)) {
    LOG(WARNING) << "Service is not available in the persistent store: " << id;
    return false;
  }
  storage->GetBool(id, kStorageAutoConnect, &auto_connect_);
  storage->GetString(id, kStorageCheckPortal, &check_portal_);
  storage->GetBool(id, kStorageFavorite, &favorite_);
  storage->GetInt(id, kStoragePriority, &priority_);
  storage->GetString(id, kStorageProxyConfig, &proxy_config_);
  storage->GetBool(id, kStorageSaveCredentials, &save_credentials_);

  LoadEapCredentials(storage, id);

  // TODO(petkov): Load these:

  // "Name"
  // "WiFi.HiddenSSID"
  // "SSID"
  // "Failure"
  // "Modified"
  // "LastAttempt"
  // WiFiService: "Passphrase"
  // "APN"
  // "LastGoodAPN"

  favorite_ = true;

  return true;
}

void Service::Unload() {
  auto_connect_ = false;
  favorite_ = false;
  // TODO(pstew): Call a centralized function to purge all profile-set
  // state.  This should be called both from Load() and Unload() since
  // even in Load() profiles aren't cumulative -- they're exclusive.
  // crosbug.com/22946
}

bool Service::Save(StoreInterface *storage) {
  const string id = GetStorageIdentifier();

  // TODO(petkov): We could choose to simplify the saving code by removing most
  // conditionals thus saving even default values.
  if (favorite_) {
    storage->SetBool(id, kStorageAutoConnect, auto_connect_);
  }
  if (check_portal_ == kCheckPortalAuto) {
    storage->DeleteKey(id, kStorageCheckPortal);
  } else {
    storage->SetString(id, kStorageCheckPortal, check_portal_);
  }
  storage->SetBool(id, kStorageFavorite, favorite_);
  storage->SetString(id, kStorageName, friendly_name_);
  SaveString(storage, id, kStorageProxyConfig, proxy_config_, false, true);
  if (priority_ != kPriorityNone) {
    storage->SetInt(id, kStoragePriority, priority_);
  } else {
    storage->DeleteKey(id, kStoragePriority);
  }
  if (save_credentials_) {
    storage->DeleteKey(id, kStorageSaveCredentials);
  } else {
    storage->SetBool(id, kStorageSaveCredentials, false);
  }

  SaveEapCredentials(storage, id);

  // TODO(petkov): Save these:

  // "WiFi.HiddenSSID"
  // "SSID"
  // "Failure"
  // "Modified"
  // "LastAttempt"
  // WiFiService: "Passphrase"
  // "APN"
  // "LastGoodAPN"

  return true;
}

void Service::MakeFavorite() {
  if (favorite_) {
    // We do not want to clobber the value of auto_connect_ (it may
    // be user-set). So return early.
    return;
  }

  auto_connect_ = true;
  favorite_ = true;
}

// static
const char *Service::ConnectFailureToString(const ConnectFailure &state) {
  switch (state) {
    case kFailureUnknown:
      return "Unknown";
    case kFailureActivationFailure:
      return "Activation Failure";
    case kFailureOutOfRange:
      return "Out of range";
    case kFailurePinMissing:
      return "PIN missing";
    case kFailureConfigurationFailed:
      return "Configuration Failed";
    case kFailureBadCredentials:
      return "Bad Credentials";
    case kFailureNeedEVDO:
      return "Need EVDO";
    case kFailureNeedHomeNetwork:
      return "Need Home Network";
    case kFailureOTASPFailure:
      return "OTASP Failure";
    case kFailureAAAFailure:
      return "AAA Failure";
  }
  return "Invalid";
}

// static
const char *Service::ConnectStateToString(const ConnectState &state) {
  switch (state) {
    case kStateUnknown:
      return "Unknown";
    case kStateIdle:
      return "Idle";
    case kStateAssociating:
      return "Associating";
    case kStateConfiguring:
      return "Configuring";
    case kStateConnected:
      return "Connected";
    case kStateDisconnected:
      return "Disconnected";
    case kStateFailure:
      return "Failure";
    case kStateOnline:
      return "Online";
  }
  return "Invalid";
}


// static
string Service::GetTechnologyString(Error */*error*/) {
  return Technology::NameFromIdentifier(technology());
}

bool Service::DecideBetween(int a, int b, bool *decision) {
  if (a == b)
    return false;
  *decision = (a > b);
  return true;
}

// static
bool Service::Compare(ServiceRefPtr a,
                      ServiceRefPtr b,
                      const vector<Technology::Identifier> &tech_order) {
  bool ret;

  if (a->state() != b->state()) {
    if (DecideBetween(a->IsConnected(), b->IsConnected(), &ret)) {
      return ret;
    }

    // TODO(pstew): Services don't know about portal state yet

    if (DecideBetween(a->IsConnecting(), b->IsConnecting(), &ret)) {
      return ret;
    }
  }

  if (DecideBetween(a->favorite(), b->favorite(), &ret) ||
      DecideBetween(a->priority(), b->priority(), &ret)) {
    return ret;
  }

  // TODO(pstew): Below this point we are making value judgements on
  // services that are not related to anything intrinsic or
  // user-specified.  These heuristics should be richer (contain
  // historical information, for example) and be subject to user
  // customization.

  for (vector<Technology::Identifier>::const_iterator it = tech_order.begin();
       it != tech_order.end();
       ++it) {
    if (DecideBetween(a->TechnologyIs(*it), b->TechnologyIs(*it), &ret))
      return ret;
  }

  if (DecideBetween(a->security_level(), b->security_level(), &ret) ||
      DecideBetween(a->strength(), b->strength(), &ret)) {
    return ret;
  }

  return a->UniqueName() < b->UniqueName();
}

const ProfileRefPtr &Service::profile() const { return profile_; }

void Service::set_profile(const ProfileRefPtr &p) { profile_ = p; }

void Service::set_connectable(bool connectable) {
  connectable_ = connectable;
  adaptor_->EmitBoolChanged(flimflam::kConnectableProperty, connectable_);
}

string Service::CalculateState(Error */*error*/) {
  switch (state_) {
    case kStateIdle:
      return flimflam::kStateIdle;
    case kStateAssociating:
      return flimflam::kStateAssociation;
    case kStateConfiguring:
      return flimflam::kStateConfiguration;
    case kStateConnected:
      // TODO(gauravsh): Until portal handling is implemented, go to "online"
      // instead of "ready" state. crosbug.com/23318
      return flimflam::kStateOnline;
    case kStateDisconnected:
      return flimflam::kStateDisconnect;
    case kStateFailure:
      return flimflam::kStateFailure;
    case kStateOnline:
      return flimflam::kStateOnline;
    case kStateUnknown:
    default:
      return "";
  }
}

void Service::HelpRegisterDerivedBool(
    const string &name,
    bool(Service::*get)(Error *),
    void(Service::*set)(const bool&, Error *)) {
  store_.RegisterDerivedBool(
      name,
      BoolAccessor(new CustomAccessor<Service, bool>(this, get, set)));
}

void Service::HelpRegisterDerivedString(
    const string &name,
    string(Service::*get)(Error *),
    void(Service::*set)(const string&, Error *)) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(new CustomAccessor<Service, string>(this, get, set)));
}

void Service::SaveString(StoreInterface *storage,
                         const string &id,
                         const string &key,
                         const string &value,
                         bool crypted,
                         bool save) {
  if (value.empty() || !save) {
    storage->DeleteKey(id, key);
    return;
  }
  if (crypted) {
    storage->SetCryptedString(id, key, value);
    return;
  }
  storage->SetString(id, key, value);
}

void Service::LoadEapCredentials(StoreInterface *storage, const string &id) {
  storage->GetCryptedString(id, kStorageEapIdentity, &eap_.identity);
  storage->GetString(id, kStorageEapEap, &eap_.eap);
  storage->GetString(id, kStorageEapInnerEap, &eap_.inner_eap);
  storage->GetCryptedString(id,
                            kStorageEapAnonymousIdentity,
                            &eap_.anonymous_identity);
  storage->GetString(id, kStorageEapClientCert, &eap_.client_cert);
  storage->GetString(id, kStorageEapCertID, &eap_.cert_id);
  storage->GetString(id, kStorageEapPrivateKey, &eap_.private_key);
  storage->GetCryptedString(id,
                            kStorageEapPrivateKeyPassword,
                            &eap_.private_key_password);
  storage->GetString(id, kStorageEapKeyID, &eap_.key_id);
  storage->GetString(id, kStorageEapCACert, &eap_.ca_cert);
  storage->GetString(id, kStorageEapCACertID, &eap_.ca_cert_id);
  storage->GetBool(id, kStorageEapUseSystemCAs, &eap_.use_system_cas);
  storage->GetString(id, kStorageEapPIN, &eap_.pin);
  storage->GetCryptedString(id, kStorageEapPassword, &eap_.password);
  storage->GetString(id, kStorageEapKeyManagement, &eap_.key_management);
  // TODO(quiche): Update Connectable property. (crosbug.com/23466)
}

void Service::SaveEapCredentials(StoreInterface *storage, const string &id) {
  bool save = save_credentials_;
  SaveString(storage, id, kStorageEapIdentity, eap_.identity, true, save);
  SaveString(storage, id, kStorageEapEap, eap_.eap, false, true);
  SaveString(storage, id, kStorageEapInnerEap, eap_.inner_eap, false, true);
  SaveString(storage,
             id,
             kStorageEapAnonymousIdentity,
             eap_.anonymous_identity,
             true,
             save);
  SaveString(storage, id, kStorageEapClientCert, eap_.client_cert, false, save);
  SaveString(storage, id, kStorageEapCertID, eap_.cert_id, false, save);
  SaveString(storage, id, kStorageEapPrivateKey, eap_.private_key, false, save);
  SaveString(storage,
             id,
             kStorageEapPrivateKeyPassword,
             eap_.private_key_password,
             true,
             save);
  SaveString(storage, id, kStorageEapKeyID, eap_.key_id, false, save);
  SaveString(storage, id, kStorageEapCACert, eap_.ca_cert, false, true);
  SaveString(storage, id, kStorageEapCACertID, eap_.ca_cert_id, false, true);
  storage->SetBool(id, kStorageEapUseSystemCAs, eap_.use_system_cas);
  SaveString(storage, id, kStorageEapPIN, eap_.pin, false, save);
  SaveString(storage, id, kStorageEapPassword, eap_.password, true, save);
  SaveString(storage,
             id,
             kStorageEapKeyManagement,
             eap_.key_management,
             false,
             true);
}

const string &Service::GetEAPKeyManagement() const {
  return eap_.key_management;
}

void Service::SetEAPKeyManagement(const string &key_management) {
  eap_.key_management = key_management;
}

}  // namespace shill
