// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <glib.h>

#include "base/file_path.h"
#include "base/file_util.h"
#include "base/stringprintf.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "update_engine/constants.h"
#include "update_engine/fake_clock.h"
#include "update_engine/fake_hardware.h"
#include "update_engine/mock_system_state.h"
#include "update_engine/omaha_request_action.h"
#include "update_engine/payload_state.h"
#include "update_engine/prefs.h"
#include "update_engine/prefs_mock.h"
#include "update_engine/test_utils.h"
#include "update_engine/utils.h"

using base::Time;
using base::TimeDelta;
using std::string;
using testing::_;
using testing::AnyNumber;
using testing::AtLeast;
using testing::Mock;
using testing::NiceMock;
using testing::Return;
using testing::SetArgumentPointee;

namespace chromeos_update_engine {

const char* kCurrentBytesDownloadedFromHttps =
  "current-bytes-downloaded-from-HttpsServer";
const char* kTotalBytesDownloadedFromHttps =
  "total-bytes-downloaded-from-HttpsServer";
const char* kCurrentBytesDownloadedFromHttp =
  "current-bytes-downloaded-from-HttpServer";
const char* kTotalBytesDownloadedFromHttp =
  "total-bytes-downloaded-from-HttpServer";

static void SetupPayloadStateWith2Urls(string hash,
                                       bool http_enabled,
                                       PayloadState* payload_state,
                                       OmahaResponse* response) {
  response->payload_urls.clear();
  response->payload_urls.push_back("http://test");
  response->payload_urls.push_back("https://test");
  response->size = 523456789;
  response->hash = hash;
  response->metadata_size = 558123;
  response->metadata_signature = "metasign";
  response->max_failure_count_per_url = 3;
  payload_state->SetResponse(*response);
  string stored_response_sign = payload_state->GetResponseSignature();

  string expected_url_https_only =
      "NumURLs = 1\n"
      "Candidate Url0 = https://test\n";

  string expected_urls_both =
      "NumURLs = 2\n"
      "Candidate Url0 = http://test\n"
      "Candidate Url1 = https://test\n";

  string expected_response_sign =
      (http_enabled ? expected_urls_both : expected_url_https_only) +
      StringPrintf("Payload Size = 523456789\n"
                   "Payload Sha256 Hash = %s\n"
                   "Metadata Size = 558123\n"
                   "Metadata Signature = metasign\n"
                   "Is Delta Payload = %d\n"
                   "Max Failure Count Per Url = %d\n"
                   "Disable Payload Backoff = %d\n",
                   hash.c_str(),
                   response->is_delta_payload,
                   response->max_failure_count_per_url,
                   response->disable_payload_backoff);
  EXPECT_EQ(expected_response_sign, stored_response_sign);
}

class PayloadStateTest : public ::testing::Test { };

TEST(PayloadStateTest, DidYouAddANewErrorCode) {
  if (kErrorCodeUmaReportedMax != 44) {
    LOG(ERROR) << "The following failure is intentional. If you added a new "
               << "ErrorCode enum value, make sure to add it to the "
               << "PayloadState::UpdateFailed method and then update this test "
               << "to the new value of kErrorCodeUmaReportedMax, which is "
               << kErrorCodeUmaReportedMax;
    EXPECT_FALSE("Please see the log line above");
  }
}

TEST(PayloadStateTest, SetResponseWorksWithEmptyResponse) {
  OmahaResponse response;
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();
  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, 0)).Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0)).Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateTimestampStart, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateDurationUptime, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttps, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttp, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, 0)).Times(AtLeast(1));
  PayloadState payload_state;
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  payload_state.SetResponse(response);
  string stored_response_sign = payload_state.GetResponseSignature();
  string expected_response_sign = "NumURLs = 0\n"
                                  "Payload Size = 0\n"
                                  "Payload Sha256 Hash = \n"
                                  "Metadata Size = 0\n"
                                  "Metadata Signature = \n"
                                  "Is Delta Payload = 0\n"
                                  "Max Failure Count Per Url = 0\n"
                                  "Disable Payload Backoff = 0\n";
  EXPECT_EQ(expected_response_sign, stored_response_sign);
  EXPECT_EQ("", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());
}

TEST(PayloadStateTest, SetResponseWorksWithSingleUrl) {
  OmahaResponse response;
  response.payload_urls.push_back("https://single.url.test");
  response.size = 123456789;
  response.hash = "hash";
  response.metadata_size = 58123;
  response.metadata_signature = "msign";
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();
  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateTimestampStart, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateDurationUptime, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttps, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttp, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, 0))
      .Times(AtLeast(1));
  PayloadState payload_state;
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  payload_state.SetResponse(response);
  string stored_response_sign = payload_state.GetResponseSignature();
  string expected_response_sign = "NumURLs = 1\n"
                                  "Candidate Url0 = https://single.url.test\n"
                                  "Payload Size = 123456789\n"
                                  "Payload Sha256 Hash = hash\n"
                                  "Metadata Size = 58123\n"
                                  "Metadata Signature = msign\n"
                                  "Is Delta Payload = 0\n"
                                  "Max Failure Count Per Url = 0\n"
                                  "Disable Payload Backoff = 0\n";
  EXPECT_EQ(expected_response_sign, stored_response_sign);
  EXPECT_EQ("https://single.url.test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());
}

