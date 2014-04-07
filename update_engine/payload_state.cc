// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/payload_state.h"

#include <algorithm>

#include <base/logging.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <base/format_macros.h>
#include <policy/device_policy.h>

#include "update_engine/clock.h"
#include "update_engine/constants.h"
#include "update_engine/hardware_interface.h"
#include "update_engine/install_plan.h"
#include "update_engine/omaha_request_params.h"
#include "update_engine/prefs.h"
#include "update_engine/real_dbus_wrapper.h"
#include "update_engine/system_state.h"
#include "update_engine/utils.h"

using base::Time;
using base::TimeDelta;
using std::min;
using std::string;

namespace chromeos_update_engine {

const TimeDelta PayloadState::kDurationSlack = TimeDelta::FromSeconds(600);

// We want to upperbound backoffs to 16 days
static const int kMaxBackoffDays = 16;

// We want to randomize retry attempts after the backoff by +/- 6 hours.
static const uint32_t kMaxBackoffFuzzMinutes = 12 * 60;

PayloadState::PayloadState()
    : prefs_(NULL),
      using_p2p_for_downloading_(false),
      payload_attempt_number_(0),
      full_payload_attempt_number_(0),
      url_index_(0),
      url_failure_count_(0),
      url_switch_count_(0),
      p2p_num_attempts_(0) {
 for (int i = 0; i <= kNumDownloadSources; i++)
  total_bytes_downloaded_[i] = current_bytes_downloaded_[i] = 0;
}

bool PayloadState::Initialize(SystemState* system_state) {
  system_state_ = system_state;
  prefs_ = system_state_->prefs();
  powerwash_safe_prefs_ = system_state_->powerwash_safe_prefs();
  LoadResponseSignature();
  LoadPayloadAttemptNumber();
  LoadFullPayloadAttemptNumber();
  LoadUrlIndex();
  LoadUrlFailureCount();
  LoadUrlSwitchCount();
  LoadBackoffExpiryTime();
  LoadUpdateTimestampStart();
  // The LoadUpdateDurationUptime() method relies on LoadUpdateTimestampStart()
  // being called before it. Don't reorder.
  LoadUpdateDurationUptime();
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    LoadCurrentBytesDownloaded(source);
    LoadTotalBytesDownloaded(source);
  }
  LoadNumReboots();
  LoadNumResponsesSeen();
  LoadRollbackVersion();
  LoadP2PFirstAttemptTimestamp();
  LoadP2PNumAttempts();
  return true;
}

void PayloadState::SetResponse(const OmahaResponse& omaha_response) {
  // Always store the latest response.
  response_ = omaha_response;

  // Compute the candidate URLs first as they are used to calculate the
  // response signature so that a change in enterprise policy for
  // HTTP downloads being enabled or not could be honored as soon as the
  // next update check happens.
  ComputeCandidateUrls();

  // Check if the "signature" of this response (i.e. the fields we care about)
  // has changed.
  string new_response_signature = CalculateResponseSignature();
  bool has_response_changed = (response_signature_ != new_response_signature);

  // If the response has changed, we should persist the new signature and
  // clear away all the existing state.
  if (has_response_changed) {
    LOG(INFO) << "Resetting all persisted state as this is a new response";
    SetNumResponsesSeen(num_responses_seen_ + 1);
    SetResponseSignature(new_response_signature);
    ResetPersistedState();
    ReportUpdatesAbandonedEventCountMetric();
    return;
  }

  // This is the earliest point at which we can validate whether the URL index
  // we loaded from the persisted state is a valid value. If the response
  // hasn't changed but the URL index is invalid, it's indicative of some
  // tampering of the persisted state.
  if (static_cast<uint32_t>(url_index_) >= candidate_urls_.size()) {
    LOG(INFO) << "Resetting all payload state as the url index seems to have "
                 "been tampered with";
    ResetPersistedState();
    return;
  }

  // Update the current download source which depends on the latest value of
  // the response.
  UpdateCurrentDownloadSource();
}

void PayloadState::SetUsingP2PForDownloading(bool value) {
  using_p2p_for_downloading_ = value;
  // Update the current download source which depends on whether we are
  // using p2p or not.
  UpdateCurrentDownloadSource();
}

void PayloadState::DownloadComplete() {
  LOG(INFO) << "Payload downloaded successfully";
  IncrementPayloadAttemptNumber();
  IncrementFullPayloadAttemptNumber();
}

void PayloadState::DownloadProgress(size_t count) {
  if (count == 0)
    return;

  CalculateUpdateDurationUptime();
  UpdateBytesDownloaded(count);

  // We've received non-zero bytes from a recent download operation.  Since our
  // URL failure count is meant to penalize a URL only for consecutive
  // failures, downloading bytes successfully means we should reset the failure
  // count (as we know at least that the URL is working). In future, we can
  // design this to be more sophisticated to check for more intelligent failure
  // patterns, but right now, even 1 byte downloaded will mark the URL to be
  // good unless it hits 10 (or configured number of) consecutive failures
  // again.

  if (GetUrlFailureCount() == 0)
    return;

  LOG(INFO) << "Resetting failure count of Url" << GetUrlIndex()
            << " to 0 as we received " << count << " bytes successfully";
  SetUrlFailureCount(0);
}

void PayloadState::AttemptStarted() {
  ClockInterface *clock = system_state_->clock();
  attempt_start_time_boot_ = clock->GetBootTime();
  attempt_start_time_monotonic_ = clock->GetMonotonicTime();
  attempt_num_bytes_downloaded_ = 0;

  metrics::ConnectionType type;
  NetworkConnectionType network_connection_type;
  NetworkTethering tethering;
  RealDBusWrapper dbus_iface;
  ConnectionManager* connection_manager = system_state_->connection_manager();
  if (!connection_manager->GetConnectionProperties(&dbus_iface,
                                                   &network_connection_type,
                                                   &tethering)) {
    LOG(ERROR) << "Failed to determine connection type.";
    type = metrics::ConnectionType::kUnknown;
  } else {
    type = utils::GetConnectionType(network_connection_type, tethering);
  }
  attempt_connection_type_ = type;
}

void PayloadState::UpdateResumed() {
  LOG(INFO) << "Resuming an update that was previously started.";
  UpdateNumReboots();
  AttemptStarted();
}

void PayloadState::UpdateRestarted() {
  LOG(INFO) << "Starting a new update";
  ResetDownloadSourcesOnNewUpdate();
  SetNumReboots(0);
  AttemptStarted();
}

void PayloadState::UpdateSucceeded() {
  // Send the relevant metrics that are tracked in this class to UMA.
  CalculateUpdateDurationUptime();
  SetUpdateTimestampEnd(system_state_->clock()->GetWallclockTime());

  CollectAndReportAttemptMetrics(kErrorCodeSuccess);
  CollectAndReportSuccessfulUpdateMetrics();

  // Reset the number of responses seen since it counts from the last
  // successful update, e.g. now.
  SetNumResponsesSeen(0);

  CreateSystemUpdatedMarkerFile();
}

