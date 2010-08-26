// Copyright (c) 2009-2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/session_manager_service.h"

#include <errno.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include "base/base64.h"
#include "base/basictypes.h"
#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "chromeos/dbus/dbus.h"
#include "chromeos/dbus/service_constants.h"
#include "login_manager/bindings/client.h"
#include "login_manager/child_job.h"
#include "login_manager/file_checker.h"
#include "login_manager/mock_child_job.h"
#include "login_manager/mock_file_checker.h"
#include "login_manager/mock_nss_util.h"
#include "login_manager/mock_owner_key.h"
#include "login_manager/mock_pref_store.h"
#include "login_manager/mock_system_utils.h"
#include "login_manager/system_utils.h"

namespace login_manager {

using ::testing::AnyNumber;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Eq;
using ::testing::Return;
using ::testing::SetArgumentPointee;
using ::testing::_;

static const char kCheckedFile[] = "/tmp/checked_file";
static const char kUptimeFile1[] = "/tmp/uptime-job1-exec";
static const char kDiskFile1[] = "/tmp/disk-job1-exec";
static const char kUptimeFile2[] = "/tmp/uptime-job2-exec";
static const char kDiskFile2[] = "/tmp/disk-job2-exec";

// Used as a base class for the tests in this file.
// Gives useful shared functionality.
class SessionManagerTest : public ::testing::Test {
 public:
  SessionManagerTest()
      : manager_(NULL),
        utils_(new MockSystemUtils),
        file_checker_(new MockFileChecker(kCheckedFile)),
        store_(new MockPrefStore),
        fake_key_(CreateArray("dummy", strlen("dummy"))) {
  }

  virtual ~SessionManagerTest() {
    delete manager_;
    FilePath uptime1(kUptimeFile1);
    FilePath disk1(kDiskFile1);
    FilePath uptime2(kUptimeFile2);
    FilePath disk2(kDiskFile2);
    file_util::Delete(uptime1, false);
    file_util::Delete(disk1, false);
    file_util::Delete(uptime2, false);
    file_util::Delete(disk2, false);

    g_array_free(fake_key_, TRUE);
  }

  virtual void SetUp() {
    property_ = StringPrintf("%s=%s", kPropName, kPropValue);
    char sig[] = "signature";
    int len = strlen(sig);
    sig[2] = '\0';  // to make sure we can handle NULL inside a signature.
    fake_sig_ = CreateArray(sig, len);
    ASSERT_TRUE(base::Base64Encode(std::string(fake_sig_->data, fake_sig_->len),
                                   &fake_sig_encoded_));
  }

  virtual void TearDown() {
    // just to be sure...
    NssUtil::set_factory(NULL);
    g_array_free(fake_sig_, TRUE);
  }

 protected:
  // NOT const so that they can be passed to methods that implement dbus calls,
  // which (of necessity) take bare gchar*.
  static char kFakeEmail[];
  static char kPropName[];
  static char kPropValue[];

  // Creates the manager with the jobs. Mocks the file checker.
  // The second job can be NULL.
  void InitManager(MockChildJob* job1, MockChildJob* job2) {
    std::vector<ChildJobInterface*> jobs;
    EXPECT_CALL(*job1, GetName())
        .WillRepeatedly(Return(std::string("job1")));
    jobs.push_back(job1);
    if (job2) {
      EXPECT_CALL(*job2, GetName())
          .WillRepeatedly(Return(std::string("job2")));
      jobs.push_back(job2);
    }
    manager_ = new SessionManagerService(jobs);
    manager_->set_file_checker(file_checker_);
    manager_->test_api().set_exit_on_child_done(true);
    manager_->test_api().set_prefstore(store_);
    ON_CALL(*store_, Persist())
        .WillByDefault(Return(true));
  }

  void MockUtils() {
    manager_->test_api().set_systemutils(utils_.release());
  }

  enum ChildRuns {
    ALWAYS, NEVER, ONCE, EXACTLY_ONCE, TWICE, MAYBE_NEVER
  };