TEST(PayloadStateTest, SetResponseWorksWithMultipleUrls) {
  OmahaResponse response;
  response.payload_urls.push_back("http://multiple.url.test");
  response.payload_urls.push_back("https://multiple.url.test");
  response.size = 523456789;
  response.hash = "rhash";
  response.metadata_size = 558123;
  response.metadata_signature = "metasign";
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();
  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttps, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttp, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, 0))
      .Times(AtLeast(1));

  PayloadState payload_state;
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  payload_state.SetResponse(response);
  string stored_response_sign = payload_state.GetResponseSignature();
  string expected_response_sign = "NumURLs = 2\n"
                                  "Candidate Url0 = http://multiple.url.test\n"
                                  "Candidate Url1 = https://multiple.url.test\n"
                                  "Payload Size = 523456789\n"
                                  "Payload Sha256 Hash = rhash\n"
                                  "Metadata Size = 558123\n"
                                  "Metadata Signature = metasign\n"
                                  "Is Delta Payload = 0\n"
                                  "Max Failure Count Per Url = 0\n"
                                  "Disable Payload Backoff = 0\n";
  EXPECT_EQ(expected_response_sign, stored_response_sign);
  EXPECT_EQ("http://multiple.url.test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());
}

TEST(PayloadStateTest, CanAdvanceUrlIndexCorrectly) {
  OmahaResponse response;
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();
  PayloadState payload_state;

  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  // Payload attempt should start with 0 and then advance to 1.
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 1))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 1))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, _)).Times(AtLeast(2));

  // Reboots will be set
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, _)).Times(AtLeast(1));

  // Url index should go from 0 to 1 twice.
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0)).Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 1)).Times(AtLeast(1));

  // Failure count should be called each times url index is set, so that's
  // 4 times for this test.
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(4));

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // This does a SetResponse which causes all the states to be set to 0 for
  // the first time.
  SetupPayloadStateWith2Urls("Hash1235", true, &payload_state, &response);
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());

  // Verify that on the first error, the URL index advances to 1.
  ErrorCode error = kErrorCodeDownloadMetadataSignatureMismatch;
  payload_state.UpdateFailed(error);
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());

  // Verify that on the next error, the URL index wraps around to 0.
  payload_state.UpdateFailed(error);
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());

  // Verify that on the next error, it again advances to 1.
  payload_state.UpdateFailed(error);
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());

  // Verify that we switched URLs three times
  EXPECT_EQ(3, payload_state.GetUrlSwitchCount());
}

TEST(PayloadStateTest, NewResponseResetsPayloadState) {
  OmahaResponse response;
  MockSystemState mock_system_state;
  PayloadState payload_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // The first response doesn't send an abandoned event.
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdatesAbandonedEventCount", 0, _, _, _)).Times(0);

  // Set the first response.
  SetupPayloadStateWith2Urls("Hash5823", true, &payload_state, &response);
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());

  // Advance the URL index to 1 by faking an error.
  ErrorCode error = kErrorCodeDownloadMetadataSignatureMismatch;
  payload_state.UpdateFailed(error);
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdatesAbandonedEventCount", 1, _, _, _));

  // Now, slightly change the response and set it again.
  SetupPayloadStateWith2Urls("Hash8225", true, &payload_state, &response);
  EXPECT_EQ(2, payload_state.GetNumResponsesSeen());

  // Fake an error again.
  payload_state.UpdateFailed(error);
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdatesAbandonedEventCount", 2, _, _, _));

  // Return a third different response.
  SetupPayloadStateWith2Urls("Hash9999", true, &payload_state, &response);
  EXPECT_EQ(3, payload_state.GetNumResponsesSeen());

  // Make sure the url index was reset to 0 because of the new response.
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
  EXPECT_EQ(0,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0, payload_state.GetCurrentBytesDownloaded(
                 kDownloadSourceHttpsServer));
  EXPECT_EQ(0,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));
}

TEST(PayloadStateTest, AllCountersGetUpdatedProperlyOnErrorCodesAndEvents) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  int progress_bytes = 100;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();

  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(2));
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 1))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 2))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(2));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 1))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 2))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, _)).Times(AtLeast(4));

  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0)).Times(AtLeast(4));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 1)).Times(AtLeast(2));

  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(7));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 1))
    .Times(AtLeast(2));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 2))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateTimestampStart, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsUpdateDurationUptime, _))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttps, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttp, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kCurrentBytesDownloadedFromHttp, progress_bytes))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kTotalBytesDownloadedFromHttp, progress_bytes))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, 0))
      .Times(AtLeast(1));

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  SetupPayloadStateWith2Urls("Hash5873", true, &payload_state, &response);
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());

  // This should advance the URL index.
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  // This should advance the failure count only.
  payload_state.UpdateFailed(kErrorCodeDownloadTransferError);
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlFailureCount());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  // This should advance the failure count only.
  payload_state.UpdateFailed(kErrorCodeDownloadTransferError);
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(2, payload_state.GetUrlFailureCount());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  // This should advance the URL index as we've reached the
  // max failure count and reset the failure count for the new URL index.
  // This should also wrap around the URL index and thus cause the payload
  // attempt number to be incremented.
  payload_state.UpdateFailed(kErrorCodeDownloadTransferError);
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(1, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(2, payload_state.GetUrlSwitchCount());
  EXPECT_TRUE(payload_state.ShouldBackoffDownload());

  // This should advance the URL index.
  payload_state.UpdateFailed(kErrorCodePayloadHashMismatchError);
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(1, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(3, payload_state.GetUrlSwitchCount());
  EXPECT_TRUE(payload_state.ShouldBackoffDownload());

  // This should advance the URL index and payload attempt number due to
  // wrap-around of URL index.
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMissingError);
  EXPECT_EQ(2, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(2, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(4, payload_state.GetUrlSwitchCount());
  EXPECT_TRUE(payload_state.ShouldBackoffDownload());

  // This HTTP error code should only increase the failure count.
  payload_state.UpdateFailed(static_cast<ErrorCode>(
      kErrorCodeOmahaRequestHTTPResponseBase + 404));
  EXPECT_EQ(2, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(2, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlFailureCount());
  EXPECT_EQ(4, payload_state.GetUrlSwitchCount());
  EXPECT_TRUE(payload_state.ShouldBackoffDownload());

  // And that failure count should be reset when we download some bytes
  // afterwards.
  payload_state.DownloadProgress(progress_bytes);
  EXPECT_EQ(2, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(2, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(4, payload_state.GetUrlSwitchCount());
  EXPECT_TRUE(payload_state.ShouldBackoffDownload());

  // Now, slightly change the response and set it again.
  SetupPayloadStateWith2Urls("Hash8532", true, &payload_state, &response);
  EXPECT_EQ(2, payload_state.GetNumResponsesSeen());

  // Make sure the url index was reset to 0 because of the new response.
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
  EXPECT_FALSE(payload_state.ShouldBackoffDownload());
}

TEST(PayloadStateTest, PayloadAttemptNumberIncreasesOnSuccessfulFullDownload) {
  OmahaResponse response;
  response.is_delta_payload = false;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();

  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 1))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 1))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, _))
    .Times(AtLeast(2));

  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(1));

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.PayloadAttemptNumber", 1, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.FullPayloadAttemptNumber", 1, _, _, _));

  // This should just advance the payload attempt number;
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  payload_state.DownloadComplete();
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(1, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
}