void PayloadState::UpdateFailed(ErrorCode error) {
  ErrorCode base_error = utils::GetBaseErrorCode(error);
  LOG(INFO) << "Updating payload state for error code: " << base_error
            << " (" << utils::CodeToString(base_error) << ")";

  if (candidate_urls_.size() == 0) {
    // This means we got this error even before we got a valid Omaha response
    // or don't have any valid candidates in the Omaha response.
    // So we should not advance the url_index_ in such cases.
    LOG(INFO) << "Ignoring failures until we get a valid Omaha response.";
    return;
  }

  CollectAndReportAttemptMetrics(base_error);

  switch (base_error) {
    // Errors which are good indicators of a problem with a particular URL or
    // the protocol used in the URL or entities in the communication channel
    // (e.g. proxies). We should try the next available URL in the next update
    // check to quickly recover from these errors.
    case kErrorCodePayloadHashMismatchError:
    case kErrorCodePayloadSizeMismatchError:
    case kErrorCodeDownloadPayloadVerificationError:
    case kErrorCodeDownloadPayloadPubKeyVerificationError:
    case kErrorCodeSignedDeltaPayloadExpectedError:
    case kErrorCodeDownloadInvalidMetadataMagicString:
    case kErrorCodeDownloadSignatureMissingInManifest:
    case kErrorCodeDownloadManifestParseError:
    case kErrorCodeDownloadMetadataSignatureError:
    case kErrorCodeDownloadMetadataSignatureVerificationError:
    case kErrorCodeDownloadMetadataSignatureMismatch:
    case kErrorCodeDownloadOperationHashVerificationError:
    case kErrorCodeDownloadOperationExecutionError:
    case kErrorCodeDownloadOperationHashMismatch:
    case kErrorCodeDownloadInvalidMetadataSize:
    case kErrorCodeDownloadInvalidMetadataSignature:
    case kErrorCodeDownloadOperationHashMissingError:
    case kErrorCodeDownloadMetadataSignatureMissingError:
    case kErrorCodePayloadMismatchedType:
    case kErrorCodeUnsupportedMajorPayloadVersion:
    case kErrorCodeUnsupportedMinorPayloadVersion:
      IncrementUrlIndex();
      break;

    // Errors which seem to be just transient network/communication related
    // failures and do not indicate any inherent problem with the URL itself.
    // So, we should keep the current URL but just increment the
    // failure count to give it more chances. This way, while we maximize our
    // chances of downloading from the URLs that appear earlier in the response
    // (because download from a local server URL that appears earlier in a
    // response is preferable than downloading from the next URL which could be
    // a internet URL and thus could be more expensive).
    case kErrorCodeError:
    case kErrorCodeDownloadTransferError:
    case kErrorCodeDownloadWriteError:
    case kErrorCodeDownloadStateInitializationError:
    case kErrorCodeOmahaErrorInHTTPResponse: // Aggregate code for HTTP errors.
      IncrementFailureCount();
      break;

    // Errors which are not specific to a URL and hence shouldn't result in
    // the URL being penalized. This can happen in two cases:
    // 1. We haven't started downloading anything: These errors don't cost us
    // anything in terms of actual payload bytes, so we should just do the
    // regular retries at the next update check.
    // 2. We have successfully downloaded the payload: In this case, the
    // payload attempt number would have been incremented and would take care
    // of the backoff at the next update check.
    // In either case, there's no need to update URL index or failure count.
    case kErrorCodeOmahaRequestError:
    case kErrorCodeOmahaResponseHandlerError:
    case kErrorCodePostinstallRunnerError:
    case kErrorCodeFilesystemCopierError:
    case kErrorCodeInstallDeviceOpenError:
    case kErrorCodeKernelDeviceOpenError:
    case kErrorCodeDownloadNewPartitionInfoError:
    case kErrorCodeNewRootfsVerificationError:
    case kErrorCodeNewKernelVerificationError:
    case kErrorCodePostinstallBootedFromFirmwareB:
    case kErrorCodePostinstallFirmwareRONotUpdatable:
    case kErrorCodeOmahaRequestEmptyResponseError:
    case kErrorCodeOmahaRequestXMLParseError:
    case kErrorCodeOmahaResponseInvalid:
    case kErrorCodeOmahaUpdateIgnoredPerPolicy:
    case kErrorCodeOmahaUpdateDeferredPerPolicy:
    case kErrorCodeOmahaUpdateDeferredForBackoff:
    case kErrorCodePostinstallPowerwashError:
    case kErrorCodeUpdateCanceledByChannelChange:
      LOG(INFO) << "Not incrementing URL index or failure count for this error";
      break;

    case kErrorCodeSuccess:                            // success code
    case kErrorCodeUmaReportedMax:                     // not an error code
    case kErrorCodeOmahaRequestHTTPResponseBase:       // aggregated already
    case kErrorCodeDevModeFlag:                       // not an error code
    case kErrorCodeResumedFlag:                        // not an error code
    case kErrorCodeTestImageFlag:                      // not an error code
    case kErrorCodeTestOmahaUrlFlag:                   // not an error code
    case kErrorCodeSpecialFlags:                       // not an error code
      // These shouldn't happen. Enumerating these  explicitly here so that we
      // can let the compiler warn about new error codes that are added to
      // action_processor.h but not added here.
      LOG(WARNING) << "Unexpected error code for UpdateFailed";
      break;

    // Note: Not adding a default here so as to let the compiler warn us of
    // any new enums that were added in the .h but not listed in this switch.
  }
}

bool PayloadState::ShouldBackoffDownload() {
  if (response_.disable_payload_backoff) {
    LOG(INFO) << "Payload backoff logic is disabled. "
                 "Can proceed with the download";
    return false;
  }
  if (system_state_->request_params()->use_p2p_for_downloading() &&
      !system_state_->request_params()->p2p_url().empty()) {
    LOG(INFO) << "Payload backoff logic is disabled because download "
              << "will happen from local peer (via p2p).";
    return false;
  }
  if (system_state_->request_params()->interactive()) {
    LOG(INFO) << "Payload backoff disabled for interactive update checks.";
    return false;
  }
  if (response_.is_delta_payload) {
    // If delta payloads fail, we want to fallback quickly to full payloads as
    // they are more likely to succeed. Exponential backoffs would greatly
    // slow down the fallback to full payloads.  So we don't backoff for delta
    // payloads.
    LOG(INFO) << "No backoffs for delta payloads. "
              << "Can proceed with the download";
    return false;
  }

  if (!system_state_->hardware()->IsOfficialBuild()) {
    // Backoffs are needed only for official builds. We do not want any delays
    // or update failures due to backoffs during testing or development.
    LOG(INFO) << "No backoffs for test/dev images. "
              << "Can proceed with the download";
    return false;
  }

  if (backoff_expiry_time_.is_null()) {
    LOG(INFO) << "No backoff expiry time has been set. "
              << "Can proceed with the download";
    return false;
  }

  if (backoff_expiry_time_ < Time::Now()) {
    LOG(INFO) << "The backoff expiry time ("
              << utils::ToString(backoff_expiry_time_)
              << ") has elapsed. Can proceed with the download";
    return false;
  }

  LOG(INFO) << "Cannot proceed with downloads as we need to backoff until "
            << utils::ToString(backoff_expiry_time_);
  return true;
}