  // Configures the file_checker_ to run the children.
  void RunChildren(ChildRuns child_runs) {
    switch (child_runs) {
    case ALWAYS:
      EXPECT_CALL(*file_checker_, exists())
          .WillRepeatedly(Return(false));
      break;
    case NEVER:
      EXPECT_CALL(*file_checker_, exists())
          .WillOnce(Return(true));
      break;
    case ONCE:
      EXPECT_CALL(*file_checker_, exists())
          .WillOnce(Return(false))
          .WillOnce(Return(true));
      break;
    case EXACTLY_ONCE:
      EXPECT_CALL(*file_checker_, exists())
          .WillOnce(Return(false));
      break;
    case TWICE:
      EXPECT_CALL(*file_checker_, exists())
          .WillOnce(Return(false))
          .WillOnce(Return(false))
          .WillOnce(Return(true));
      break;
    case MAYBE_NEVER:
      // If it's called, don't run.
      EXPECT_CALL(*file_checker_, exists())
          .Times(AnyNumber())
          .WillOnce(Return(true));
    }
  }

  // Creates one job and a manager for it, running it |child_runs| times.
  MockChildJob* CreateTrivialMockJob(ChildRuns child_runs) {
    MockChildJob* job = new MockChildJob();
    InitManager(job, NULL);
    RunChildren(child_runs);

    return job;
  }

  // Caller takes ownership.
  GArray* CreateArray(const char* input, const int len) {
    GArray* output = g_array_new(FALSE, FALSE, 1);
    g_array_append_vals(output, input, len);
    return output;
  }

  SessionManagerService* manager_;
  scoped_ptr<MockSystemUtils> utils_;
  MockFileChecker* file_checker_;
  MockPrefStore* store_;
  std::string property_;
  GArray* fake_key_;
  GArray* fake_sig_;
  std::string fake_sig_encoded_;
};

// static
char SessionManagerTest::kFakeEmail[] = "cmasone@whaaat.org";
char SessionManagerTest::kPropName[] = "name";
char SessionManagerTest::kPropValue[] = "value";


// compatible with void Run()
static void BadExit() { _exit(1); }
static void BadExitAfterSleep() { sleep(1); _exit(1); }
static void RunAndSleep() { while (true) { sleep(1); } };
static void CleanExit() { _exit(0); }

TEST_F(SessionManagerTest, NoLoopTest) {
  MockChildJob* job = CreateTrivialMockJob(NEVER);
  manager_->Run();
}

TEST_F(SessionManagerTest, BadExitChild) {
  MockChildJob* job = CreateTrivialMockJob(ONCE);
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  EXPECT_CALL(*job, ShouldStop())
      .Times(1)
      .WillOnce(Return(false));
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(BadExit));

  manager_->Run();
}

TEST_F(SessionManagerTest, BadExitChild1) {
  MockChildJob* job1 = new MockChildJob;
  MockChildJob* job2 = new MockChildJob;
  InitManager(job1, job2);

  RunChildren(TWICE);
  EXPECT_CALL(*job1, RecordTime())
      .Times(2);
  EXPECT_CALL(*job2, RecordTime())
      .Times(1);
  EXPECT_CALL(*job1, ShouldStop())
      .Times(2);
  ON_CALL(*job1, Run())
      .WillByDefault(Invoke(BadExitAfterSleep));
  ON_CALL(*job2, Run())
      .WillByDefault(Invoke(RunAndSleep));

  manager_->Run();
}

TEST_F(SessionManagerTest, BadExitChild2) {
  MockChildJob* job1 = new MockChildJob;
  MockChildJob* job2 = new MockChildJob;
  InitManager(job1, job2);

  RunChildren(TWICE);
  EXPECT_CALL(*job1, RecordTime())
      .Times(1);
  EXPECT_CALL(*job2, RecordTime())
      .Times(2);
  EXPECT_CALL(*job2, ShouldStop())
      .Times(2);
  ON_CALL(*job1, Run())
      .WillByDefault(Invoke(RunAndSleep));
  ON_CALL(*job2, Run())
      .WillByDefault(Invoke(BadExitAfterSleep));

  manager_->Run();
}