TEST(PayloadStateTest, PayloadAttemptNumberIncreasesOnSuccessfulDeltaDownload) {
  OmahaResponse response;
  response.is_delta_payload = true;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();

  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AnyNumber());
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsPayloadAttemptNumber, 1))
    .Times(AtLeast(1));

  // kPrefsFullPayloadAttemptNumber is not incremented for delta payloads.
  EXPECT_CALL(*prefs, SetInt64(kPrefsFullPayloadAttemptNumber, 0))
    .Times(AtLeast(1));

  EXPECT_CALL(*prefs, SetInt64(kPrefsBackoffExpiryTime, _))
    .Times(1);

  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlIndex, 0))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs, SetInt64(kPrefsCurrentUrlFailureCount, 0))
    .Times(AtLeast(1));

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  // Metrics for Full payload attempt number is not sent with Delta payloads.
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.PayloadAttemptNumber", 1, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.FullPayloadAttemptNumber", _, _, _, _))
      .Times(0);

  // This should just advance the payload attempt number;
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  payload_state.DownloadComplete();
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
}

TEST(PayloadStateTest, SetResponseResetsInvalidUrlIndex) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash4427", true, &payload_state, &response);

  // Generate enough events to advance URL index, failure count and
  // payload attempt number all to 1.
  payload_state.DownloadComplete();
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  payload_state.UpdateFailed(kErrorCodeDownloadTransferError);
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(1, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlFailureCount());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());

  // Now, simulate a corrupted url index on persisted store which gets
  // loaded when update_engine restarts. Using a different prefs object
  // so as to not bother accounting for the uninteresting calls above.
  MockSystemState mock_system_state2;
  NiceMock<PrefsMock>* prefs2 = mock_system_state2.mock_prefs();
  EXPECT_CALL(*prefs2, Exists(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*prefs2, GetInt64(_,_)).Times(AtLeast(1));
  EXPECT_CALL(*prefs2, GetInt64(kPrefsPayloadAttemptNumber, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs2, GetInt64(kPrefsFullPayloadAttemptNumber, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs2, GetInt64(kPrefsCurrentUrlIndex, _))
      .WillRepeatedly(DoAll(SetArgumentPointee<1>(2), Return(true)));
  EXPECT_CALL(*prefs2, GetInt64(kPrefsCurrentUrlFailureCount, _))
    .Times(AtLeast(1));
  EXPECT_CALL(*prefs2, GetInt64(kPrefsUrlSwitchCount, _))
    .Times(AtLeast(1));

  // Note: This will be a different payload object, but the response should
  // have the same hash as before so as to not trivially reset because the
  // response was different. We want to specifically test that even if the
  // response is same, we should reset the state if we find it corrupted.
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state2));
  SetupPayloadStateWith2Urls("Hash4427", true, &payload_state, &response);

  // Make sure all counters get reset to 0 because of the corrupted URL index
  // we supplied above.
  EXPECT_EQ(0, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());
}