void PayloadState::Rollback() {
  SetRollbackVersion(system_state_->request_params()->app_version());
}

void PayloadState::IncrementPayloadAttemptNumber() {
  // Update the payload attempt number for both payload types: full and delta.
  SetPayloadAttemptNumber(GetPayloadAttemptNumber() + 1);

  // Report the metric every time the value is incremented.
  string metric = "Installer.PayloadAttemptNumber";
  int value = GetPayloadAttemptNumber();

  LOG(INFO) << "Uploading " << value << " (count) for metric " <<  metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       value,
       1,    // min value
       50,   // max value
       kNumDefaultUmaBuckets);
}

void PayloadState::IncrementFullPayloadAttemptNumber() {
  // Update the payload attempt number for full payloads and the backoff time.
  if (response_.is_delta_payload) {
    LOG(INFO) << "Not incrementing payload attempt number for delta payloads";
    return;
  }

  LOG(INFO) << "Incrementing the full payload attempt number";
  SetFullPayloadAttemptNumber(GetFullPayloadAttemptNumber() + 1);
  UpdateBackoffExpiryTime();

  // Report the metric every time the value is incremented.
  string metric = "Installer.FullPayloadAttemptNumber";
  int value = GetFullPayloadAttemptNumber();

  LOG(INFO) << "Uploading " << value << " (count) for metric " <<  metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       value,
       1,    // min value
       50,   // max value
       kNumDefaultUmaBuckets);
}

void PayloadState::IncrementUrlIndex() {
  uint32_t next_url_index = GetUrlIndex() + 1;
  if (next_url_index < candidate_urls_.size()) {
    LOG(INFO) << "Incrementing the URL index for next attempt";
    SetUrlIndex(next_url_index);
  } else {
    LOG(INFO) << "Resetting the current URL index (" << GetUrlIndex() << ") to "
              << "0 as we only have " << candidate_urls_.size()
              << " candidate URL(s)";
    SetUrlIndex(0);
    IncrementPayloadAttemptNumber();
    IncrementFullPayloadAttemptNumber();
  }

  // If we have multiple URLs, record that we just switched to another one
  if (candidate_urls_.size() > 1)
    SetUrlSwitchCount(url_switch_count_ + 1);

  // Whenever we update the URL index, we should also clear the URL failure
  // count so we can start over fresh for the new URL.
  SetUrlFailureCount(0);
}

void PayloadState::IncrementFailureCount() {
  uint32_t next_url_failure_count = GetUrlFailureCount() + 1;
  if (next_url_failure_count < response_.max_failure_count_per_url) {
    LOG(INFO) << "Incrementing the URL failure count";
    SetUrlFailureCount(next_url_failure_count);
  } else {
    LOG(INFO) << "Reached max number of failures for Url" << GetUrlIndex()
              << ". Trying next available URL";
    IncrementUrlIndex();
  }
}

void PayloadState::UpdateBackoffExpiryTime() {
  if (response_.disable_payload_backoff) {
    LOG(INFO) << "Resetting backoff expiry time as payload backoff is disabled";
    SetBackoffExpiryTime(Time());
    return;
  }

  if (GetFullPayloadAttemptNumber() == 0) {
    SetBackoffExpiryTime(Time());
    return;
  }

  // Since we're doing left-shift below, make sure we don't shift more
  // than this. E.g. if int is 4-bytes, don't left-shift more than 30 bits,
  // since we don't expect value of kMaxBackoffDays to be more than 100 anyway.
  int num_days = 1; // the value to be shifted.
  const int kMaxShifts = (sizeof(num_days) * 8) - 2;

  // Normal backoff days is 2 raised to (payload_attempt_number - 1).
  // E.g. if payload_attempt_number is over 30, limit power to 30.
  int power = min(GetFullPayloadAttemptNumber() - 1, kMaxShifts);

  // The number of days is the minimum of 2 raised to (payload_attempt_number
  // - 1) or kMaxBackoffDays.
  num_days = min(num_days << power, kMaxBackoffDays);

  // We don't want all retries to happen exactly at the same time when
  // retrying after backoff. So add some random minutes to fuzz.
  int fuzz_minutes = utils::FuzzInt(0, kMaxBackoffFuzzMinutes);
  TimeDelta next_backoff_interval = TimeDelta::FromDays(num_days) +
                                    TimeDelta::FromMinutes(fuzz_minutes);
  LOG(INFO) << "Incrementing the backoff expiry time by "
            << utils::FormatTimeDelta(next_backoff_interval);
  SetBackoffExpiryTime(Time::Now() + next_backoff_interval);
}

void PayloadState::UpdateCurrentDownloadSource() {
  current_download_source_ = kNumDownloadSources;

  if (using_p2p_for_downloading_) {
    current_download_source_ = kDownloadSourceHttpPeer;
  } else if (GetUrlIndex() < candidate_urls_.size())  {
    string current_url = candidate_urls_[GetUrlIndex()];
    if (StartsWithASCII(current_url, "https://", false))
      current_download_source_ = kDownloadSourceHttpsServer;
    else if (StartsWithASCII(current_url, "http://", false))
      current_download_source_ = kDownloadSourceHttpServer;
  }

  LOG(INFO) << "Current download source: "
            << utils::ToString(current_download_source_);
}

void PayloadState::UpdateBytesDownloaded(size_t count) {
  SetCurrentBytesDownloaded(
      current_download_source_,
      GetCurrentBytesDownloaded(current_download_source_) + count,
      false);
  SetTotalBytesDownloaded(
      current_download_source_,
      GetTotalBytesDownloaded(current_download_source_) + count,
      false);

  attempt_num_bytes_downloaded_ += count;
}

PayloadType PayloadState::CalculatePayloadType() {
  PayloadType payload_type;
  OmahaRequestParams* params = system_state_->request_params();
  if (response_.is_delta_payload) {
    payload_type = kPayloadTypeDelta;
  } else if (params->delta_okay()) {
    payload_type = kPayloadTypeFull;
  } else {  // Full payload, delta was not allowed by request.
    payload_type = kPayloadTypeForcedFull;
  }
  return payload_type;
}