TEST_F(SessionManagerTest, CleanExitChild) {
  MockChildJob* job = CreateTrivialMockJob(EXACTLY_ONCE);
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(CleanExit));

  manager_->Run();
}

TEST_F(SessionManagerTest, CleanExitChild2) {
  MockChildJob* job1 = new MockChildJob;
  MockChildJob* job2 = new MockChildJob;
  InitManager(job1, job2);
  // Let the manager cause the clean exit.
  manager_->test_api().set_exit_on_child_done(false);

  RunChildren(EXACTLY_ONCE);
  EXPECT_CALL(*job1, RecordTime())
      .Times(1);
  EXPECT_CALL(*job2, RecordTime())
      .Times(1);
  ON_CALL(*job1, Run())
      .WillByDefault(Invoke(RunAndSleep));
  ON_CALL(*job2, Run())
      .WillByDefault(Invoke(CleanExit));

  manager_->Run();
}

TEST_F(SessionManagerTest, LoadOwnerKey) {
  MockChildJob* job = CreateTrivialMockJob(EXACTLY_ONCE);
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(CleanExit));

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));

  manager_->test_api().set_ownerkey(key);

  manager_->Run();
}

TEST_F(SessionManagerTest, LockedExit) {
  MockChildJob* job1 = new MockChildJob;
  MockChildJob* job2 = new MockChildJob;
  InitManager(job1, job2);
  // Let the manager cause the clean exit.
  manager_->test_api().set_exit_on_child_done(false);

  RunChildren(ALWAYS);

  EXPECT_CALL(*job1, RecordTime())
      .Times(1);
  EXPECT_CALL(*job2, RecordTime())
      .Times(1);
  EXPECT_CALL(*job1, ShouldStop())
      .Times(0);

  manager_->test_api().set_screen_locked(true);
  ON_CALL(*job1, Run())
      .WillByDefault(Invoke(BadExitAfterSleep));
  ON_CALL(*job2, Run())
      .WillByDefault(Invoke(RunAndSleep));

  manager_->Run();
}

TEST_F(SessionManagerTest, MustStopChild) {
  MockChildJob* job = CreateTrivialMockJob(ALWAYS);
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  EXPECT_CALL(*job, ShouldStop())
      .Times(1)
      .WillOnce(Return(true));
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(BadExit));

  manager_->Run();
}

static const pid_t kDummyPid = 4;
TEST_F(SessionManagerTest, SessionNotStartedCleanupTest) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  manager_->test_api().set_child_pid(0, kDummyPid);

  int timeout = 3;
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGKILL))
      .WillOnce(Return(0));
  EXPECT_CALL(*(utils_.get()), ChildIsGone(kDummyPid, timeout))
      .WillOnce(Return(true));
  MockUtils();

  manager_->test_api().CleanupChildren(timeout);
}

TEST_F(SessionManagerTest, SessionNotStartedSlowKillCleanupTest) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  manager_->test_api().set_child_pid(0, kDummyPid);

  int timeout = 3;
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGKILL))
      .WillOnce(Return(0));
  EXPECT_CALL(*(utils_.get()), ChildIsGone(kDummyPid, timeout))
      .WillOnce(Return(false));
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGABRT))
      .WillOnce(Return(0));
  MockUtils();

  manager_->test_api().CleanupChildren(timeout);
}

TEST_F(SessionManagerTest, SessionStartedCleanupTest) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  manager_->test_api().set_child_pid(0, kDummyPid);

  gboolean out;
  gchar email[] = "user@somewhere";
  gchar nothing[] = "";
  int timeout = 3;
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGTERM))
      .WillOnce(Return(0));
  EXPECT_CALL(*(utils_.get()), ChildIsGone(kDummyPid, timeout))
      .WillOnce(Return(true));
  MockUtils();

  std::string email_string(email);
  EXPECT_CALL(*job, StartSession(email_string))
      .Times(1);

  manager_->StartSession(email, nothing, &out, NULL);
  manager_->test_api().CleanupChildren(timeout);
}