TEST(PayloadStateTest, NoBackoffForDeltaPayloads) {
  OmahaResponse response;
  response.is_delta_payload = true;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash6437", true, &payload_state, &response);

  // Simulate a successful download and see that we're ready to download
  // again without any backoff as this is a delta payload.
  payload_state.DownloadComplete();
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_FALSE(payload_state.ShouldBackoffDownload());

  // Simulate two failures (enough to cause payload backoff) and check
  // again that we're ready to re-download without any backoff as this is
  // a delta payload.
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(2, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(0, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_FALSE(payload_state.ShouldBackoffDownload());
}

static void CheckPayloadBackoffState(PayloadState* payload_state,
                                     int expected_attempt_number,
                                     TimeDelta expected_days) {
  payload_state->DownloadComplete();
  EXPECT_EQ(expected_attempt_number,
      payload_state->GetFullPayloadAttemptNumber());
  EXPECT_TRUE(payload_state->ShouldBackoffDownload());
  Time backoff_expiry_time = payload_state->GetBackoffExpiryTime();
  // Add 1 hour extra to the 6 hour fuzz check to tolerate edge cases.
  TimeDelta max_fuzz_delta = TimeDelta::FromHours(7);
  Time expected_min_time = Time::Now() + expected_days - max_fuzz_delta;
  Time expected_max_time = Time::Now() + expected_days + max_fuzz_delta;
  EXPECT_LT(expected_min_time.ToInternalValue(),
            backoff_expiry_time.ToInternalValue());
  EXPECT_GT(expected_max_time.ToInternalValue(),
            backoff_expiry_time.ToInternalValue());
}

TEST(PayloadStateTest, BackoffPeriodsAreInCorrectRange) {
  OmahaResponse response;
  response.is_delta_payload = false;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8939", true, &payload_state, &response);

  CheckPayloadBackoffState(&payload_state, 1,  TimeDelta::FromDays(1));
  CheckPayloadBackoffState(&payload_state, 2,  TimeDelta::FromDays(2));
  CheckPayloadBackoffState(&payload_state, 3,  TimeDelta::FromDays(4));
  CheckPayloadBackoffState(&payload_state, 4,  TimeDelta::FromDays(8));
  CheckPayloadBackoffState(&payload_state, 5,  TimeDelta::FromDays(16));
  CheckPayloadBackoffState(&payload_state, 6,  TimeDelta::FromDays(16));
  CheckPayloadBackoffState(&payload_state, 7,  TimeDelta::FromDays(16));
  CheckPayloadBackoffState(&payload_state, 8,  TimeDelta::FromDays(16));
  CheckPayloadBackoffState(&payload_state, 9,  TimeDelta::FromDays(16));
  CheckPayloadBackoffState(&payload_state, 10,  TimeDelta::FromDays(16));
}

TEST(PayloadStateTest, BackoffLogicCanBeDisabled) {
  OmahaResponse response;
  response.disable_payload_backoff = true;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8939", true, &payload_state, &response);

  // Simulate a successful download and see that we are ready to download
  // again without any backoff.
  payload_state.DownloadComplete();
  EXPECT_EQ(1, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(1, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_FALSE(payload_state.ShouldBackoffDownload());

  // Test again, this time by simulating two errors that would cause
  // the payload attempt number to increment due to wrap around. And
  // check that we are still ready to re-download without any backoff.
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  payload_state.UpdateFailed(kErrorCodeDownloadMetadataSignatureMismatch);
  EXPECT_EQ(2, payload_state.GetPayloadAttemptNumber());
  EXPECT_EQ(2, payload_state.GetFullPayloadAttemptNumber());
  EXPECT_FALSE(payload_state.ShouldBackoffDownload());
}

TEST(PayloadStateTest, BytesDownloadedMetricsGetAddedToCorrectSources) {
  OmahaResponse response;
  response.disable_payload_backoff = true;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  int https_total = 0;
  int http_total = 0;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash3286", true, &payload_state, &response);
  EXPECT_EQ(1, payload_state.GetNumResponsesSeen());

  // Simulate a previous attempt with in order to set an initial non-zero value
  // for the total bytes downloaded for HTTP.
  int prev_chunk = 323456789;
  http_total += prev_chunk;
  payload_state.DownloadProgress(prev_chunk);

  // Ensure that the initial values for HTTP reflect this attempt.
  EXPECT_EQ(prev_chunk,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(http_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));

  // Change the response hash so as to simulate a new response which will
  // reset the current bytes downloaded, but not the total bytes downloaded.
  SetupPayloadStateWith2Urls("Hash9904", true, &payload_state, &response);
  EXPECT_EQ(2, payload_state.GetNumResponsesSeen());

  // First, simulate successful download of a few bytes over HTTP.
  int first_chunk = 5000000;
  http_total += first_chunk;
  payload_state.DownloadProgress(first_chunk);
  // Test that first all progress is made on HTTP and none on HTTPS.
  EXPECT_EQ(first_chunk,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(http_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0, payload_state.GetCurrentBytesDownloaded(
                 kDownloadSourceHttpsServer));
  EXPECT_EQ(https_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));

  // Simulate an error that'll cause the url index to point to https.
  ErrorCode error = kErrorCodeDownloadMetadataSignatureMismatch;
  payload_state.UpdateFailed(error);

  // Test that no new progress is made on HTTP and new progress is on HTTPS.
  int second_chunk = 23456789;
  https_total += second_chunk;
  payload_state.DownloadProgress(second_chunk);
  EXPECT_EQ(first_chunk,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(http_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(second_chunk, payload_state.GetCurrentBytesDownloaded(
              kDownloadSourceHttpsServer));
  EXPECT_EQ(https_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));

  // Simulate error to go back to http.
  payload_state.UpdateFailed(error);
  int third_chunk = 32345678;
  int http_chunk = first_chunk + third_chunk;
  http_total += third_chunk;
  int https_chunk = second_chunk;
  payload_state.DownloadProgress(third_chunk);

  // Test that third chunk is again back on HTTP. HTTPS remains on second chunk.
  EXPECT_EQ(http_chunk,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(http_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(second_chunk, payload_state.GetCurrentBytesDownloaded(
                 kDownloadSourceHttpsServer));
  EXPECT_EQ(https_total,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(_, _, _, _, _))
    .Times(AnyNumber());
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.SuccessfulMBsDownloadedFromHttpServer",
      http_chunk / kNumBytesInOneMiB, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.TotalMBsDownloadedFromHttpServer",
      http_total / kNumBytesInOneMiB, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.SuccessfulMBsDownloadedFromHttpsServer",
      https_chunk / kNumBytesInOneMiB, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.TotalMBsDownloadedFromHttpsServer",
      https_total / kNumBytesInOneMiB, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdateURLSwitches",
      2, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdateDurationMinutes",
      _, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.UpdateDurationUptimeMinutes",
      _, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.DownloadSourcesUsed", 3, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.DownloadOverheadPercentage", 542, _, _, _));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendEnumToUMA(
      "Installer.PayloadFormat", kPayloadTypeFull, kNumPayloadTypes));
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.AttemptsCount.Total", 1, _, _, _));

  payload_state.UpdateSucceeded();

  // Make sure the metrics are reset after a successful update.
  EXPECT_EQ(0,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0, payload_state.GetCurrentBytesDownloaded(
                 kDownloadSourceHttpsServer));
  EXPECT_EQ(0,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));
  EXPECT_EQ(0, payload_state.GetNumResponsesSeen());
}

TEST(PayloadStateTest, RestartingUpdateResetsMetrics) {
  OmahaResponse response;
  MockSystemState mock_system_state;
  PayloadState payload_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // Set the first response.
  SetupPayloadStateWith2Urls("Hash5823", true, &payload_state, &response);

  int num_bytes = 10000;
  payload_state.DownloadProgress(num_bytes);
  EXPECT_EQ(num_bytes,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(num_bytes,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(0, payload_state.GetCurrentBytesDownloaded(
                 kDownloadSourceHttpsServer));
  EXPECT_EQ(0,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpsServer));

  payload_state.UpdateRestarted();
  // Make sure the current bytes downloaded is reset, but not the total bytes.
  EXPECT_EQ(0,
            payload_state.GetCurrentBytesDownloaded(kDownloadSourceHttpServer));
  EXPECT_EQ(num_bytes,
            payload_state.GetTotalBytesDownloaded(kDownloadSourceHttpServer));
}

TEST(PayloadStateTest, NumRebootsIncrementsCorrectly) {
  MockSystemState mock_system_state;
  PayloadState payload_state;

  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();
  EXPECT_CALL(*prefs, SetInt64(_,_)).Times(AtLeast(0));
  EXPECT_CALL(*prefs, SetInt64(kPrefsNumReboots, 1)).Times(AtLeast(1));

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  payload_state.UpdateRestarted();
  EXPECT_EQ(0, payload_state.GetNumReboots());

  EXPECT_CALL(mock_system_state, system_rebooted()).WillOnce(Return(true));
  payload_state.UpdateResumed();
  // Num reboots should be incremented because system rebooted detected.
  EXPECT_EQ(1, payload_state.GetNumReboots());

  EXPECT_CALL(mock_system_state, system_rebooted()).WillOnce(Return(false));
  payload_state.UpdateResumed();
  // Num reboots should now be 1 as reboot was not detected.
  EXPECT_EQ(1, payload_state.GetNumReboots());

  // Restart the update again to verify we set the num of reboots back to 0.
  payload_state.UpdateRestarted();
  EXPECT_EQ(0, payload_state.GetNumReboots());
}

TEST(PayloadStateTest, RollbackVersion) {
  MockSystemState mock_system_state;
  PayloadState payload_state;

  NiceMock<PrefsMock>* mock_powerwash_safe_prefs =
      mock_system_state.mock_powerwash_safe_prefs();
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // Verify pre-conditions are good.
  EXPECT_TRUE(payload_state.GetRollbackVersion().empty());

  // Mock out the os version and make sure it's blacklisted correctly.
  string rollback_version = "2345.0.0";
  OmahaRequestParams params(&mock_system_state);
  params.Init(rollback_version, "", false);
  mock_system_state.set_request_params(&params);

  EXPECT_CALL(*mock_powerwash_safe_prefs, SetString(kPrefsRollbackVersion,
                                                    rollback_version));
  payload_state.Rollback();

  EXPECT_EQ(rollback_version, payload_state.GetRollbackVersion());
}

TEST(PayloadStateTest, DurationsAreCorrect) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  FakeClock fake_clock;
  Prefs prefs;
  string temp_dir;

  // Set the clock to a well-known time - 1 second on the wall-clock
  // and 2 seconds on the monotonic clock
  fake_clock.SetWallclockTime(Time::FromInternalValue(1000000));
  fake_clock.SetMonotonicTime(Time::FromInternalValue(2000000));

  // We need persistent preferences for this test
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateDurationTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));

  mock_system_state.set_clock(&fake_clock);
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // Check that durations are correct for a successful update where
  // time has advanced 7 seconds on the wall clock and 4 seconds on
  // the monotonic clock.
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);
  fake_clock.SetWallclockTime(Time::FromInternalValue(8000000));
  fake_clock.SetMonotonicTime(Time::FromInternalValue(6000000));
  payload_state.UpdateSucceeded();
  EXPECT_EQ(payload_state.GetUpdateDuration().InMicroseconds(), 7000000);
  EXPECT_EQ(payload_state.GetUpdateDurationUptime().InMicroseconds(), 4000000);

  // Check that durations are reset when a new response comes in.
  SetupPayloadStateWith2Urls("Hash8594", true, &payload_state, &response);
  EXPECT_EQ(payload_state.GetUpdateDuration().InMicroseconds(), 0);
  EXPECT_EQ(payload_state.GetUpdateDurationUptime().InMicroseconds(), 0);

  // Advance time a bit (10 secs), simulate download progress and
  // check that durations are updated.
  fake_clock.SetWallclockTime(Time::FromInternalValue(18000000));
  fake_clock.SetMonotonicTime(Time::FromInternalValue(16000000));
  payload_state.DownloadProgress(10);
  EXPECT_EQ(payload_state.GetUpdateDuration().InMicroseconds(), 10000000);
  EXPECT_EQ(payload_state.GetUpdateDurationUptime().InMicroseconds(), 10000000);

  // Now simulate a reboot by resetting monotonic time (to 5000) and
  // creating a new PayloadState object and check that we load the
  // durations correctly (e.g. they are the same as before).
  fake_clock.SetMonotonicTime(Time::FromInternalValue(5000));
  PayloadState payload_state2;
  EXPECT_TRUE(payload_state2.Initialize(&mock_system_state));
  EXPECT_EQ(payload_state2.GetUpdateDuration().InMicroseconds(), 10000000);
  EXPECT_EQ(payload_state2.GetUpdateDurationUptime().InMicroseconds(),10000000);

  // Advance wall-clock by 7 seconds and monotonic clock by 6 seconds
  // and check that the durations are increased accordingly.
  fake_clock.SetWallclockTime(Time::FromInternalValue(25000000));
  fake_clock.SetMonotonicTime(Time::FromInternalValue(6005000));
  payload_state2.UpdateSucceeded();
  EXPECT_EQ(payload_state2.GetUpdateDuration().InMicroseconds(), 17000000);
  EXPECT_EQ(payload_state2.GetUpdateDurationUptime().InMicroseconds(),16000000);

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, RebootAfterSuccessfulUpdateTest) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  FakeClock fake_clock;
  Prefs prefs;
  string temp_dir;

  // Set the clock to a well-known time (t = 30 seconds).
  fake_clock.SetWallclockTime(Time::FromInternalValue(
      30 * Time::kMicrosecondsPerSecond));

  // We need persistent preferences for this test
  EXPECT_TRUE(utils::MakeTempDirectory(
      "/tmp/RebootAfterSuccessfulUpdateTest.XXXXXX", &temp_dir));
  prefs.Init(FilePath(temp_dir));

  mock_system_state.set_clock(&fake_clock);
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // Make the update succeed.
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);
  payload_state.UpdateSucceeded();

  // Check that the marker was written.
  EXPECT_TRUE(prefs.Exists(kPrefsSystemUpdatedMarker));

  // Now simulate a reboot and set the wallclock time to a later point
  // (t = 500 seconds). We do this by using a new PayloadState object
  // and checking that it emits the right UMA metric with the right
  // value.
  fake_clock.SetWallclockTime(Time::FromInternalValue(
      500 * Time::kMicrosecondsPerSecond));
  PayloadState payload_state2;
  EXPECT_TRUE(payload_state2.Initialize(&mock_system_state));

  // Expect 500 - 30 seconds = 470 seconds ~= 7 min 50 sec
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.TimeToRebootMinutes",
      7, _, _, _));
  EXPECT_CALL(mock_system_state, system_rebooted())
      .WillRepeatedly(Return(true));

  payload_state2.UpdateEngineStarted();

  // Check that the marker was nuked.
  EXPECT_FALSE(prefs.Exists(kPrefsSystemUpdatedMarker));

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, RestartAfterCrash) {
  PayloadState payload_state;
  MockSystemState mock_system_state;
  NiceMock<PrefsMock>* prefs = mock_system_state.mock_prefs();

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // No prefs should be used after a crash.
  EXPECT_CALL(*prefs, Exists(_)).Times(0);
  EXPECT_CALL(*prefs, SetString(_, _)).Times(0);
  EXPECT_CALL(*prefs, SetInt64(_, _)).Times(0);
  EXPECT_CALL(*prefs, SetBoolean(_, _)).Times(0);
  EXPECT_CALL(*prefs, GetString(_, _)).Times(0);
  EXPECT_CALL(*prefs, GetInt64(_, _)).Times(0);
  EXPECT_CALL(*prefs, GetBoolean(_, _)).Times(0);

  // No metrics are reported after a crash.
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(),
              SendToUMA(_, _, _, _, _)).Times(0);

  // Simulate an update_engine restart without a reboot.
  EXPECT_CALL(mock_system_state, system_rebooted())
      .WillRepeatedly(Return(false));

  payload_state.UpdateEngineStarted();
}