// TODO(zeuthen): Currently we don't report the UpdateEngine.Attempt.*
// metrics if the attempt ends abnormally, e.g. if the update_engine
// process crashes or the device is rebooted. See
// http://crbug.com/357676
void PayloadState::CollectAndReportAttemptMetrics(ErrorCode code) {
  int attempt_number = GetPayloadAttemptNumber();

  PayloadType payload_type = CalculatePayloadType();

  int64_t payload_size = response_.size;

  int64_t payload_bytes_downloaded = attempt_num_bytes_downloaded_;

  ClockInterface *clock = system_state_->clock();
  base::TimeDelta duration = clock->GetBootTime() - attempt_start_time_boot_;
  base::TimeDelta duration_uptime = clock->GetMonotonicTime() -
      attempt_start_time_monotonic_;

  int64_t payload_download_speed_bps = 0;
  int64_t usec = duration_uptime.InMicroseconds();
  if (usec > 0) {
    double sec = static_cast<double>(usec) / Time::kMicrosecondsPerSecond;
    double bps = static_cast<double>(payload_bytes_downloaded) / sec;
    payload_download_speed_bps = static_cast<int64_t>(bps);
  }

  DownloadSource download_source = current_download_source_;

  metrics::DownloadErrorCode payload_download_error_code =
    metrics::DownloadErrorCode::kUnset;
  ErrorCode internal_error_code = kErrorCodeSuccess;
  metrics::AttemptResult attempt_result = utils::GetAttemptResult(code);

  // Add additional detail to AttemptResult
  switch (attempt_result) {
    case metrics::AttemptResult::kPayloadDownloadError:
      payload_download_error_code = utils::GetDownloadErrorCode(code);
      break;

    case metrics::AttemptResult::kInternalError:
      internal_error_code = code;
      break;

    // Explicit fall-through for cases where we do not have additional
    // detail. We avoid the default keyword to force people adding new
    // AttemptResult values to visit this code and examine whether
    // additional detail is needed.
    case metrics::AttemptResult::kUpdateSucceeded:
    case metrics::AttemptResult::kMetadataMalformed:
    case metrics::AttemptResult::kOperationMalformed:
    case metrics::AttemptResult::kOperationExecutionError:
    case metrics::AttemptResult::kMetadataVerificationFailed:
    case metrics::AttemptResult::kPayloadVerificationFailed:
    case metrics::AttemptResult::kVerificationFailed:
    case metrics::AttemptResult::kPostInstallFailed:
    case metrics::AttemptResult::kAbnormalTermination:
    case metrics::AttemptResult::kNumConstants:
    case metrics::AttemptResult::kUnset:
      break;
  }

  metrics::ReportUpdateAttemptMetrics(system_state_,
                                      attempt_number,
                                      payload_type,
                                      duration,
                                      duration_uptime,
                                      payload_size,
                                      payload_bytes_downloaded,
                                      payload_download_speed_bps,
                                      download_source,
                                      attempt_result,
                                      internal_error_code,
                                      payload_download_error_code,
                                      attempt_connection_type_);
}

void PayloadState::CollectAndReportSuccessfulUpdateMetrics() {
  string metric;

  // Report metrics collected from all known download sources to UMA.
  int64_t successful_bytes_by_source[kNumDownloadSources];
  int64_t total_bytes_by_source[kNumDownloadSources];
  int64_t successful_bytes = 0;
  int64_t total_bytes = 0;
  int64_t successful_mbs = 0;
  int64_t total_mbs = 0;

  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    int64_t bytes;

    // Only consider this download source (and send byte counts) as
    // having been used if we downloaded a non-trivial amount of bytes
    // (e.g. at least 1 MiB) that contributed to the final success of
    // the update. Otherwise we're going to end up with a lot of
    // zero-byte events in the histogram.

    bytes = GetCurrentBytesDownloaded(source);
    successful_bytes_by_source[i] = bytes;
    successful_bytes += bytes;
    successful_mbs += bytes / kNumBytesInOneMiB;
    SetCurrentBytesDownloaded(source, 0, true);

    bytes = GetTotalBytesDownloaded(source);
    total_bytes_by_source[i] = bytes;
    total_bytes += bytes;
    total_mbs += bytes / kNumBytesInOneMiB;
    SetTotalBytesDownloaded(source, 0, true);
  }

  int download_overhead_percentage = 0;
  if (successful_bytes > 0) {
    download_overhead_percentage = (total_bytes - successful_bytes) * 100ULL /
                                   successful_bytes;
  }

  int url_switch_count = static_cast<int>(url_switch_count_);

  int reboot_count = GetNumReboots();

  SetNumReboots(0);

  TimeDelta duration = GetUpdateDuration();
  TimeDelta duration_uptime = GetUpdateDurationUptime();

  prefs_->Delete(kPrefsUpdateTimestampStart);
  prefs_->Delete(kPrefsUpdateDurationUptime);

  PayloadType payload_type = CalculatePayloadType();

  int64_t payload_size = response_.size;

  int attempt_count = GetPayloadAttemptNumber();

  int updates_abandoned_count = num_responses_seen_ - 1;

  metrics::ReportSuccessfulUpdateMetrics(system_state_,
                                         attempt_count,
                                         updates_abandoned_count,
                                         payload_type,
                                         payload_size,
                                         total_bytes_by_source,
                                         download_overhead_percentage,
                                         duration,
                                         reboot_count,
                                         url_switch_count);

  // TODO(zeuthen): This is the old metric reporting code which is
  // slated for removal soon. See http://crbug.com/355745 for details.

  // The old metrics code is using MiB's instead of bytes to calculate
  // the overhead which due to rounding makes the numbers slightly
  // different.
  download_overhead_percentage = 0;
  if (successful_mbs > 0) {
    download_overhead_percentage = (total_mbs - successful_mbs) * 100ULL /
                                   successful_mbs;
  }

  int download_sources_used = 0;
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    const int kMaxMiBs = 10240;  // Anything above 10GB goes in the last bucket.
    int64_t mbs;

    // Only consider this download source (and send byte counts) as
    // having been used if we downloaded a non-trivial amount of bytes
    // (e.g. at least 1 MiB) that contributed to the final success of
    // the update. Otherwise we're going to end up with a lot of
    // zero-byte events in the histogram.

    mbs = successful_bytes_by_source[i] / kNumBytesInOneMiB;
    if (mbs > 0) {
      metric = "Installer.SuccessfulMBsDownloadedFrom" +
          utils::ToString(source);
      LOG(INFO) << "Uploading " << mbs << " (MBs) for metric " << metric;
      system_state_->metrics_lib()->SendToUMA(metric,
                                              mbs,
                                              0,  // min
                                              kMaxMiBs,
                                              kNumDefaultUmaBuckets);
    }

    mbs = total_bytes_by_source[i] / kNumBytesInOneMiB;
    if (mbs > 0) {
      metric = "Installer.TotalMBsDownloadedFrom" + utils::ToString(source);
      LOG(INFO) << "Uploading " << mbs << " (MBs) for metric " << metric;
      system_state_->metrics_lib()->SendToUMA(metric,
                                              mbs,
                                              0,  // min
                                              kMaxMiBs,
                                              kNumDefaultUmaBuckets);
      download_sources_used |= (1 << i);
    }
  }

  metric = "Installer.DownloadSourcesUsed";
  LOG(INFO) << "Uploading 0x" << std::hex << download_sources_used
            << " (bit flags) for metric " << metric;
  int num_buckets = std::min(1 << kNumDownloadSources, kNumDefaultUmaBuckets);
  system_state_->metrics_lib()->SendToUMA(metric,
                                          download_sources_used,
                                          0,  // min
                                          1 << kNumDownloadSources,
                                          num_buckets);

  metric = "Installer.DownloadOverheadPercentage";
  LOG(INFO) << "Uploading " << download_overhead_percentage
            << "% for metric " << metric;
  system_state_->metrics_lib()->SendToUMA(metric,
                                          download_overhead_percentage,
                                          0,     // min: 0% overhead
                                          1000,  // max: 1000% overhead
                                          kNumDefaultUmaBuckets);

  metric = "Installer.UpdateURLSwitches";
  LOG(INFO) << "Uploading " << url_switch_count
            << " (count) for metric " << metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       url_switch_count,
       0,    // min value
       100,  // max value
       kNumDefaultUmaBuckets);

  metric = "Installer.UpdateNumReboots";
  LOG(INFO) << "Uploading reboot count of " << reboot_count << " for metric "
            <<  metric;
  system_state_->metrics_lib()->SendToUMA(
      metric,
      reboot_count,  // sample
      0,    // min = 0.
      50,   // max
      25);  // buckets

  metric = "Installer.UpdateDurationMinutes";
  system_state_->metrics_lib()->SendToUMA(
       metric,
       static_cast<int>(duration.InMinutes()),
       1,             // min: 1 minute
       365*24*60,     // max: 1 year (approx)
       kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(duration)
            << " for metric " <<  metric;

  metric = "Installer.UpdateDurationUptimeMinutes";
  system_state_->metrics_lib()->SendToUMA(
       metric,
       static_cast<int>(duration_uptime.InMinutes()),
       1,             // min: 1 minute
       30*24*60,      // max: 1 month (approx)
       kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(duration_uptime)
            << " for metric " <<  metric;

  metric = "Installer.PayloadFormat";
  system_state_->metrics_lib()->SendEnumToUMA(
      metric,
      payload_type,
      kNumPayloadTypes);
  LOG(INFO) << "Uploading " << utils::ToString(payload_type)
            << " for metric " <<  metric;

  metric = "Installer.AttemptsCount.Total";
  system_state_->metrics_lib()->SendToUMA(
       metric,
       attempt_count,
       1,      // min
       50,     // max
       kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << attempt_count
            << " for metric " <<  metric;

  metric = "Installer.UpdatesAbandonedCount";
  LOG(INFO) << "Uploading " << updates_abandoned_count
            << " (count) for metric " <<  metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       updates_abandoned_count,
       0,    // min value
       100,  // max value
       kNumDefaultUmaBuckets);
}

void PayloadState::UpdateNumReboots() {
  // We only update the reboot count when the system has been detected to have
  // been rebooted.
  if (!system_state_->system_rebooted()) {
    return;
  }

  SetNumReboots(GetNumReboots() + 1);
}

void PayloadState::SetNumReboots(uint32_t num_reboots) {
  CHECK(prefs_);
  num_reboots_ = num_reboots;
  prefs_->SetInt64(kPrefsNumReboots, num_reboots);
  LOG(INFO) << "Number of Reboots during current update attempt = "
            << num_reboots_;
}

void PayloadState::ResetPersistedState() {
  SetPayloadAttemptNumber(0);
  SetFullPayloadAttemptNumber(0);
  SetUrlIndex(0);
  SetUrlFailureCount(0);
  SetUrlSwitchCount(0);
  UpdateBackoffExpiryTime(); // This will reset the backoff expiry time.
  SetUpdateTimestampStart(system_state_->clock()->GetWallclockTime());
  SetUpdateTimestampEnd(Time()); // Set to null time
  SetUpdateDurationUptime(TimeDelta::FromSeconds(0));
  ResetDownloadSourcesOnNewUpdate();
  ResetRollbackVersion();
  SetP2PNumAttempts(0);
  SetP2PFirstAttemptTimestamp(Time()); // Set to null time
}

void PayloadState::ResetRollbackVersion() {
  CHECK(powerwash_safe_prefs_);
  rollback_version_ = "";
  powerwash_safe_prefs_->Delete(kPrefsRollbackVersion);
}

void PayloadState::ResetDownloadSourcesOnNewUpdate() {
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    SetCurrentBytesDownloaded(source, 0, true);
    // Note: Not resetting the TotalBytesDownloaded as we want that metric
    // to count the bytes downloaded across various update attempts until
    // we have successfully applied the update.
  }
}