TEST_F(SessionManagerTest, SessionStartedSlowKillCleanupTest) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  SessionManagerService::TestApi test_api = manager_->test_api();
  test_api.set_child_pid(0, kDummyPid);

  int timeout = 3;
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGTERM))
      .WillOnce(Return(0));
  EXPECT_CALL(*(utils_.get()), ChildIsGone(kDummyPid, timeout))
      .WillOnce(Return(false));
  EXPECT_CALL(*(utils_.get()), kill(kDummyPid, SIGABRT))
      .WillOnce(Return(0));
  MockUtils();

  gboolean out;
  gchar email[] = "user@somewhere";
  gchar nothing[] = "";

  std::string email_string(email);
  EXPECT_CALL(*job, StartSession(email_string))
      .Times(1);

  manager_->StartSession(email, nothing, &out, NULL);
  test_api.CleanupChildren(timeout);
}

static void Sleep() { execl("/bin/sleep", "sleep", "10000", NULL); }
TEST_F(SessionManagerTest, SessionStartedSigTermTest) {
  int pid = fork();
  if (pid == 0) {
    MockChildJob* job = CreateTrivialMockJob(EXACTLY_ONCE);

    gboolean out;
    gchar email[] = "user@somewhere";
    gchar nothing[] = "";
    std::string email_string(email);

    EXPECT_CALL(*job, RecordTime())
        .Times(1);
    ON_CALL(*job, Run())
        .WillByDefault(Invoke(Sleep));
    EXPECT_CALL(*job, StartSession(email_string))
        .Times(1);

    manager_->StartSession(email, nothing, &out, NULL);
    manager_->Run();
    delete manager_;
    manager_ = NULL;
    exit(0);
  }
  sleep(1);
  kill(pid, SIGTERM);
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;

  DLOG(INFO) << "exited waitpid. " << pid << "\n"
             << "  WIFSIGNALED is " << WIFSIGNALED(status) << "\n"
             << "  WTERMSIG is " << WTERMSIG(status) << "\n"
             << "  WIFEXITED is " << WIFEXITED(status) << "\n"
             << "  WEXITSTATUS is " << WEXITSTATUS(status);

  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

TEST_F(SessionManagerTest, StartSessionTest) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);

  gboolean out;
  gchar email[] = "user@somewhere";
  gchar nothing[] = "";
  std::string email_string(email);
  EXPECT_CALL(*job, StartSession(email_string))
      .Times(1);
  manager_->StartSession(email, nothing, &out, NULL);
}

TEST_F(SessionManagerTest, StartSessionErrorTest) {
  MockChildJob* job = CreateTrivialMockJob(ALWAYS);
  gboolean out;
  gchar email[] = "user";
  gchar nothing[] = "";
  GError* error = NULL;
  manager_->StartSession(email, nothing, &out, &error);
  EXPECT_EQ(CHROMEOS_LOGIN_ERROR_INVALID_EMAIL, error->code);
  g_error_free(error);
}

TEST_F(SessionManagerTest, StopSessionTest) {
  MockChildJob* job = CreateTrivialMockJob(ALWAYS);
  gboolean out;
  gchar nothing[] = "";
  manager_->StopSession(nothing, &out, NULL);
}

static const std::string name() { return "foo"; }

TEST_F(SessionManagerTest, StatsRecorded) {
  FilePath uptime(kUptimeFile1);
  FilePath disk(kDiskFile1);
  file_util::Delete(uptime, false);
  file_util::Delete(disk, false);
  MockChildJob* job = CreateTrivialMockJob(EXACTLY_ONCE);
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(CleanExit));
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  ON_CALL(*job, GetName())
      .WillByDefault(Invoke(name));
  manager_->Run();
  EXPECT_TRUE(file_util::PathExists(uptime));
  EXPECT_TRUE(file_util::PathExists(disk));
}

class SadNssUtil : public MockNssUtil {
 public:
  SadNssUtil() : MockNssUtil() {
    EXPECT_CALL(*this, OpenUserDB())
        .WillOnce(Return(false));
  }
  virtual ~SadNssUtil() {}
};

TEST_F(SessionManagerTest, SetOwnerKeyNssDbFail) {
  MockFactory<SadNssUtil> factory;
  NssUtil::set_factory(&factory);

  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->SetOwnerKey(fake_key_, &error));
}