TEST(PayloadStateTest, CandidateUrlsComputedCorrectly) {
  OmahaResponse response;
  MockSystemState mock_system_state;
  PayloadState payload_state;

  // Pretend that this is an offical build so that the HTTP download policy
  // is honored.
  EXPECT_CALL(mock_system_state, IsOfficialBuild())
      .WillRepeatedly(Return(true));

  policy::MockDevicePolicy disable_http_policy;
  EXPECT_CALL(mock_system_state, device_policy())
      .WillRepeatedly(Return(&disable_http_policy));
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  // Test with no device policy. Should default to allowing http.
  EXPECT_CALL(disable_http_policy, GetHttpDownloadsEnabled(_))
      .WillRepeatedly(Return(false));

  // Set the first response.
  SetupPayloadStateWith2Urls("Hash8433", true, &payload_state, &response);

  // Check that we use the HTTP URL since there is no value set for allowing
  // http.
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());

  // Test with device policy not allowing http updates.
  EXPECT_CALL(disable_http_policy, GetHttpDownloadsEnabled(_))
      .WillRepeatedly(DoAll(SetArgumentPointee<0>(false), Return(true)));

  // Reset state and set again.
  SetupPayloadStateWith2Urls("Hash8433", false, &payload_state, &response);

  // Check that we skip the HTTP URL and use only the HTTPS url.
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());

  // Advance the URL index to 1 by faking an error.
  ErrorCode error = kErrorCodeDownloadMetadataSignatureMismatch;
  payload_state.UpdateFailed(error);

  // Check that we still skip the HTTP URL and use only the HTTPS url.
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlSwitchCount());

  // Now, slightly change the response and set it again.
  SetupPayloadStateWith2Urls("Hash2399", false, &payload_state, &response);

  // Check that we still skip the HTTP URL and use only the HTTPS url.
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());

  // Now, pretend that the HTTP policy is turned on. We want to make sure
  // the new policy is honored.
  policy::MockDevicePolicy enable_http_policy;
  EXPECT_CALL(mock_system_state, device_policy())
      .WillRepeatedly(Return(&enable_http_policy));
  EXPECT_CALL(enable_http_policy, GetHttpDownloadsEnabled(_))
      .WillRepeatedly(DoAll(SetArgumentPointee<0>(true), Return(true)));

  // Now, set the same response using the same hash
  // so that we can test that the state is reset not because of the
  // hash but because of the policy change which results in candidate url
  // list change.
  SetupPayloadStateWith2Urls("Hash2399", true, &payload_state, &response);

  // Check that we use the HTTP URL now and the failure count is reset.
  EXPECT_EQ("http://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());

  // Fake a failure and see if we're moving over to the HTTPS url and update
  // the URL switch count properly.
  payload_state.UpdateFailed(error);
  EXPECT_EQ("https://test", payload_state.GetCurrentUrl());
  EXPECT_EQ(1, payload_state.GetUrlSwitchCount());
  EXPECT_EQ(0, payload_state.GetUrlFailureCount());
}