int64_t PayloadState::GetPersistedValue(const string& key) {
  CHECK(prefs_);
  if (!prefs_->Exists(key))
    return 0;

  int64_t stored_value;
  if (!prefs_->GetInt64(key, &stored_value))
    return 0;

  if (stored_value < 0) {
    LOG(ERROR) << key << ": Invalid value (" << stored_value
               << ") in persisted state. Defaulting to 0";
    return 0;
  }

  return stored_value;
}

string PayloadState::CalculateResponseSignature() {
  string response_sign = base::StringPrintf(
      "NumURLs = %d\n", static_cast<int>(candidate_urls_.size()));

  for (size_t i = 0; i < candidate_urls_.size(); i++)
    response_sign += base::StringPrintf("Candidate Url%d = %s\n",
                                        static_cast<int>(i),
                                        candidate_urls_[i].c_str());

  response_sign += base::StringPrintf(
      "Payload Size = %ju\n"
      "Payload Sha256 Hash = %s\n"
      "Metadata Size = %ju\n"
      "Metadata Signature = %s\n"
      "Is Delta Payload = %d\n"
      "Max Failure Count Per Url = %d\n"
      "Disable Payload Backoff = %d\n",
      static_cast<uintmax_t>(response_.size),
      response_.hash.c_str(),
      static_cast<uintmax_t>(response_.metadata_size),
      response_.metadata_signature.c_str(),
      response_.is_delta_payload,
      response_.max_failure_count_per_url,
      response_.disable_payload_backoff);
  return response_sign;
}

void PayloadState::LoadResponseSignature() {
  CHECK(prefs_);
  string stored_value;
  if (prefs_->Exists(kPrefsCurrentResponseSignature) &&
      prefs_->GetString(kPrefsCurrentResponseSignature, &stored_value)) {
    SetResponseSignature(stored_value);
  }
}

void PayloadState::SetResponseSignature(const string& response_signature) {
  CHECK(prefs_);
  response_signature_ = response_signature;
  LOG(INFO) << "Current Response Signature = \n" << response_signature_;
  prefs_->SetString(kPrefsCurrentResponseSignature, response_signature_);
}

void PayloadState::LoadPayloadAttemptNumber() {
  SetPayloadAttemptNumber(GetPersistedValue(kPrefsPayloadAttemptNumber));
}

void PayloadState::LoadFullPayloadAttemptNumber() {
  SetFullPayloadAttemptNumber(GetPersistedValue(
      kPrefsFullPayloadAttemptNumber));
}

void PayloadState::SetPayloadAttemptNumber(int payload_attempt_number) {
  CHECK(prefs_);
  payload_attempt_number_ = payload_attempt_number;
  LOG(INFO) << "Payload Attempt Number = " << payload_attempt_number_;
  prefs_->SetInt64(kPrefsPayloadAttemptNumber, payload_attempt_number_);
}

void PayloadState::SetFullPayloadAttemptNumber(
    int full_payload_attempt_number) {
  CHECK(prefs_);
  full_payload_attempt_number_ = full_payload_attempt_number;
  LOG(INFO) << "Full Payload Attempt Number = " << full_payload_attempt_number_;
  prefs_->SetInt64(kPrefsFullPayloadAttemptNumber,
      full_payload_attempt_number_);
}