class KeyFailUtil : public MockNssUtil {
 public:
  KeyFailUtil() : MockNssUtil() {
    EXPECT_CALL(*this, OpenUserDB())
        .WillOnce(Return(true));
    EXPECT_CALL(*this, CheckOwnerKey(_))
        .WillOnce(Return(false));
  }
  virtual ~KeyFailUtil() {}
};

TEST_F(SessionManagerTest, SetOwnerKeyCheckFail) {
  MockFactory<KeyFailUtil> factory;
  NssUtil::set_factory(&factory);

  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->SetOwnerKey(fake_key_, &error));
}

class KeyCheckUtil : public MockNssUtil {
 public:
  KeyCheckUtil() : MockNssUtil() {
    EXPECT_CALL(*this, OpenUserDB())
        .WillOnce(Return(true));
    EXPECT_CALL(*this, CheckOwnerKey(_))
        .WillOnce(Return(true));
  }
  virtual ~KeyCheckUtil() {}
};

TEST_F(SessionManagerTest, SetOwnerKeyPopulateFail) {
  MockFactory<KeyCheckUtil> factory;
  NssUtil::set_factory(&factory);

  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  std::vector<uint8> pub_key;
  NssUtil::KeyFromBuffer(fake_key_, &pub_key);

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromBuffer(pub_key))
      .WillOnce(Return(false));

  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->SetOwnerKey(fake_key_, &error));
}

TEST_F(SessionManagerTest, InitiateSetOwnerKey) {
  MockFactory<KeyCheckUtil> factory;
  NssUtil::set_factory(&factory);

  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  std::vector<uint8> pub_key;
  NssUtil::KeyFromBuffer(fake_key_, &pub_key);

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromBuffer(pub_key))
      .WillOnce(Return(true));

  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->SetOwnerKey(fake_key_, &error));
}

TEST_F(SessionManagerTest, SetOwnerKey) {
  MockFactory<KeyCheckUtil> factory;
  NssUtil::set_factory(&factory);

  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  std::vector<uint8> pub_key;
  NssUtil::KeyFromBuffer(fake_key_, &pub_key);

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, PopulateFromBuffer(pub_key))
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Persist())
      .WillOnce(Return(true));

  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->SetOwnerKey(fake_key_, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, WhitelistNoKey) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(false));
  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->Whitelist(kFakeEmail, fake_sig_, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, WhitelistVerifyFail) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Verify(kFakeEmail, strlen(kFakeEmail),
                           fake_sig_->data, fake_sig_->len))
      .WillOnce(Return(false));
  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->Whitelist(kFakeEmail, fake_sig_, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, Whitelist) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Verify(kFakeEmail, strlen(kFakeEmail),
                           fake_sig_->data, fake_sig_->len))
      .WillOnce(Return(true));

  manager_->test_api().set_ownerkey(key);

  EXPECT_CALL(*store_, Whitelist(kFakeEmail, _))
      .Times(1);
  EXPECT_CALL(*store_, Persist())
      .WillOnce(Return(true))
      .WillOnce(Return(true));

  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->Whitelist(kFakeEmail, fake_sig_, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, Unwhitelist) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Verify(kFakeEmail, strlen(kFakeEmail),
                           fake_sig_->data, fake_sig_->len))
      .WillOnce(Return(true));

  manager_->test_api().set_ownerkey(key);

  EXPECT_CALL(*store_, Unwhitelist(kFakeEmail))
      .Times(1);
  EXPECT_CALL(*store_, Persist())
      .WillOnce(Return(true))
      .WillOnce(Return(true));

  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->Unwhitelist(kFakeEmail, fake_sig_, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, CheckWhitelistFail) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  manager_->test_api().set_ownerkey(key);

  EXPECT_CALL(*store_, GetFromWhitelist(kFakeEmail, _))
      .WillOnce(Return(false));
  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  GArray* out_sig = NULL;
  EXPECT_EQ(FALSE, manager_->CheckWhitelist(kFakeEmail, &out_sig, &error));

  manager_->Run();
}