TEST(PayloadStateTest, PayloadTypeMetricWhenTypeIsDelta) {
  OmahaResponse response;
  response.is_delta_payload = true;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash6437", true, &payload_state, &response);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendEnumToUMA(
      "Installer.PayloadFormat", kPayloadTypeDelta, kNumPayloadTypes));
  payload_state.UpdateSucceeded();

  // Mock the request to a request where the delta was disabled but Omaha sends
  // a delta anyway and test again.
  OmahaRequestParams params(&mock_system_state);
  params.set_delta_okay(false);
  mock_system_state.set_request_params(&params);

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash6437", true, &payload_state, &response);

  payload_state.DownloadComplete();

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendEnumToUMA(
      "Installer.PayloadFormat", kPayloadTypeDelta, kNumPayloadTypes));
  payload_state.UpdateSucceeded();
}

TEST(PayloadStateTest, PayloadTypeMetricWhenTypeIsForcedFull) {
  OmahaResponse response;
  response.is_delta_payload = false;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash6437", true, &payload_state, &response);

  // Mock the request to a request where the delta was disabled.
  OmahaRequestParams params(&mock_system_state);
  params.set_delta_okay(false);
  mock_system_state.set_request_params(&params);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendEnumToUMA(
      "Installer.PayloadFormat", kPayloadTypeForcedFull, kNumPayloadTypes));
  payload_state.UpdateSucceeded();
}