void PayloadState::LoadUrlIndex() {
  SetUrlIndex(GetPersistedValue(kPrefsCurrentUrlIndex));
}

void PayloadState::SetUrlIndex(uint32_t url_index) {
  CHECK(prefs_);
  url_index_ = url_index;
  LOG(INFO) << "Current URL Index = " << url_index_;
  prefs_->SetInt64(kPrefsCurrentUrlIndex, url_index_);

  // Also update the download source, which is purely dependent on the
  // current URL index alone.
  UpdateCurrentDownloadSource();
}

void PayloadState::LoadUrlSwitchCount() {
  SetUrlSwitchCount(GetPersistedValue(kPrefsUrlSwitchCount));
}

void PayloadState::SetUrlSwitchCount(uint32_t url_switch_count) {
  CHECK(prefs_);
  url_switch_count_ = url_switch_count;
  LOG(INFO) << "URL Switch Count = " << url_switch_count_;
  prefs_->SetInt64(kPrefsUrlSwitchCount, url_switch_count_);
}

void PayloadState::LoadUrlFailureCount() {
  SetUrlFailureCount(GetPersistedValue(kPrefsCurrentUrlFailureCount));
}

void PayloadState::SetUrlFailureCount(uint32_t url_failure_count) {
  CHECK(prefs_);
  url_failure_count_ = url_failure_count;
  LOG(INFO) << "Current URL (Url" << GetUrlIndex()
            << ")'s Failure Count = " << url_failure_count_;
  prefs_->SetInt64(kPrefsCurrentUrlFailureCount, url_failure_count_);
}

void PayloadState::LoadBackoffExpiryTime() {
  CHECK(prefs_);
  int64_t stored_value;
  if (!prefs_->Exists(kPrefsBackoffExpiryTime))
    return;

  if (!prefs_->GetInt64(kPrefsBackoffExpiryTime, &stored_value))
    return;

  Time stored_time = Time::FromInternalValue(stored_value);
  if (stored_time > Time::Now() + TimeDelta::FromDays(kMaxBackoffDays)) {
    LOG(ERROR) << "Invalid backoff expiry time ("
               << utils::ToString(stored_time)
               << ") in persisted state. Resetting.";
    stored_time = Time();
  }
  SetBackoffExpiryTime(stored_time);
}

void PayloadState::SetBackoffExpiryTime(const Time& new_time) {
  CHECK(prefs_);
  backoff_expiry_time_ = new_time;
  LOG(INFO) << "Backoff Expiry Time = "
            << utils::ToString(backoff_expiry_time_);
  prefs_->SetInt64(kPrefsBackoffExpiryTime,
                   backoff_expiry_time_.ToInternalValue());
}

TimeDelta PayloadState::GetUpdateDuration() {
  Time end_time = update_timestamp_end_.is_null()
    ? system_state_->clock()->GetWallclockTime() :
      update_timestamp_end_;
  return end_time - update_timestamp_start_;
}

void PayloadState::LoadUpdateTimestampStart() {
  int64_t stored_value;
  Time stored_time;

  CHECK(prefs_);

  Time now = system_state_->clock()->GetWallclockTime();

  if (!prefs_->Exists(kPrefsUpdateTimestampStart)) {
    // The preference missing is not unexpected - in that case, just
    // use the current time as start time
    stored_time = now;
  } else if (!prefs_->GetInt64(kPrefsUpdateTimestampStart, &stored_value)) {
    LOG(ERROR) << "Invalid UpdateTimestampStart value. Resetting.";
    stored_time = now;
  } else {
    stored_time = Time::FromInternalValue(stored_value);
  }

  // Sanity check: If the time read from disk is in the future
  // (modulo some slack to account for possible NTP drift
  // adjustments), something is fishy and we should report and
  // reset.
  TimeDelta duration_according_to_stored_time = now - stored_time;
  if (duration_according_to_stored_time < -kDurationSlack) {
    LOG(ERROR) << "The UpdateTimestampStart value ("
               << utils::ToString(stored_time)
               << ") in persisted state is "
               << utils::FormatTimeDelta(duration_according_to_stored_time)
               << " in the future. Resetting.";
    stored_time = now;
  }

  SetUpdateTimestampStart(stored_time);
}

void PayloadState::SetUpdateTimestampStart(const Time& value) {
  CHECK(prefs_);
  update_timestamp_start_ = value;
  prefs_->SetInt64(kPrefsUpdateTimestampStart,
                   update_timestamp_start_.ToInternalValue());
  LOG(INFO) << "Update Timestamp Start = "
            << utils::ToString(update_timestamp_start_);
}

void PayloadState::SetUpdateTimestampEnd(const Time& value) {
  update_timestamp_end_ = value;
  LOG(INFO) << "Update Timestamp End = "
            << utils::ToString(update_timestamp_end_);
}

TimeDelta PayloadState::GetUpdateDurationUptime() {
  return update_duration_uptime_;
}

void PayloadState::LoadUpdateDurationUptime() {
  int64_t stored_value;
  TimeDelta stored_delta;

  CHECK(prefs_);

  if (!prefs_->Exists(kPrefsUpdateDurationUptime)) {
    // The preference missing is not unexpected - in that case, just
    // we'll use zero as the delta
  } else if (!prefs_->GetInt64(kPrefsUpdateDurationUptime, &stored_value)) {
    LOG(ERROR) << "Invalid UpdateDurationUptime value. Resetting.";
    stored_delta = TimeDelta::FromSeconds(0);
  } else {
    stored_delta = TimeDelta::FromInternalValue(stored_value);
  }

  // Sanity-check: Uptime can never be greater than the wall-clock
  // difference (modulo some slack). If it is, report and reset
  // to the wall-clock difference.
  TimeDelta diff = GetUpdateDuration() - stored_delta;
  if (diff < -kDurationSlack) {
    LOG(ERROR) << "The UpdateDurationUptime value ("
               << utils::FormatTimeDelta(stored_delta)
               << ") in persisted state is "
               << utils::FormatTimeDelta(diff)
               << " larger than the wall-clock delta. Resetting.";
    stored_delta = update_duration_current_;
  }

  SetUpdateDurationUptime(stored_delta);
}

void PayloadState::LoadNumReboots() {
  SetNumReboots(GetPersistedValue(kPrefsNumReboots));
}

void PayloadState::LoadRollbackVersion() {
  CHECK(powerwash_safe_prefs_);
  string rollback_version;
  if (powerwash_safe_prefs_->GetString(kPrefsRollbackVersion,
                                       &rollback_version)) {
    SetRollbackVersion(rollback_version);
  }
}

void PayloadState::SetRollbackVersion(const string& rollback_version) {
  CHECK(powerwash_safe_prefs_);
  LOG(INFO) << "Blacklisting version "<< rollback_version;
  rollback_version_ = rollback_version;
  powerwash_safe_prefs_->SetString(kPrefsRollbackVersion, rollback_version);
}