TEST_F(SessionManagerTest, CheckWhitelist) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  manager_->test_api().set_ownerkey(key);

  MockPrefStore* store = new MockPrefStore;
  EXPECT_CALL(*store_, GetFromWhitelist(kFakeEmail, _))
      .WillOnce(DoAll(SetArgumentPointee<1>(fake_sig_encoded_),
                      Return(true)))
      .RetiresOnSaturation();
  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  GArray* out_sig = NULL;
  ASSERT_EQ(TRUE, manager_->CheckWhitelist(kFakeEmail, &out_sig, &error));

  int i = 0;
  for (i = 0; i < fake_sig_->len; i++) {
    EXPECT_EQ(g_array_index(fake_sig_, uint8, i),
              g_array_index(out_sig, uint8, i));
  }
  EXPECT_EQ(i, fake_sig_->len);
  manager_->Run();
}

TEST_F(SessionManagerTest, StorePropertyNoKey) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(false));
  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->StoreProperty(kPropName,
                                           kPropValue,
                                           fake_sig_,
                                           &error));
  manager_->Run();
}

TEST_F(SessionManagerTest, StorePropertyVerifyFail) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Verify(Eq(property_), property_.length(),
                           fake_sig_->data, fake_sig_->len))
      .WillOnce(Return(false));
  manager_->test_api().set_ownerkey(key);

  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->StoreProperty(kPropName,
                                           kPropValue,
                                           fake_sig_,
                                           &error));
  manager_->Run();
}

TEST_F(SessionManagerTest, StoreProperty) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, IsPopulated())
      .WillOnce(Return(true));
  EXPECT_CALL(*key, Verify(Eq(property_), property_.length(),
                           fake_sig_->data, fake_sig_->len))
      .WillOnce(Return(true));
  manager_->test_api().set_ownerkey(key);

  EXPECT_CALL(*store_, Set(kPropName, kPropValue, _))
      .Times(1);
  EXPECT_CALL(*store_, Persist())
      .WillOnce(Return(true))
      .WillOnce(Return(true));
  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->StoreProperty(kPropName,
                                          kPropValue,
                                          fake_sig_,
                                          &error));
  manager_->Run();
}

TEST_F(SessionManagerTest, RetrievePropertyFail) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  manager_->test_api().set_ownerkey(key);

  EXPECT_CALL(*store_, Get(kPropName, _, _))
      .WillOnce(Return(false));
  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  GArray* out_sig = NULL;
  gchar* out_value = NULL;
  EXPECT_EQ(FALSE, manager_->RetrieveProperty(kPropName,
                                              &out_value,
                                              &out_sig,
                                              &error));
  manager_->Run();
}

TEST_F(SessionManagerTest, RetrieveProperty) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();

  MockOwnerKey* key = new MockOwnerKey;
  EXPECT_CALL(*key, PopulateFromDiskIfPossible())
      .WillOnce(Return(true));
  manager_->test_api().set_ownerkey(key);

  std::string value(kPropValue);
  EXPECT_CALL(*store_, Get(kPropName, _, _))
      .WillOnce(DoAll(SetArgumentPointee<1>(value),
                      SetArgumentPointee<2>(fake_sig_encoded_),
                      Return(true)))
      .RetiresOnSaturation();
  manager_->test_api().set_prefstore(store_);

  GError* error = NULL;
  GArray* out_sig = NULL;
  gchar* out_value = NULL;
  EXPECT_EQ(TRUE, manager_->RetrieveProperty(kPropName,
                                             &out_value,
                                             &out_sig,
                                             &error));
  int i = 0;
  for (i = 0; i < fake_sig_->len; i++) {
    EXPECT_EQ(g_array_index(fake_sig_, uint8, i),
              g_array_index(out_sig, uint8, i));
  }
  EXPECT_EQ(i, fake_sig_->len);
  manager_->Run();
}

TEST_F(SessionManagerTest, RestartJobUnknownPid) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  MockUtils();
  SessionManagerService::TestApi test_api = manager_->test_api();
  test_api.set_child_pid(0, kDummyPid);

  gboolean out;
  gint pid = kDummyPid + 1;
  gchar arguments[] = "";
  GError* error = NULL;
  EXPECT_EQ(FALSE, manager_->RestartJob(pid, arguments, &out, &error));
  EXPECT_EQ(CHROMEOS_LOGIN_ERROR_UNKNOWN_PID, error->code);
  EXPECT_EQ(FALSE, out);
}