TEST(PayloadStateTest, PayloadTypeMetricWhenTypeIsFull) {
  OmahaResponse response;
  response.is_delta_payload = false;
  PayloadState payload_state;
  MockSystemState mock_system_state;

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash6437", true, &payload_state, &response);

  // Mock the request to a request where the delta is enabled, although the
  // result is full.
  OmahaRequestParams params(&mock_system_state);
  params.set_delta_okay(true);
  mock_system_state.set_request_params(&params);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendEnumToUMA(
      "Installer.PayloadFormat", kPayloadTypeFull, kNumPayloadTypes));
  payload_state.UpdateSucceeded();
}

TEST(PayloadStateTest, RebootAfterUpdateFailedMetric) {
  FakeHardware fake_hardware;
  MockSystemState mock_system_state;
  OmahaResponse response;
  PayloadState payload_state;
  Prefs prefs;
  string temp_dir;

  // Setup an environment with persistent prefs across simulated reboots.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateReboot.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_prefs(&prefs);

  fake_hardware.SetBootDevice("/dev/sda3");
  fake_hardware.SetKernelDeviceOfBootDevice("/dev/sda3", "/dev/sda2");
  mock_system_state.set_hardware(&fake_hardware);

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash3141", true, &payload_state, &response);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();
  payload_state.UpdateSucceeded();
  payload_state.ExpectRebootInNewVersion("Version:12345678");

  // Reboot into the same environment to get an UMA metric with a value of 1.
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", 1, _, _, _));
  payload_state.ReportFailedBootIfNeeded();
  Mock::VerifyAndClearExpectations(mock_system_state.mock_metrics_lib());

  // Simulate a second update and reboot into the same environment, this should
  // send a value of 2.
  payload_state.ExpectRebootInNewVersion("Version:12345678");

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", 2, _, _, _));
  payload_state.ReportFailedBootIfNeeded();
  Mock::VerifyAndClearExpectations(mock_system_state.mock_metrics_lib());

  // Simulate a third failed reboot to new version, but this time for a
  // different payload. This should send a value of 1 this time.
  payload_state.ExpectRebootInNewVersion("Version:3141592");
  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", 1, _, _, _));
  payload_state.ReportFailedBootIfNeeded();
  Mock::VerifyAndClearExpectations(mock_system_state.mock_metrics_lib());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, RebootAfterUpdateSucceed) {
  FakeHardware fake_hardware;
  MockSystemState mock_system_state;
  OmahaResponse response;
  PayloadState payload_state;
  Prefs prefs;
  string temp_dir;

  // Setup an environment with persistent prefs across simulated reboots.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateReboot.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_prefs(&prefs);

  fake_hardware.SetKernelDeviceOfBootDevice("/dev/sda3", "/dev/sda2");
  fake_hardware.SetKernelDeviceOfBootDevice("/dev/sda5", "/dev/sda4");
  fake_hardware.SetBootDevice("/dev/sda3");
  mock_system_state.set_hardware(&fake_hardware);

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash3141", true, &payload_state, &response);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();
  payload_state.UpdateSucceeded();
  payload_state.ExpectRebootInNewVersion("Version:12345678");

  // Change the BootDevice to a different one, no metric should be sent.
  fake_hardware.SetBootDevice("/dev/sda5");

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", _, _, _, _))
      .Times(0);
  payload_state.ReportFailedBootIfNeeded();

  // A second reboot in eiher partition should not send a metric.
  payload_state.ReportFailedBootIfNeeded();
  fake_hardware.SetBootDevice("/dev/sda3");
  payload_state.ReportFailedBootIfNeeded();

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, RebootAfterCanceledUpdate) {
  MockSystemState mock_system_state;
  OmahaResponse response;
  PayloadState payload_state;
  Prefs prefs;
  string temp_dir;

  // Setup an environment with persistent prefs across simulated reboots.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateReboot.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_prefs(&prefs);

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash3141", true, &payload_state, &response);

  // Simulate a successful download and update.
  payload_state.DownloadComplete();
  payload_state.UpdateSucceeded();
  payload_state.ExpectRebootInNewVersion("Version:12345678");

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", _, _, _, _))
      .Times(0);

  // Cancel the applied update.
  payload_state.ResetUpdateStatus();

  // Simulate a reboot.
  payload_state.ReportFailedBootIfNeeded();

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, UpdateSuccessWithWipedPrefs) {
  MockSystemState mock_system_state;
  PayloadState payload_state;
  Prefs prefs;
  string temp_dir;

  // Setup an environment with persistent but initially empty prefs.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateReboot.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_prefs(&prefs);

  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));

  EXPECT_CALL(*mock_system_state.mock_metrics_lib(), SendToUMA(
      "Installer.RebootToNewPartitionAttempt", _, _, _, _))
      .Times(0);

  // Simulate a reboot in this environment.
  payload_state.ReportFailedBootIfNeeded();

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, DisallowP2PAfterTooManyAttempts) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  Prefs prefs;
  string temp_dir;

  // We need persistent preferences for this test.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateP2PTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));

  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  // Should allow exactly kMaxP2PAttempts...
  for (int n = 0; n < kMaxP2PAttempts; n++) {
    payload_state.P2PNewAttempt();
    EXPECT_TRUE(payload_state.P2PAttemptAllowed());
  }
  // ... but not more than that.
  payload_state.P2PNewAttempt();
  EXPECT_FALSE(payload_state.P2PAttemptAllowed());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, DisallowP2PAfterDeadline) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  FakeClock fake_clock;
  Prefs prefs;
  string temp_dir;

  // We need persistent preferences for this test.
  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateP2PTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));

  mock_system_state.set_clock(&fake_clock);
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  // Set the clock to 1 second.
  Time epoch = Time::FromInternalValue(1000000);
  fake_clock.SetWallclockTime(epoch);

  // Do an attempt - this will set the timestamp.
  payload_state.P2PNewAttempt();

  // Check that the timestamp equals what we just set.
  EXPECT_EQ(epoch, payload_state.GetP2PFirstAttemptTimestamp());

  // Time hasn't advanced - this should work.
  EXPECT_TRUE(payload_state.P2PAttemptAllowed());

  // Set clock to half the deadline - this should work.
  fake_clock.SetWallclockTime(epoch +
      TimeDelta::FromSeconds(kMaxP2PAttemptTimeSeconds) / 2);
  EXPECT_TRUE(payload_state.P2PAttemptAllowed());

  // Check that the first attempt timestamp hasn't changed just
  // because the wall-clock time changed.
  EXPECT_EQ(epoch, payload_state.GetP2PFirstAttemptTimestamp());

  // Set clock to _just_ before the deadline - this should work.
  fake_clock.SetWallclockTime(epoch +
      TimeDelta::FromSeconds(kMaxP2PAttemptTimeSeconds - 1));
  EXPECT_TRUE(payload_state.P2PAttemptAllowed());

  // Set clock to _just_ after the deadline - this should not work.
  fake_clock.SetWallclockTime(epoch +
      TimeDelta::FromSeconds(kMaxP2PAttemptTimeSeconds + 1));
  EXPECT_FALSE(payload_state.P2PAttemptAllowed());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, P2PStateVarsInitialValue) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  Prefs prefs;
  string temp_dir;

  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateP2PTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  Time null_time = Time();
  EXPECT_EQ(null_time, payload_state.GetP2PFirstAttemptTimestamp());
  EXPECT_EQ(0, payload_state.GetP2PNumAttempts());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, P2PStateVarsArePersisted) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  FakeClock fake_clock;
  Prefs prefs;
  string temp_dir;

  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateP2PTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_clock(&fake_clock);
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  // Set the clock to something known.
  Time time = Time::FromInternalValue(12345);
  fake_clock.SetWallclockTime(time);

  // New p2p attempt - as a side-effect this will update the p2p state vars.
  payload_state.P2PNewAttempt();
  EXPECT_EQ(1, payload_state.GetP2PNumAttempts());
  EXPECT_EQ(time, payload_state.GetP2PFirstAttemptTimestamp());

  // Now create a new PayloadState and check that it loads the state
  // vars correctly.
  PayloadState payload_state2;
  EXPECT_TRUE(payload_state2.Initialize(&mock_system_state));
  EXPECT_EQ(1, payload_state2.GetP2PNumAttempts());
  EXPECT_EQ(time, payload_state2.GetP2PFirstAttemptTimestamp());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