void PayloadState::SetUpdateDurationUptimeExtended(const TimeDelta& value,
                                                   const Time& timestamp,
                                                   bool use_logging) {
  CHECK(prefs_);
  update_duration_uptime_ = value;
  update_duration_uptime_timestamp_ = timestamp;
  prefs_->SetInt64(kPrefsUpdateDurationUptime,
                   update_duration_uptime_.ToInternalValue());
  if (use_logging) {
    LOG(INFO) << "Update Duration Uptime = "
              << utils::FormatTimeDelta(update_duration_uptime_);
  }
}

void PayloadState::SetUpdateDurationUptime(const TimeDelta& value) {
  Time now = system_state_->clock()->GetMonotonicTime();
  SetUpdateDurationUptimeExtended(value, now, true);
}

void PayloadState::CalculateUpdateDurationUptime() {
  Time now = system_state_->clock()->GetMonotonicTime();
  TimeDelta uptime_since_last_update = now - update_duration_uptime_timestamp_;
  TimeDelta new_uptime = update_duration_uptime_ + uptime_since_last_update;
  // We're frequently called so avoid logging this write
  SetUpdateDurationUptimeExtended(new_uptime, now, false);
}

string PayloadState::GetPrefsKey(const string& prefix, DownloadSource source) {
  return prefix + "-from-" + utils::ToString(source);
}

void PayloadState::LoadCurrentBytesDownloaded(DownloadSource source) {
  string key = GetPrefsKey(kPrefsCurrentBytesDownloaded, source);
  SetCurrentBytesDownloaded(source, GetPersistedValue(key), true);
}

void PayloadState::SetCurrentBytesDownloaded(
    DownloadSource source,
    uint64_t current_bytes_downloaded,
    bool log) {
  CHECK(prefs_);

  if (source >= kNumDownloadSources)
    return;

  // Update the in-memory value.
  current_bytes_downloaded_[source] = current_bytes_downloaded;

  string prefs_key = GetPrefsKey(kPrefsCurrentBytesDownloaded, source);
  prefs_->SetInt64(prefs_key, current_bytes_downloaded);
  LOG_IF(INFO, log) << "Current bytes downloaded for "
                    << utils::ToString(source) << " = "
                    << GetCurrentBytesDownloaded(source);
}

void PayloadState::LoadTotalBytesDownloaded(DownloadSource source) {
  string key = GetPrefsKey(kPrefsTotalBytesDownloaded, source);
  SetTotalBytesDownloaded(source, GetPersistedValue(key), true);
}

void PayloadState::SetTotalBytesDownloaded(
    DownloadSource source,
    uint64_t total_bytes_downloaded,
    bool log) {
  CHECK(prefs_);

  if (source >= kNumDownloadSources)
    return;

  // Update the in-memory value.
  total_bytes_downloaded_[source] = total_bytes_downloaded;

  // Persist.
  string prefs_key = GetPrefsKey(kPrefsTotalBytesDownloaded, source);
  prefs_->SetInt64(prefs_key, total_bytes_downloaded);
  LOG_IF(INFO, log) << "Total bytes downloaded for "
                    << utils::ToString(source) << " = "
                    << GetTotalBytesDownloaded(source);
}

void PayloadState::LoadNumResponsesSeen() {
  SetNumResponsesSeen(GetPersistedValue(kPrefsNumResponsesSeen));
}

void PayloadState::SetNumResponsesSeen(int num_responses_seen) {
  CHECK(prefs_);
  num_responses_seen_ = num_responses_seen;
  LOG(INFO) << "Num Responses Seen = " << num_responses_seen_;
  prefs_->SetInt64(kPrefsNumResponsesSeen, num_responses_seen_);
}

void PayloadState::ReportUpdatesAbandonedEventCountMetric() {
  string metric = "Installer.UpdatesAbandonedEventCount";
  int value = num_responses_seen_ - 1;

  // Do not send an "abandoned" event when 0 payloads were abandoned since the
  // last successful update.
  if (value == 0)
    return;

  LOG(INFO) << "Uploading " << value << " (count) for metric " <<  metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       value,
       0,    // min value
       100,  // max value
       kNumDefaultUmaBuckets);
}

void PayloadState::ComputeCandidateUrls() {
  bool http_url_ok = true;

  if (system_state_->hardware()->IsOfficialBuild()) {
    const policy::DevicePolicy* policy = system_state_->device_policy();
    if (policy && policy->GetHttpDownloadsEnabled(&http_url_ok) && !http_url_ok)
      LOG(INFO) << "Downloads via HTTP Url are not enabled by device policy";
  } else {
    LOG(INFO) << "Allowing HTTP downloads for unofficial builds";
    http_url_ok = true;
  }

  candidate_urls_.clear();
  for (size_t i = 0; i < response_.payload_urls.size(); i++) {
    string candidate_url = response_.payload_urls[i];
    if (StartsWithASCII(candidate_url, "http://", false) && !http_url_ok)
        continue;
    candidate_urls_.push_back(candidate_url);
    LOG(INFO) << "Candidate Url" << (candidate_urls_.size() - 1)
              << ": " << candidate_url;
  }

  LOG(INFO) << "Found " << candidate_urls_.size() << " candidate URLs "
            << "out of " << response_.payload_urls.size() << " URLs supplied";
}

void PayloadState::CreateSystemUpdatedMarkerFile() {
  CHECK(prefs_);
  int64_t value = system_state_->clock()->GetWallclockTime().ToInternalValue();
  prefs_->SetInt64(kPrefsSystemUpdatedMarker, value);
}