TEST_F(SessionManagerTest, RestartJob) {
  MockChildJob* job = CreateTrivialMockJob(MAYBE_NEVER);
  SessionManagerService::TestApi test_api = manager_->test_api();
  test_api.set_child_pid(0, kDummyPid);
  EXPECT_CALL(*(utils_.get()), kill(-kDummyPid, SIGKILL))
      .WillOnce(Return(0));
  MockUtils();

  EXPECT_CALL(*job, SetArguments("dummy"))
      .Times(1);
  EXPECT_CALL(*job, RecordTime())
      .Times(1);
  ON_CALL(*job, Run())
      .WillByDefault(Invoke(CleanExit));

  gboolean out;
  gint pid = kDummyPid;
  gchar arguments[] = "dummy";
  GError* error = NULL;
  EXPECT_EQ(TRUE, manager_->RestartJob(pid, arguments, &out, &error));
  EXPECT_EQ(TRUE, out);
}

TEST(SessionManagerTestStatic, EmailAddressTest) {
  const char valid[] = "user@somewhere";
  EXPECT_TRUE(login_manager::SessionManagerService::ValidateEmail(valid));
}

TEST(SessionManagerTestStatic, EmailAddressNonAsciiTest) {
  char invalid[4] = "a@m";
  invalid[2] = 254;
  EXPECT_FALSE(login_manager::SessionManagerService::ValidateEmail(invalid));
}

TEST(SessionManagerTestStatic, EmailAddressNoAtTest) {
  const char no_at[] = "user";
  EXPECT_FALSE(login_manager::SessionManagerService::ValidateEmail(no_at));
}

TEST(SessionManagerTestStatic, EmailAddressTooMuchAtTest) {
  const char extra_at[] = "user@what@where";
  EXPECT_FALSE(login_manager::SessionManagerService::ValidateEmail(extra_at));
}

TEST(SessionManagerTestStatic, GetArgLists0) {
  std::vector<std::string> args;
  std::vector<std::vector<std::string> > arg_lists =
      SessionManagerService::GetArgLists(args);
  EXPECT_EQ(0, arg_lists.size());
}

static std::vector<std::vector<std::string> > GetArgs(const char** c_args) {
  std::vector<std::string> args;
  while (*c_args) {
    args.push_back(*c_args);
    c_args++;
  }
  return SessionManagerService::GetArgLists(args);
}

TEST(SessionManagerTestStatic, GetArgLists1) {
  const char* c_args[] = {"a", "b", "c", NULL};
  std::vector<std::vector<std::string> > arg_lists = GetArgs(c_args);
  EXPECT_EQ(1, arg_lists.size());
  EXPECT_EQ(3, arg_lists[0].size());
}

TEST(SessionManagerTestStatic, GetArgLists2) {
  const char* c_args[] = {"a", "b", "c", "--", "d", NULL};
  std::vector<std::vector<std::string> > arg_lists = GetArgs(c_args);
  EXPECT_EQ(2, arg_lists.size());
  EXPECT_EQ(3, arg_lists[0].size());
  EXPECT_EQ(1, arg_lists[1].size());
}

TEST(SessionManagerTestStatic, GetArgLists_TrailingDashes) {
  const char* c_args[] = {"a", "b", "c", "--", NULL};
  std::vector<std::vector<std::string> > arg_lists = GetArgs(c_args);
  EXPECT_EQ(1, arg_lists.size());
  EXPECT_EQ(3, arg_lists[0].size());
}

TEST(SessionManagerTestStatic, GetArgLists3_InitialDashes) {
  const char* c_args[] = {"--", "a", "b", "c", NULL};
  std::vector<std::vector<std::string> > arg_lists = GetArgs(c_args);
  EXPECT_EQ(1, arg_lists.size());
  EXPECT_EQ(3, arg_lists[0].size());
}

}  // namespace login_manager