TEST(PayloadStateTest, P2PStateVarsAreClearedOnNewResponse) {
  OmahaResponse response;
  PayloadState payload_state;
  MockSystemState mock_system_state;
  FakeClock fake_clock;
  Prefs prefs;
  string temp_dir;

  EXPECT_TRUE(utils::MakeTempDirectory("/tmp/PayloadStateP2PTests.XXXXXX",
                                       &temp_dir));
  prefs.Init(FilePath(temp_dir));
  mock_system_state.set_clock(&fake_clock);
  mock_system_state.set_prefs(&prefs);
  EXPECT_TRUE(payload_state.Initialize(&mock_system_state));
  SetupPayloadStateWith2Urls("Hash8593", true, &payload_state, &response);

  // Set the clock to something known.
  Time time = Time::FromInternalValue(12345);
  fake_clock.SetWallclockTime(time);

  // New p2p attempt - as a side-effect this will update the p2p state vars.
  payload_state.P2PNewAttempt();
  EXPECT_EQ(1, payload_state.GetP2PNumAttempts());
  EXPECT_EQ(time, payload_state.GetP2PFirstAttemptTimestamp());

  // Set a new response...
  SetupPayloadStateWith2Urls("Hash9904", true, &payload_state, &response);

  // ... and check that it clears the P2P state vars.
  Time null_time = Time();
  EXPECT_EQ(0, payload_state.GetP2PNumAttempts());
  EXPECT_EQ(null_time, payload_state.GetP2PFirstAttemptTimestamp());

  EXPECT_TRUE(utils::RecursiveUnlinkDir(temp_dir));
}

}