void PayloadState::BootedIntoUpdate(TimeDelta time_to_reboot) {
  // Send |time_to_reboot| as a UMA stat.
  string metric = "Installer.TimeToRebootMinutes";
  system_state_->metrics_lib()->SendToUMA(metric,
                                          time_to_reboot.InMinutes(),
                                          0,        // min: 0 minute
                                          30*24*60, // max: 1 month (approx)
                                          kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(time_to_reboot)
            << " for metric " <<  metric;

  metric = metrics::kMetricTimeToRebootMinutes;
  system_state_->metrics_lib()->SendToUMA(metric,
                                          time_to_reboot.InMinutes(),
                                          0,        // min: 0 minute
                                          30*24*60, // max: 1 month (approx)
                                          kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(time_to_reboot)
            << " for metric " <<  metric;
}

void PayloadState::UpdateEngineStarted() {
  // Avoid the UpdateEngineStarted actions if this is not the first time we
  // run the update engine since reboot.
  if (!system_state_->system_rebooted())
    return;

  // Figure out if we just booted into a new update
  if (prefs_->Exists(kPrefsSystemUpdatedMarker)) {
    int64_t stored_value;
    if (prefs_->GetInt64(kPrefsSystemUpdatedMarker, &stored_value)) {
      Time system_updated_at = Time::FromInternalValue(stored_value);
      if (!system_updated_at.is_null()) {
        TimeDelta time_to_reboot =
            system_state_->clock()->GetWallclockTime() - system_updated_at;
        if (time_to_reboot.ToInternalValue() < 0) {
          LOG(ERROR) << "time_to_reboot is negative - system_updated_at: "
                     << utils::ToString(system_updated_at);
        } else {
          BootedIntoUpdate(time_to_reboot);
        }
      }
    }
    prefs_->Delete(kPrefsSystemUpdatedMarker);
  }
  // Check if it is needed to send metrics about a failed reboot into a new
  // version.
  ReportFailedBootIfNeeded();
}

void PayloadState::ReportFailedBootIfNeeded() {
  // If the kPrefsTargetVersionInstalledFrom is present, a successfully applied
  // payload was marked as ready immediately before the last reboot, and we
  // need to check if such payload successfully rebooted or not.
  if (prefs_->Exists(kPrefsTargetVersionInstalledFrom)) {
    int64_t installed_from = 0;
    if (!prefs_->GetInt64(kPrefsTargetVersionInstalledFrom, &installed_from)) {
      LOG(ERROR) << "Error reading TargetVersionInstalledFrom on reboot.";
      return;
    }
    if (int(installed_from) ==
        utils::GetPartitionNumber(system_state_->hardware()->BootDevice())) {
      // A reboot was pending, but the chromebook is again in the same
      // BootDevice where the update was installed from.
      int64_t target_attempt;
      if (!prefs_->GetInt64(kPrefsTargetVersionAttempt, &target_attempt)) {
        LOG(ERROR) << "Error reading TargetVersionAttempt when "
                      "TargetVersionInstalledFrom was present.";
        target_attempt = 1;
      }

      // Report the UMA metric of the current boot failure.
      string metric = "Installer.RebootToNewPartitionAttempt";

      LOG(INFO) << "Uploading " << target_attempt
                << " (count) for metric " <<  metric;
      system_state_->metrics_lib()->SendToUMA(
           metric,
           target_attempt,
           1,    // min value
           50,   // max value
           kNumDefaultUmaBuckets);

      metric = metrics::kMetricFailedUpdateCount;
      LOG(INFO) << "Uploading " << target_attempt
                << " (count) for metric " <<  metric;
      system_state_->metrics_lib()->SendToUMA(
           metric,
           target_attempt,
           1,    // min value
           50,   // max value
           kNumDefaultUmaBuckets);
    } else {
      prefs_->Delete(kPrefsTargetVersionAttempt);
      prefs_->Delete(kPrefsTargetVersionUniqueId);
    }
    prefs_->Delete(kPrefsTargetVersionInstalledFrom);
  }
}

void PayloadState::ExpectRebootInNewVersion(const string& target_version_uid) {
  // Expect to boot into the new partition in the next reboot setting the
  // TargetVersion* flags in the Prefs.
  string stored_target_version_uid;
  string target_version_id;
  string target_partition;
  int64_t target_attempt;

  if (prefs_->Exists(kPrefsTargetVersionUniqueId) &&
      prefs_->GetString(kPrefsTargetVersionUniqueId,
                        &stored_target_version_uid) &&
      stored_target_version_uid == target_version_uid) {
    if (!prefs_->GetInt64(kPrefsTargetVersionAttempt, &target_attempt))
      target_attempt = 0;
  } else {
    prefs_->SetString(kPrefsTargetVersionUniqueId, target_version_uid);
    target_attempt = 0;
  }
  prefs_->SetInt64(kPrefsTargetVersionAttempt, target_attempt + 1);

  prefs_->SetInt64(kPrefsTargetVersionInstalledFrom,
                    utils::GetPartitionNumber(
                        system_state_->hardware()->BootDevice()));
}

void PayloadState::ResetUpdateStatus() {
  // Remove the TargetVersionInstalledFrom pref so that if the machine is
  // rebooted the next boot is not flagged as failed to rebooted into the
  // new applied payload.
  prefs_->Delete(kPrefsTargetVersionInstalledFrom);

  // Also decrement the attempt number if it exists.
  int64_t target_attempt;
  if (prefs_->GetInt64(kPrefsTargetVersionAttempt, &target_attempt))
    prefs_->SetInt64(kPrefsTargetVersionAttempt, target_attempt-1);
}

int PayloadState::GetP2PNumAttempts() {
  return p2p_num_attempts_;
}

void PayloadState::SetP2PNumAttempts(int value) {
  p2p_num_attempts_ = value;
  LOG(INFO) << "p2p Num Attempts = " << p2p_num_attempts_;
  CHECK(prefs_);
  prefs_->SetInt64(kPrefsP2PNumAttempts, value);
}

void PayloadState::LoadP2PNumAttempts() {
  SetP2PNumAttempts(GetPersistedValue(kPrefsP2PNumAttempts));
}

Time PayloadState::GetP2PFirstAttemptTimestamp() {
  return p2p_first_attempt_timestamp_;
}

void PayloadState::SetP2PFirstAttemptTimestamp(const Time& time) {
  p2p_first_attempt_timestamp_ = time;
  LOG(INFO) << "p2p First Attempt Timestamp = "
            << utils::ToString(p2p_first_attempt_timestamp_);
  CHECK(prefs_);
  int64_t stored_value = time.ToInternalValue();
  prefs_->SetInt64(kPrefsP2PFirstAttemptTimestamp, stored_value);
}

void PayloadState::LoadP2PFirstAttemptTimestamp() {
  int64_t stored_value = GetPersistedValue(kPrefsP2PFirstAttemptTimestamp);
  Time stored_time = Time::FromInternalValue(stored_value);
  SetP2PFirstAttemptTimestamp(stored_time);
}

void PayloadState::P2PNewAttempt() {
  CHECK(prefs_);
  // Set timestamp, if it hasn't been set already
  if (p2p_first_attempt_timestamp_.is_null()) {
    SetP2PFirstAttemptTimestamp(system_state_->clock()->GetWallclockTime());
  }
  // Increase number of attempts
  SetP2PNumAttempts(GetP2PNumAttempts() + 1);
}

bool PayloadState::P2PAttemptAllowed() {
  if (p2p_num_attempts_ > kMaxP2PAttempts) {
    LOG(INFO) << "Number of p2p attempts is " << p2p_num_attempts_
              << " which is greater than "
              << kMaxP2PAttempts
              << " - disallowing p2p.";
    return false;
  }

  if (!p2p_first_attempt_timestamp_.is_null()) {
    Time now = system_state_->clock()->GetWallclockTime();
    TimeDelta time_spent_attempting_p2p = now - p2p_first_attempt_timestamp_;
    if (time_spent_attempting_p2p.InSeconds() < 0) {
      LOG(ERROR) << "Time spent attempting p2p is negative"
                 << " - disallowing p2p.";
      return false;
    }
    if (time_spent_attempting_p2p.InSeconds() > kMaxP2PAttemptTimeSeconds) {
      LOG(INFO) << "Time spent attempting p2p is "
                << utils::FormatTimeDelta(time_spent_attempting_p2p)
                << " which is greater than "
                << utils::FormatTimeDelta(TimeDelta::FromSeconds(
                       kMaxP2PAttemptTimeSeconds))
                << " - disallowing p2p.";
      return false;
    }
  }

  return true;
}

}  // namespace chromeos_update_engine
