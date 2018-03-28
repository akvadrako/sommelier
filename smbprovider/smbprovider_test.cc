// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include <dbus/mock_bus.h>
#include <dbus/object_path.h>
#include <gtest/gtest.h>

#include "smbprovider/fake_samba_interface.h"
#include "smbprovider/iterator/directory_iterator.h"
#include "smbprovider/mount_manager.h"
#include "smbprovider/proto_bindings/directory_entry.pb.h"
#include "smbprovider/smbprovider.h"
#include "smbprovider/smbprovider_helper.h"
#include "smbprovider/smbprovider_test_helper.h"

using brillo::dbus_utils::DBusObject;

namespace smbprovider {

namespace {

constexpr size_t kFileSize = 10;
constexpr uint64_t kFileDate = 42;

ErrorType CastError(int error) {
  EXPECT_GE(error, 0);
  return static_cast<ErrorType>(error);
}

void ValidateFDContent(int fd,
                       int32_t length_to_read,
                       std::vector<uint8_t>::const_iterator data_start_iterator,
                       std::vector<uint8_t>::const_iterator data_end_iterator) {
  EXPECT_EQ(length_to_read,
            std::distance(data_start_iterator, data_end_iterator));
  std::vector<uint8_t> buffer(length_to_read);
  EXPECT_TRUE(base::ReadFromFD(fd, reinterpret_cast<char*>(buffer.data()),
                               buffer.size()));
  EXPECT_TRUE(std::equal(data_start_iterator, data_end_iterator, buffer.begin(),
                         buffer.end()));
}

// Reads the temp file |fd| into a buffer, then parses the buffer into a
// DeleteListProto.
DeleteListProto GetDeleteListProtoFromFD(int fd, int32_t length_to_read) {
  std::vector<uint8_t> buffer(length_to_read);
  EXPECT_TRUE(base::ReadFromFD(fd, reinterpret_cast<char*>(buffer.data()),
                               buffer.size()));

  DeleteListProto delete_list;
  EXPECT_TRUE(delete_list.ParseFromArray(buffer.data(), buffer.size()));

  return delete_list;
}

DirectoryEntryListProto GetDirectoryEntryListProtoFromBlob(
    const ProtoBlob& blob) {
  DirectoryEntryListProto entries;
  EXPECT_TRUE(entries.ParseFromArray(blob.data(), blob.size()));

  return entries;
}

}  // namespace

class SmbProviderTest : public testing::Test {
 public:
  SmbProviderTest() { SetUpSmbProvider(); }
  ~SmbProviderTest() override = default;

 protected:
  using DirEntries = std::vector<smbc_dirent>;

  // Helper method that adds "smb://wdshare/test" as a mountable share and
  // mounts it.
  int32_t PrepareMount() {
    fake_samba_->AddDirectory(GetDefaultServer());
    fake_samba_->AddDirectory(GetDefaultMountRoot());
    int32_t mount_id;
    int32_t err;
    ProtoBlob proto_blob = CreateMountOptionsBlob(GetDefaultMountRoot());
    smbprovider_->Mount(proto_blob, &err, &mount_id);
    EXPECT_EQ(ERROR_OK, CastError(err));
    ExpectNoOpenEntries();
    return mount_id;
  }

  // Helper method that calls PrepareMount() and adds a single directory with a
  // single file in the mount.
  int32_t PrepareSingleFileMount() {
    const int32_t mount_id = PrepareMount();
    fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
    fake_samba_->AddFile(GetAddedFullFilePath());
    return mount_id;
  }

  // Helper method that calls PrepareMount() and adds a single directory with a
  // single file in the mount. |file_data| is the data that will be in the file.
  int32_t PrepareSingleFileMountWithData(std::vector<uint8_t> file_data) {
    const int32_t mount_id = PrepareMount();
    fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
    fake_samba_->AddFile(GetAddedFullFilePath(), kFileDate,
                         std::move(file_data));
    return mount_id;
  }

  // Helper method that opens an already added file located in
  // GetDefaultFilePath(). PrepareSingleFileMount() or
  // PrepareSingleFileMountWithData() must be called beforehand.
  int32_t OpenAddedFile(int32_t mount_id) {
    return OpenAddedFile(mount_id, GetDefaultFilePath());
  }

  // Helper method that opens an already added file located in |path|.
  // PrepareSingleFileMount() or PrepareSingleFileMountWithData() must be called
  // beforehand.
  int32_t OpenAddedFile(int32_t mount_id, const std::string& path) {
    return OpenAddedFile(mount_id, path, false);
  }

  // Helper method that opens an already added file located in |path|.
  // PrepareSingleFileMount() or PrepareSingleFileMountWithData() must be called
  // beforehand. Permissions will be O_RDWR if |writeable| is true, otherwise it
  // will be O_RDONLY.
  int32_t OpenAddedFile(int32_t mount_id,
                        const std::string& path,
                        bool writeable) {
    int32_t file_id;
    int32_t error_code;
    ProtoBlob open_file_blob =
        CreateOpenFileOptionsBlob(mount_id, path, writeable);
    smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);
    DCHECK_EQ(static_cast<int32_t>(ERROR_OK), error_code);
    return file_id;
  }

  // Helper method that opens an already added directory located in |path|.
  // Returns the directory id.
  int32_t OpenAddedDirectory(const std::string& path) {
    int32_t dir_id;
    int32_t error_code = fake_samba_->OpenDirectory(path, &dir_id);
    DCHECK_EQ(0, error_code);
    return dir_id;
  }

  void SetUpSmbProvider() {
    std::unique_ptr<FakeSambaInterface> fake_ptr =
        std::make_unique<FakeSambaInterface>();
    fake_samba_ = fake_ptr.get();
    std::unique_ptr<MountManager> mount_manager_ptr =
        std::make_unique<MountManager>();
    mount_manager_ = mount_manager_ptr.get();
    const dbus::ObjectPath object_path("/object/path");
    smbprovider_ = std::make_unique<SmbProvider>(
        std::make_unique<DBusObject>(nullptr, mock_bus_, object_path),
        std::move(fake_ptr), std::move(mount_manager_ptr));
  }

  // Helper method that asserts there are no entries that have not been
  // closed.
  void ExpectNoOpenEntries() { EXPECT_FALSE(fake_samba_->HasOpenEntries()); }

  // Helper method that creates a CloseFileOptionsBlob for |file_id| and
  // calls SmbProvider::CloseFile with it, expecting success.
  void CloseFileHelper(int32_t mount_id, int32_t file_id) {
    ProtoBlob close_file_blob = CreateCloseFileOptionsBlob(mount_id, file_id);
    EXPECT_EQ(ERROR_OK, smbprovider_->CloseFile(close_file_blob));
  }

  // Helper method to read a file using the given options, and outputs a file
  // descriptor |fd|.
  void ReadFile(int32_t mount_id,
                int32_t file_id,
                int64_t offset,
                int32_t length,
                brillo::dbus_utils::FileDescriptor* fd) {
    int32_t err;
    ProtoBlob read_file_blob =
        CreateReadFileOptionsBlob(mount_id, file_id, offset, length);
    smbprovider_->ReadFile(read_file_blob, &err, fd);
    EXPECT_EQ(ERROR_OK, CastError(err));
  }

  void WriteToTempFileWithData(const std::vector<uint8_t>& data,
                               base::ScopedFD* fd) {
    EXPECT_GT(0, fd->get());
    fd->reset(temp_file_manager_.CreateTempFile(data).release());
    EXPECT_LE(0, fd->get());
  }

  scoped_refptr<dbus::MockBus> mock_bus_ =
      new dbus::MockBus(dbus::Bus::Options());
  std::unique_ptr<SmbProvider> smbprovider_;
  FakeSambaInterface* fake_samba_;
  MountManager* mount_manager_;
  TempFileManager temp_file_manager_;

 private:
  DISALLOW_COPY_AND_ASSIGN(SmbProviderTest);
};

// Should properly serialize protobuf.
TEST_F(SmbProviderTest, ShouldSerializeProto) {
  const std::string path("smb://192.168.0.1/test");
  MountOptionsProto mount_options;
  mount_options.set_path(path);
  ProtoBlob buffer;
  EXPECT_EQ(ERROR_OK, SerializeProtoToBlob(mount_options, &buffer));
  EXPECT_EQ(mount_options.ByteSizeLong(), buffer.size());

  MountOptionsProto deserialized_proto;
  EXPECT_TRUE(deserialized_proto.ParseFromArray(buffer.data(), buffer.size()));
  EXPECT_EQ(path, deserialized_proto.path());
}

// Mount fails when an invalid protobuf with missing fields is passed.
TEST_F(SmbProviderTest, MountFailsWithInvalidProto) {
  int32_t mount_id;
  int32_t err;
  ProtoBlob empty_blob;
  smbprovider_->Mount(empty_blob, &err, &mount_id);
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(err));
  EXPECT_EQ(0, mount_manager_->MountCount());
  ExpectNoOpenEntries();
}

// Mount fails when mounting a share that doesn't exist.
TEST_F(SmbProviderTest, MountFailsWithInvalidShare) {
  int32_t mount_id;
  int32_t err;
  ProtoBlob proto_blob = CreateMountOptionsBlob("smb://test/invalid");
  smbprovider_->Mount(proto_blob, &err, &mount_id);
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(err));
  EXPECT_EQ(0, mount_manager_->MountCount());
  ExpectNoOpenEntries();
}

// Unmount fails when an invalid protobuf with missing fields is passed.
TEST_F(SmbProviderTest, UnmountFailsWithInvalidProto) {
  ProtoBlob empty_blob;
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->Unmount(empty_blob)));
}

// Unmount fails when unmounting an |mount_id| that wasn't previously mounted.
TEST_F(SmbProviderTest, UnmountFailsWithUnmountedShare) {
  ProtoBlob proto_blob = CreateUnmountOptionsBlob(123);
  int32_t error = smbprovider_->Unmount(proto_blob);
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(error));
  ExpectNoOpenEntries();
}

// Mounting different shares should return different mount ids.
TEST_F(SmbProviderTest, MountReturnsDifferentMountIds) {
  fake_samba_->AddDirectory("smb://wdshare");
  fake_samba_->AddDirectory("smb://wdshare/dogs");
  fake_samba_->AddDirectory("smb://wdshare/cats");

  int32_t error;
  int32_t mount1_id;
  ProtoBlob proto_blob_1 = CreateMountOptionsBlob("smb://wdshare/dogs");
  smbprovider_->Mount(proto_blob_1, &error, &mount1_id);
  EXPECT_EQ(ERROR_OK, error);
  EXPECT_EQ(1, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount1_id));

  int32_t mount2_id;
  ProtoBlob proto_blob_2 = CreateMountOptionsBlob("smb://wdshare/cats");
  smbprovider_->Mount(proto_blob_2, &error, &mount2_id);
  EXPECT_EQ(ERROR_OK, error);
  EXPECT_EQ(2, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount2_id));

  EXPECT_NE(mount1_id, mount2_id);
}

// Mount and unmount succeed when mounting a valid share path and unmounting
// using the |mount_id| from |Mount|.
TEST_F(SmbProviderTest, MountUnmountSucceedsWithValidShare) {
  int32_t mount_id = PrepareMount();
  EXPECT_GE(mount_id, 0);
  ExpectNoOpenEntries();
  EXPECT_EQ(1, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount_id));

  ProtoBlob proto_blob = CreateUnmountOptionsBlob(mount_id);
  int32_t error = smbprovider_->Unmount(proto_blob);
  EXPECT_EQ(ERROR_OK, CastError(error));
  ExpectNoOpenEntries();
  EXPECT_EQ(0, mount_manager_->MountCount());
  EXPECT_FALSE(mount_manager_->IsAlreadyMounted(mount_id));
}

// Mount ids should not be reused.
TEST_F(SmbProviderTest, MountIdsDontGetReused) {
  fake_samba_->AddDirectory("smb://wdshare");
  fake_samba_->AddDirectory("smb://wdshare/dogs");

  // Create the first mount.
  int32_t error;
  int32_t mount_id_1;
  ProtoBlob mount_options_blob_1 = CreateMountOptionsBlob("smb://wdshare/dogs");
  smbprovider_->Mount(mount_options_blob_1, &error, &mount_id_1);
  EXPECT_EQ(1, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount_id_1));

  // Unmount the original mount.
  ProtoBlob unmount_options_blob = CreateUnmountOptionsBlob(mount_id_1);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->Unmount(unmount_options_blob)));
  EXPECT_EQ(0, mount_manager_->MountCount());
  EXPECT_FALSE(mount_manager_->IsAlreadyMounted(mount_id_1));

  // Mount a second mount and verify it got a new id.
  int32_t mount_id_2;
  ProtoBlob mount_options_blob_2 = CreateMountOptionsBlob("smb://wdshare/dogs");
  smbprovider_->Mount(mount_options_blob_2, &error, &mount_id_2);
  EXPECT_EQ(1, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount_id_2));
  EXPECT_NE(mount_id_1, mount_id_2);
}

// ReadDirectory fails when an invalid protobuf with missing fields is passed.
TEST_F(SmbProviderTest, ReadDirectoryFailsWithInvalidProto) {
  int32_t err;
  ProtoBlob results;
  ProtoBlob empty_blob;
  smbprovider_->ReadDirectory(empty_blob, &err, &results);
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(err));
  EXPECT_TRUE(results.empty());
}

// ReadDirectory fails when passed a |mount_id| that wasn't previously mounted.
TEST_F(SmbProviderTest, ReadDirectoryFailsWithUnmountedShare) {
  ProtoBlob results;
  int32_t err;
  ProtoBlob read_directory_blob = CreateReadDirectoryOptionsBlob(
      999 /* mount_id */, GetAddedFullDirectoryPath());
  smbprovider_->ReadDirectory(read_directory_blob, &err, &results);
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(err));
  ExpectNoOpenEntries();
}

// Read directory fails when passed a path that doesn't exist.
TEST_F(SmbProviderTest, ReadDirectoryFailsWithInvalidDir) {
  int32_t mount_id = PrepareMount();

  ProtoBlob results;
  int32_t err;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, "/test/invalid");
  smbprovider_->ReadDirectory(read_directory_blob, &err, &results);
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(err));
}

// ReadDirectory succeeds when reading an empty directory.
TEST_F(SmbProviderTest, ReadDirectorySucceedsWithEmptyDir) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  ProtoBlob results;
  int32_t err;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, GetDefaultDirectoryPath());
  smbprovider_->ReadDirectory(read_directory_blob, &err, &results);

  DirectoryEntryListProto entries;
  const std::string parsed_proto(results.begin(), results.end());
  EXPECT_TRUE(entries.ParseFromString(parsed_proto));

  EXPECT_EQ(ERROR_OK, CastError(err));
  EXPECT_EQ(0, entries.entries_size());
  ExpectNoOpenEntries();
}

// Read directory succeeds and returns expected entries.
TEST_F(SmbProviderTest, ReadDirectorySucceedsWithNonEmptyDir) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  fake_samba_->AddFile(GetDefaultFullPath("/path/file.jpg"), kFileSize);
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/images"));

  ProtoBlob results;
  int32_t error_code;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, GetDefaultDirectoryPath());
  smbprovider_->ReadDirectory(read_directory_blob, &error_code, &results);

  DirectoryEntryListProto entries;
  const std::string parsed_proto(results.begin(), results.end());
  EXPECT_TRUE(entries.ParseFromString(parsed_proto));

  EXPECT_EQ(ERROR_OK, CastError(error_code));
  EXPECT_EQ(2, entries.entries_size());

  const DirectoryEntryProto& entry1 = entries.entries(0);
  EXPECT_FALSE(entry1.is_directory());
  EXPECT_EQ("file.jpg", entry1.name());

  const DirectoryEntryProto& entry2 = entries.entries(1);
  EXPECT_TRUE(entry2.is_directory());
  EXPECT_EQ("images", entry2.name());
}

// Read directory succeeds and omits "." and ".." entries.
TEST_F(SmbProviderTest, ReadDirectoryDoesntReturnSelfAndParentEntries) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  fake_samba_->AddFile(GetDefaultFullPath("/path/file.jpg"), kFileSize);
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/."));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/.."));

  ProtoBlob results;
  int32_t error_code;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, GetDefaultDirectoryPath());
  smbprovider_->ReadDirectory(read_directory_blob, &error_code, &results);

  DirectoryEntryListProto entries;
  const std::string parsed_proto(results.begin(), results.end());
  EXPECT_TRUE(entries.ParseFromString(parsed_proto));

  EXPECT_EQ(ERROR_OK, CastError(error_code));
  EXPECT_EQ(1, entries.entries_size());

  const DirectoryEntryProto& entry = entries.entries(0);
  EXPECT_FALSE(entry.is_directory());
  EXPECT_EQ("file.jpg", entry.name());
}

// Read directory fails when called on a file.
TEST_F(SmbProviderTest, ReadDirectoryFailsWithFile) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath());

  ProtoBlob results;
  int32_t error_code;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, GetDefaultFilePath());
  smbprovider_->ReadDirectory(read_directory_blob, &error_code, &results);

  EXPECT_EQ(ERROR_NOT_A_DIRECTORY, CastError(error_code));
}

// Read directory fails when called on a non file.
TEST_F(SmbProviderTest, ReadDirectoryFailsWithNonFileNonDirectory) {
  int32_t mount_id = PrepareMount();
  const std::string printer_path = "/path/canon.cn";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddEntry(GetDefaultFullPath(printer_path), SMBC_PRINTER_SHARE);

  ProtoBlob results;
  int32_t error_code;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, printer_path);
  smbprovider_->ReadDirectory(read_directory_blob, &error_code, &results);

  EXPECT_EQ(ERROR_NOT_A_DIRECTORY, CastError(error_code));
}

// Read directory succeeds and omits non files / non directories.
TEST_F(SmbProviderTest, ReadDirectoryDoesNotReturnNonFileNonDir) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetDefaultFullPath("/path/file.jpg"));
  fake_samba_->AddEntry(GetDefaultFullPath("/path/canon.cn"),
                        SMBC_PRINTER_SHARE);

  ProtoBlob results;
  int32_t error_code;
  ProtoBlob read_directory_blob =
      CreateReadDirectoryOptionsBlob(mount_id, GetDefaultDirectoryPath());
  smbprovider_->ReadDirectory(read_directory_blob, &error_code, &results);

  DirectoryEntryListProto entries;
  const std::string parsed_proto(results.begin(), results.end());
  EXPECT_TRUE(entries.ParseFromString(parsed_proto));

  EXPECT_EQ(ERROR_OK, CastError(error_code));
  EXPECT_EQ(1, entries.entries_size());

  const DirectoryEntryProto& entry = entries.entries(0);
  EXPECT_FALSE(entry.is_directory());
  EXPECT_EQ("file.jpg", entry.name());
}

// GetMetadata fails on non files/dirs. Overall, other types
// are treated as if they do not exist.
TEST_F(SmbProviderTest, GetMetadataFailsWithNonFileNonDir) {
  int32_t mount_id = PrepareMount();
  const std::string printer_path = "/path/canon.cn";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddEntry(GetDefaultFullPath(printer_path), SMBC_PRINTER_SHARE);

  ProtoBlob result;
  int32_t error_code;
  ProtoBlob get_metadata_blob =
      CreateGetMetadataOptionsBlob(mount_id, printer_path);
  smbprovider_->GetMetadataEntry(get_metadata_blob, &error_code, &result);

  EXPECT_TRUE(result.empty());
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(error_code));
}

// GetMetadata fails when an invalid protobuf with missing fields is passed.
TEST_F(SmbProviderTest, GetMetadataFailsWithInvalidProto) {
  int32_t err;
  ProtoBlob result;
  ProtoBlob empty_blob;
  smbprovider_->GetMetadataEntry(empty_blob, &err, &result);
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(err));
  EXPECT_TRUE(result.empty());
}

// GetMetadata fails with when passed a |mount_id| that wasn't previously
// mounted.
TEST_F(SmbProviderTest, GetMetadataFailsWithUnmountedShare) {
  int32_t err;
  ProtoBlob result;
  ProtoBlob get_metadata_blob =
      CreateGetMetadataOptionsBlob(123, GetDefaultDirectoryPath());
  smbprovider_->GetMetadataEntry(get_metadata_blob, &err, &result);
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(err));
}

// GetMetadata fails when passed a path that doesn't exist.
TEST_F(SmbProviderTest, GetMetadataFailsWithInvalidPath) {
  int32_t mount_id = PrepareMount();

  ProtoBlob result;
  int32_t error_code;
  ProtoBlob get_metadata_blob =
      CreateGetMetadataOptionsBlob(mount_id, "/test/invalid");
  smbprovider_->GetMetadataEntry(get_metadata_blob, &error_code, &result);
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(error_code));
}

// GetMetadata succeeds when passed a valid share path.
TEST_F(SmbProviderTest, GetMetadataSucceeds) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  ProtoBlob result;
  int32_t error_code;
  ProtoBlob get_metadata_blob =
      CreateGetMetadataOptionsBlob(mount_id, GetDefaultFilePath());
  smbprovider_->GetMetadataEntry(get_metadata_blob, &error_code, &result);

  DirectoryEntryProto entry;
  const std::string parsed_proto(result.begin(), result.end());
  EXPECT_TRUE(entry.ParseFromString(parsed_proto));

  EXPECT_EQ(ERROR_OK, CastError(error_code));
  EXPECT_FALSE(entry.is_directory());
  EXPECT_EQ("dog.jpg", entry.name());
  EXPECT_EQ(kFileSize, entry.size());
  EXPECT_EQ(kFileDate, entry.last_modified_time());
}

// OpenFile fails when called on a non existent file.
TEST_F(SmbProviderTest, OpenFileFailsFileDoesNotExist) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  EXPECT_EQ(ERROR_NOT_FOUND, error_code);
  EXPECT_EQ(-1, file_id);
}

// OpenFile succeeds and returns a valid file_id when called on a valid file.
TEST_F(SmbProviderTest, OpenFileSucceedsOnValidFile) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  EXPECT_EQ(ERROR_OK, error_code);
  EXPECT_GE(file_id, 0);

  CloseFileHelper(mount_id, file_id);
}

// OpenFile fails when called on a directory.
TEST_F(SmbProviderTest, OpenFileFailsOnDirectory) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  EXPECT_EQ(ERROR_NOT_FOUND, error_code);
  EXPECT_EQ(-1, file_id);
}

// OpenFile fails when called on a non file / non directory.
TEST_F(SmbProviderTest, OpenFileFailsOnNonFileNonDir) {
  int32_t mount_id = PrepareMount();
  const std::string printer_path = "/path/canon.cn";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddEntry(GetDefaultFullPath(printer_path), SMBC_PRINTER_SHARE);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, printer_path, false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  EXPECT_EQ(ERROR_NOT_FOUND, error_code);
  EXPECT_EQ(-1, file_id);
}

// OpenFile sets read and write flags correctly in the corresponding OpenInfo.
TEST_F(SmbProviderTest, OpenFileReadandWriteFlagSetCorrectly) {
  int32_t mount_id = PrepareMount();
  const std::string file_path1 = "/path/dog.jpg";
  const std::string file_path2 = "/path/cat.jpg";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetDefaultFullPath(file_path1), kFileSize, kFileDate);
  fake_samba_->AddFile(GetDefaultFullPath(file_path2), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, file_path1, false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  int32_t file_id_2;
  int32_t error_code_2;
  open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, file_path2, true /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code_2, &file_id_2);

  EXPECT_EQ(ERROR_OK, error_code);
  EXPECT_TRUE(fake_samba_->HasReadSet(file_id));
  EXPECT_FALSE(fake_samba_->HasWriteSet(file_id));

  EXPECT_EQ(ERROR_OK, error_code_2);
  EXPECT_TRUE(fake_samba_->HasReadSet(file_id_2));
  EXPECT_TRUE(fake_samba_->HasWriteSet(file_id_2));

  CloseFileHelper(mount_id, file_id);
  CloseFileHelper(mount_id, file_id_2);
}

// CloseFile succeeds on a valid file.
TEST_F(SmbProviderTest, CloseFileSucceedsOnOpenFile) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);
  EXPECT_EQ(ERROR_OK, error_code);

  CloseFileHelper(mount_id, file_id);
}

// CloseFile closes the correct file when multiple files are open.
TEST_F(SmbProviderTest, CloseFileClosesCorrectFile) {
  int32_t mount_id = PrepareMount();
  const std::string file_path1 = "/path/dog.jpg";
  const std::string file_path2 = "/path/cat.jpg";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetDefaultFullPath(file_path1), kFileSize, kFileDate);
  fake_samba_->AddFile(GetDefaultFullPath(file_path2), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, file_path1, false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  int32_t file_id_2;
  int32_t error_code_2;
  open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, file_path2, false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code_2, &file_id_2);

  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id));
  EXPECT_FALSE(fake_samba_->IsDirectoryFDOpen(file_id));
  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id_2));
  EXPECT_FALSE(fake_samba_->IsDirectoryFDOpen(file_id_2));
  EXPECT_NE(file_id, file_id_2);

  CloseFileHelper(mount_id, file_id);
  EXPECT_FALSE(fake_samba_->IsFileFDOpen(file_id));
  EXPECT_FALSE(fake_samba_->IsDirectoryFDOpen(file_id));
  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id_2));
  EXPECT_FALSE(fake_samba_->IsDirectoryFDOpen(file_id_2));

  CloseFileHelper(mount_id, file_id_2);
}

// CloseFile closes the correct instance of a file with opened more than once.
TEST_F(SmbProviderTest, CloseFileClosesCorrectInstanceOfSameFile) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);

  int32_t file_id_2;
  int32_t error_code_2;
  open_file_blob = CreateOpenFileOptionsBlob(mount_id, GetDefaultFilePath(),
                                             false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code_2, &file_id_2);

  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id));
  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id_2));
  EXPECT_NE(file_id, file_id_2);

  CloseFileHelper(mount_id, file_id);
  EXPECT_FALSE(fake_samba_->IsFileFDOpen(file_id));
  EXPECT_TRUE(fake_samba_->IsFileFDOpen(file_id_2));

  CloseFileHelper(mount_id, file_id_2);
}

// CloseFile fails when called on a closed file.
TEST_F(SmbProviderTest, CloseFileFailsWhenFileIsNotOpen) {
  int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob = CreateOpenFileOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);
  EXPECT_EQ(ERROR_OK, error_code);
  CloseFileHelper(mount_id, file_id);

  ProtoBlob close_file_blob = CreateCloseFileOptionsBlob(mount_id, file_id);
  EXPECT_EQ(ERROR_NOT_FOUND, smbprovider_->CloseFile(close_file_blob));
}

// CloseFile fails when called with a non-existant file handler.
TEST_F(SmbProviderTest, CloseFileFailsOnNonExistantFileHandler) {
  ProtoBlob close_file_blob =
      CreateCloseFileOptionsBlob(1 /* mount_id */, 1564 /* file_id */);

  EXPECT_EQ(ERROR_NOT_FOUND, smbprovider_->CloseFile(close_file_blob));
}

// DeleteEntry succeeds when called without recursive on an empty directory.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnEmptyDirectory) {
  const int32_t mount_id = PrepareMount();
  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));
}

// DeleteEntry succeeds when called on a file.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnFile) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));
}

// DeleteEntry fails when called without recursive on a non-empty directory.
TEST_F(SmbProviderTest, DeleteEntryFailsWithoutRecursiveOnNonEmptyDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);
  EXPECT_EQ(ERROR_NOT_EMPTY, smbprovider_->DeleteEntry(delete_entry_blob));
}

// DeleteEntry fails when called on non-existent file or directory.
TEST_F(SmbProviderTest, DeleteEntryFailsOnNonExistentEntries) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);
  EXPECT_EQ(ERROR_NOT_FOUND, smbprovider_->DeleteEntry(delete_entry_blob));

  ProtoBlob delete_entry_blob_2 =
      CreateDeleteEntryOptionsBlob(mount_id, "/cat.png", false /* recursive */);
  EXPECT_EQ(ERROR_NOT_FOUND, smbprovider_->DeleteEntry(delete_entry_blob_2));
}

// DeleteEntry deletes the correct file.
TEST_F(SmbProviderTest, DeleteEntryDeletesCorrectFile) {
  const int32_t mount_id = PrepareMount();
  const std::string file_path1 = "/path/dog.jpg";
  const std::string file_path2 = "/path/cat.jpg";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetDefaultFullPath(file_path1), kFileSize, kFileDate);
  fake_samba_->AddFile(GetDefaultFullPath(file_path2), kFileSize, kFileDate);

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath(file_path1)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath(file_path2)));
}

// DeleteEntry deletes the correct directory.
TEST_F(SmbProviderTest, DeleteEntryDeletesCorrectDirectory) {
  const int32_t mount_id = PrepareMount();
  const std::string dir_path1 = "/path/dogs";
  const std::string dir_path2 = "/path/cats";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddDirectory(GetDefaultFullPath(dir_path1));
  fake_samba_->AddDirectory(GetDefaultFullPath(dir_path2));

  ProtoBlob delete_entry_blob =
      CreateDeleteEntryOptionsBlob(mount_id, dir_path1, false /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath(dir_path1)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath(dir_path2)));
}

// DeleteEntry should fail on a non-file, non-directory.
TEST_F(SmbProviderTest, DeleteEntryFailsOnNonFileNonDirectory) {
  const int32_t mount_id = PrepareMount();
  const std::string printer_path = "/path/canon.cn";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddEntry(GetDefaultFullPath(printer_path), SMBC_PRINTER_SHARE);

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, printer_path, false /* recursive */);
  EXPECT_EQ(ERROR_NOT_FOUND, smbprovider_->DeleteEntry(delete_entry_blob));
}

// DeleteEntry succeeds on an empty directory when called with the recusive
// flag.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnEmptyDirecotryWithRecursive) {
  const int32_t mount_id = PrepareMount();
  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetAddedFullDirectoryPath()));
}

// DeleteEntry succeeds on a file when called with the recurisve flag.
TEST_F(SmbProviderTest, DeleteEntrySuceedsOnFileWithRecursive) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddFile(GetAddedFullFilePath(), kFileSize, kFileDate);

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultFilePath(), true /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetAddedFullFilePath()));
}

// DeleteEntry succeeds on a directory of files.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnADirOfFiles) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/1.jpg"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/3.png"));

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/1.jpg")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/2.txt")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/3.png")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path")));
}

// DeleteEntry succeeds on multiple levels of nested directories.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnNestedEmptyDirectories) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs/lab"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/cats"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/cats/blue"));

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs/lab")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/cats/blue")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/cats")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path")));
}

// DeleteEntry succeeds on a dir with a file and a non-empty dir.
TEST_F(SmbProviderTest, DeleteEntrySucceedsOnADirWithAfileAndNonEmptyDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/dogs/1.jpg"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_OK, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(
      fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs/1.jpg")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/2.txt")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path")));
}

// DeleteEntry immediately fails as soon as an entry cannot be deleted.
TEST_F(SmbProviderTest, DeleteEntryFailsWhenAFileCannotBeDeleted) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/dogs/1.jpg"));
  fake_samba_->AddLockedFile(GetDefaultFullPath("/path/2.txt"));

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_ACCESS_DENIED, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_FALSE(
      fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs/1.jpg")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path/2.txt")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path")));
}

// DeleteEntry immediately fails as soon as a Directory cannot be opened.
TEST_F(SmbProviderTest, DeleteEntryFailsWhenADirectoryCannotBeOpened) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));

  ProtoBlob delete_entry_blob = CreateDeleteEntryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);
  EXPECT_EQ(ERROR_ACCESS_DENIED, smbprovider_->DeleteEntry(delete_entry_blob));

  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path/dogs")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path/2.txt")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path")));
}

// ReadFile fails when passed in invalid proto.
TEST_F(SmbProviderTest, ReadFileFailsWithInvalidProto) {
  int32_t err;
  ProtoBlob empty_blob;
  brillo::dbus_utils::FileDescriptor fd;

  smbprovider_->ReadFile(empty_blob, &err, &fd);

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(err));
  EXPECT_GE(0, fd.get());
}

// ReadFile fails when passed an invalid file descriptor.
TEST_F(SmbProviderTest, ReadFileFailsWithBadFD) {
  int32_t err;
  ProtoBlob blob = CreateReadFileOptionsBlob(0 /* mount_id */, -1 /* file_id */,
                                             0 /* offset */, 1 /* length */);
  brillo::dbus_utils::FileDescriptor fd;
  smbprovider_->ReadFile(blob, &err, &fd);

  EXPECT_NE(ERROR_OK, CastError(err));
  EXPECT_GE(0, fd.get());
}

// ReadFile fails when passed an unopened file descriptor.
TEST_F(SmbProviderTest, ReadFileFailsWithUnopenedFD) {
  int32_t err;
  ProtoBlob blob = CreateReadFileOptionsBlob(
      0 /* mount_id */, 100 /* file_id */, 0 /* offset */, 1 /* length */);
  brillo::dbus_utils::FileDescriptor fd;
  smbprovider_->ReadFile(blob, &err, &fd);

  EXPECT_NE(ERROR_OK, CastError(err));
  EXPECT_GE(0, fd.get());
}

// ReadFile fails when passed a negative offset.
TEST_F(SmbProviderTest, ReadFileFailsWithNegativeOffset) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t file_id = OpenAddedFile(mount_id);

  int32_t err;
  ProtoBlob blob = CreateReadFileOptionsBlob(mount_id, file_id, -1 /* offset */,
                                             1 /* length */);
  brillo::dbus_utils::FileDescriptor fd;
  smbprovider_->ReadFile(blob, &err, &fd);

  EXPECT_NE(ERROR_OK, CastError(err));
  EXPECT_GE(0, fd.get());
}

// ReadFile fails when passed a negative length.
TEST_F(SmbProviderTest, ReadFileFailsWithNegativeLength) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t file_id = OpenAddedFile(mount_id);

  int32_t err;
  ProtoBlob blob = CreateReadFileOptionsBlob(mount_id, file_id, 0 /* offset */,
                                             -1 /* length */);
  brillo::dbus_utils::FileDescriptor fd;
  smbprovider_->ReadFile(blob, &err, &fd);

  EXPECT_NE(ERROR_OK, CastError(err));
  EXPECT_GE(0, fd.get());
}

// ReadFile returns a valid file descriptor on success.
TEST_F(SmbProviderTest, ReadFileReturnsValidFileDescriptor) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t file_id = OpenAddedFile(mount_id);

  brillo::dbus_utils::FileDescriptor fd;
  ReadFile(mount_id, file_id, 0 /* offset */, file_data.size(), &fd);

  EXPECT_LT(0, fd.get());
  CloseFileHelper(mount_id, file_id);
}

// ReadFile should properly call Seek and ending offset for file should be
// (offset + length).
TEST_F(SmbProviderTest, ReadFileSeeksToOffset) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t file_id = OpenAddedFile(mount_id);

  const int64_t offset = 5;
  const int32_t length_to_read = 2;
  DCHECK_GT(file_data.size(), offset);
  DCHECK_GE(file_data.size(), offset + length_to_read);

  EXPECT_EQ(0, fake_samba_->GetFileOffset(file_id));

  brillo::dbus_utils::FileDescriptor fd;
  ReadFile(mount_id, file_id, offset, length_to_read, &fd);

  EXPECT_EQ(offset + length_to_read, fake_samba_->GetFileOffset(file_id));
  CloseFileHelper(mount_id, file_id);
}

// ReadFile should properly write the read bytes into a temporary file.
TEST_F(SmbProviderTest, ReadFileWritesTemporaryFile) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t file_id = OpenAddedFile(mount_id);

  const int64_t offset = 3;
  const int32_t length_to_read = 2;

  brillo::dbus_utils::FileDescriptor fd;
  ReadFile(mount_id, file_id, offset, length_to_read, &fd);

  // Compare the written value to the expected value.
  ValidateFDContent(fd.get(), length_to_read, file_data.begin() + offset,
                    file_data.begin() + offset + length_to_read);
  CloseFileHelper(mount_id, file_id);
}

// ReadFile should properly read the correct file when there are multiple
// files.
TEST_F(SmbProviderTest, ReadFileReadsCorrectFile) {
  const std::vector<uint8_t> file_data1 = {0, 1, 2, 3, 4, 5};
  const std::vector<uint8_t> file_data2 = {10, 11, 12, 13, 14, 15};
  const std::string file_path = "/path/cat.jpg";

  // PrepareSingleFileMountWithData() prepares a mount and adds a file in
  // GetDefaultFilePath().
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data1);

  // Add an additional file with different data.
  fake_samba_->AddFile(GetDefaultFullPath(file_path), kFileDate, file_data2);

  // Open both files.
  const int32_t file_id1 = OpenAddedFile(mount_id, GetDefaultFilePath());
  const int32_t file_id2 = OpenAddedFile(mount_id, file_path);
  EXPECT_NE(file_id1, file_id2);

  brillo::dbus_utils::FileDescriptor fd1;
  ReadFile(mount_id, file_id1, 0 /* offset */, file_data1.size(), &fd1);

  brillo::dbus_utils::FileDescriptor fd2;
  ReadFile(mount_id, file_id2, 0 /* offset */, file_data2.size(), &fd2);

  // Compare the written values to the expected values.
  ValidateFDContent(fd1.get(), file_data1.size(), file_data1.begin(),
                    file_data1.end());

  ValidateFDContent(fd2.get(), file_data2.size(), file_data2.begin(),
                    file_data2.end());

  // Close files.
  CloseFileHelper(mount_id, file_id1);
  CloseFileHelper(mount_id, file_id2);
}

// CreateFile fails when passed an invalid protobuf.
TEST_F(SmbProviderTest, CreateFileFailsWithInvalidProto) {
  ProtoBlob empty_blob;
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->CreateFile(empty_blob)));
}

// CreateFile fails when passed an invalid mount.
TEST_F(SmbProviderTest, CreateFileFailsWithInvalidMount) {
  ProtoBlob create_blob =
      CreateCreateFileOptionsBlob(999, GetDefaultFilePath());

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->CreateFile(create_blob)));
}

// CreateFile fails when the parent directory does not exist.
TEST_F(SmbProviderTest, CreateFileFailsWhenParentDoesNotExist) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob create_blob = CreateCreateFileOptionsBlob(mount_id, "/new/dog.jpg");

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->CreateFile(create_blob)));
}

// CreateFile fails when the parent directory is locked.
TEST_F(SmbProviderTest, CreateFileFailsWhenParentDirIsLocked) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/cats"));

  ProtoBlob create_blob =
      CreateCreateFileOptionsBlob(mount_id, "/cats/dog.jpg");

  EXPECT_EQ(ERROR_ACCESS_DENIED,
            CastError(smbprovider_->CreateFile(create_blob)));
}

// CreateFile fails when the file already exits.
TEST_F(SmbProviderTest, CreateFileFailsWhenFileExists) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/dog.jpg"));

  ProtoBlob create_blob = CreateCreateFileOptionsBlob(mount_id, "/dog.jpg");

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateFile(create_blob)));
}

// CreateFile succeeds when passed valid parameters and closes the file handle.
TEST_F(SmbProviderTest, CreateFileSucceeds) {
  const int32_t mount_id = PrepareMount();
  const std::string path = "/dog.jpg";

  ProtoBlob create_blob = CreateCreateFileOptionsBlob(mount_id, path);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateFile(create_blob)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath(path)));
  ExpectNoOpenEntries();
}

// Created file should be able to be opened.
TEST_F(SmbProviderTest, CreatedFileCanBeOpened) {
  const int32_t mount_id = PrepareMount();
  const std::string path = "/dog.jpg";

  ProtoBlob create_blob = CreateCreateFileOptionsBlob(mount_id, path);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateFile(create_blob)));

  int32_t file_id;
  int32_t error_code;
  ProtoBlob open_file_blob =
      CreateOpenFileOptionsBlob(mount_id, path, false /* writeable */);
  smbprovider_->OpenFile(open_file_blob, &error_code, &file_id);
  EXPECT_EQ(ERROR_OK, CastError(error_code));
  EXPECT_GE(file_id, 0);

  CloseFileHelper(mount_id, file_id);
}

// CreateFile should be able to create multiple files with different paths.
TEST_F(SmbProviderTest, CreateMultipleFiles) {
  const int32_t mount_id = PrepareMount();
  const std::string path1 = "/dog.jpg";
  const std::string path2 = "/cat.jpg";

  ProtoBlob create_blob1 = CreateCreateFileOptionsBlob(mount_id, path1);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateFile(create_blob1)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath(path1)));

  ProtoBlob create_blob2 = CreateCreateFileOptionsBlob(mount_id, path2);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateFile(create_blob2)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath(path2)));
}

// CreateFile should fail if a file already exists in the path.
TEST_F(SmbProviderTest, CreateFileFailsFileAlreadyExists) {
  const int32_t mount_id = PrepareMount();
  const std::string path = "/dog.jpg";

  fake_samba_->AddFile(GetDefaultFullPath("/dog.jpg"));

  ProtoBlob create_blob2 = CreateCreateFileOptionsBlob(mount_id, path);
  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateFile(create_blob2)));
}

// CreateFile should fail if a directory already exists in the path.
TEST_F(SmbProviderTest, CreateFileFailsDirectoryExists) {
  const int32_t mount_id = PrepareMount();
  const std::string directory_path = "/dogs";

  // Add a directory located at "/dogs".
  fake_samba_->AddDirectory(GetDefaultFullPath(directory_path));

  // Attempt to add a file located at "/dogs".
  ProtoBlob create_blob = CreateCreateFileOptionsBlob(mount_id, directory_path);

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateFile(create_blob)));
}

// Truncate should fail when passed an invalid protobuf.
TEST_F(SmbProviderTest, TruncateFailsWithInvalidProto) {
  ProtoBlob empty_blob;
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->Truncate(empty_blob)));
}

// Truncate should fail when passed a mount id that does not exist.
TEST_F(SmbProviderTest, TruncateFailsWithMountThatDoesntExist) {
  ProtoBlob blob =
      CreateTruncateOptionsBlob(999, GetDefaultFilePath(), 0 /* length */);

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->Truncate(blob)));
}

// Truncate should fail when passed a file path that does not exist.
TEST_F(SmbProviderTest, TruncateFailsWithFilePathThatDoesntExist) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);

  ProtoBlob blob =
      CreateTruncateOptionsBlob(mount_id, "/foo/bar.txt", 0 /* length */);

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->Truncate(blob)));
}

// Truncate should fail when passed a negative length.
TEST_F(SmbProviderTest, TruncateFailsWithNegativeLength) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);

  ProtoBlob blob = CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(),
                                             -1 /* length */);

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(smbprovider_->Truncate(blob)));
}

// Truncate should close the file when truncate returns an error.
TEST_F(SmbProviderTest, TruncateReturnsCorrectError) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t expected_error = EACCES;

  fake_samba_->SetTruncateError(expected_error);

  ProtoBlob blob =
      CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(), 1 /* length */);

  EXPECT_EQ(GetErrorFromErrno(expected_error),
            CastError(smbprovider_->Truncate(blob)));
  ExpectNoOpenEntries();
}

// Truncate should return the error from truncate even if CloseFile fails.
TEST_F(SmbProviderTest, TruncateReturnsCorrectErrorWhenCloseFileFails) {
  const std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int32_t truncate_error = EACCES;

  // Set the errors that Truncate and Close will return.
  fake_samba_->SetTruncateError(truncate_error);
  fake_samba_->SetCloseFileError(EMFILE);

  // Call Truncate.
  ProtoBlob blob =
      CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(), 1 /* length */);

  // Error returned should be the one that Truncate returned.
  EXPECT_EQ(GetErrorFromErrno(truncate_error),
            CastError(smbprovider_->Truncate(blob)));
}

// Truncate should successfully change the file size to a smaller length.
TEST_F(SmbProviderTest, TruncateChangesFileSizeToSmallerLength) {
  std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int64_t new_length = 5;

  // Truncate the length to the smaller size.
  ProtoBlob blob =
      CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(), new_length);

  // Truncate should be successful.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->Truncate(blob)));

  // Resize the original vector to get the expected value.
  file_data.resize(new_length, 0);
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), file_data));
  ExpectNoOpenEntries();
}

// Truncate should successfully change the file size to a larger length.
TEST_F(SmbProviderTest, TruncateChangesFileSizeToLargerLength) {
  std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);
  const int64_t new_length = 50;

  // Truncate the length to the larger size.
  ProtoBlob blob =
      CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(), new_length);

  // Truncate should be successful.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->Truncate(blob)));

  // Resize the original vector to get the expected value. The appended values
  // should be initialized to '0'.
  file_data.resize(new_length, 0);
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), file_data));
  ExpectNoOpenEntries();
}

TEST_F(SmbProviderTest, TruncateSucceedsWithSameLength) {
  std::vector<uint8_t> file_data = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const int32_t mount_id = PrepareSingleFileMountWithData(file_data);

  // Truncate the length to the same size.
  ProtoBlob blob = CreateTruncateOptionsBlob(mount_id, GetDefaultFilePath(),
                                             file_data.size());

  // Truncate should be successful.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->Truncate(blob)));
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), file_data));
  ExpectNoOpenEntries();
}

TEST_F(SmbProviderTest, WriteFileFailsWithEmptyProto) {
  ProtoBlob empty_blob;
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->WriteFile(empty_blob, base::ScopedFD())));
}

TEST_F(SmbProviderTest, WriteFileFailsWithNegativeOffset) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);

  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, -1 /* offset */, 0 /* length */);
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->WriteFile(write_blob, base::ScopedFD())));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileFailsWithNegativeLength) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);

  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, -1 /* length */);

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->WriteFile(write_blob, base::ScopedFD())));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileFailsWithFileIdThatDoesntExist) {
  const int32_t mount_id = PrepareSingleFileMount();

  // Pass in an invalid file id to options.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, 999 /* file_id */, 0 /* offset */, 0 /* length */);

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->WriteFile(write_blob, base::ScopedFD())));
}

TEST_F(SmbProviderTest, WriteFileFailsWithInvalidFileDescriptor) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);

  // Create blob with valid parameters.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, 0 /* length */);

  // Create an invalid file descriptor.
  base::ScopedFD fd;
  EXPECT_FALSE(fd.is_valid());

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileFailsWithDirectoryId) {
  const int32_t mount_id = PrepareSingleFileMount();

  // Create a temporary file with a valid file descriptor.
  const std::vector<uint8_t> data = {0, 1, 2, 3};
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);

  const int32_t dir_id = OpenAddedDirectory(GetAddedFullDirectoryPath());

  // Pass in the directory id to options.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, dir_id, 0 /* offset */, 0 /* length */);

  EXPECT_EQ(ERROR_NOT_FOUND,
            CastError(smbprovider_->WriteFile(write_blob, fd)));
}

TEST_F(SmbProviderTest, WriteFileFailsWithLengthTooLarge) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);

  // Attempt to read size() + 1 bytes.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size() + 1);

  // Should return error since it read the wrong number of bytes.
  EXPECT_EQ(ERROR_IO, CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileSucceedsWithShorterLength) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);

  // Attempt to read size() - 1 bytes.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size() - 1);

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileSucceedsWithExactLength) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);

  // Attempt to read size() bytes.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size());

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileFailsWithReadOnlyFile) {
  const int32_t mount_id = PrepareSingleFileMount();
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Open a file with read-only permissions.
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), false /* writeable */);

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);

  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size());

  // Should return error since it attempted to write to a read-only file.
  EXPECT_EQ(ERROR_INVALID_OPERATION,
            CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileCorrectlyWritesToFileInSamba) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Validate that the file does not have the same data.
  EXPECT_FALSE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), data));

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size());

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  // File should have the correct data.
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), data));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileCorrectlyWritesFromOffset) {
  const std::vector<uint8_t> original_data = {0, 1, 2, 3, 4, 5};
  const int32_t mount_id = PrepareSingleFileMountWithData(original_data);
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> new_data = {'a', 'b'};

  // Create a temporary file with a valid file descriptor.
  const int64_t offset = 1;
  base::ScopedFD fd;
  WriteToTempFileWithData(new_data, &fd);
  ProtoBlob write_blob =
      CreateWriteFileOptionsBlob(mount_id, file_id, offset, new_data.size());

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  // File should have the correct data.
  const std::vector<uint8_t> expected = {0, 'a', 'b', 3, 4, 5};
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), expected));

  // Offset should be equal to original offset + the write size.
  EXPECT_EQ(offset + new_data.size(), fake_samba_->GetFileOffset(file_id));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileFailsWithOffsetBiggerThanSize) {
  const std::vector<uint8_t> original_data = {0, 1};
  const int32_t mount_id = PrepareSingleFileMountWithData(original_data);
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> new_data = {'a'};

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(new_data, &fd);

  // Attempt to write with offset size() + 1.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, original_data.size() + 1, new_data.size());

  // Should return error since offset is bigger than current size.
  EXPECT_EQ(ERROR_INVALID_OPERATION,
            CastError(smbprovider_->WriteFile(write_blob, fd)));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileCorrectlyWritesTwice) {
  const int32_t mount_id = PrepareSingleFileMount();
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);
  const std::vector<uint8_t> data = {0, 1, 2, 3};

  // Create a temporary file with a valid file descriptor.
  base::ScopedFD fd;
  WriteToTempFileWithData(data, &fd);
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, data.size());

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  // File should have the correct data.
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), data));

  // Create another temporary file with a valid file descriptor.
  const std::vector<uint8_t> new_data = {4, 5, 6, 7};
  base::ScopedFD fd2;
  WriteToTempFileWithData(new_data, &fd2);

  // Write starting at the end of the first written data.
  ProtoBlob write_blob2 = CreateWriteFileOptionsBlob(
      mount_id, file_id, data.size(), new_data.size());

  // Should return OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob2, fd2)));

  // File should have the correct data.
  const std::vector<uint8_t> expected = {0, 1, 2, 3, 4, 5, 6, 7};
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), expected));
  EXPECT_EQ(expected.size(), fake_samba_->GetFileOffset(file_id));

  CloseFileHelper(mount_id, file_id);
}

TEST_F(SmbProviderTest, WriteFileCorrectlyWritesToCorrectFile) {
  const std::vector<uint8_t> original_data = {0, 1};
  const int32_t mount_id = PrepareSingleFileMountWithData(original_data);
  const int32_t file_id =
      OpenAddedFile(mount_id, GetDefaultFilePath(), true /* writeable */);

  // Add a second file with the same data.
  const std::string file_path2 = "/path/cat.jpg";
  fake_samba_->AddFile(GetDefaultFullPath(file_path2), kFileDate,
                       original_data);
  const int32_t file_id2 =
      OpenAddedFile(mount_id, file_path2, true /* writeable */);

  // Create a temporary file with a valid file descriptor.
  const std::vector<uint8_t> new_data = {'a', 'b'};
  base::ScopedFD fd;
  WriteToTempFileWithData(new_data, &fd);

  // Write to the file1.
  ProtoBlob write_blob = CreateWriteFileOptionsBlob(
      mount_id, file_id, 0 /* offset */, new_data.size());

  // Should be OK.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->WriteFile(write_blob, fd)));

  // File1 should have the new data.
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetAddedFullFilePath(), new_data));
  EXPECT_EQ(new_data.size(), fake_samba_->GetFileOffset(file_id));

  // File2 should have the original data still.
  EXPECT_TRUE(fake_samba_->IsFileDataEqual(GetDefaultFullPath(file_path2),
                                           original_data));
  EXPECT_EQ(0, fake_samba_->GetFileOffset(file_id2));

  CloseFileHelper(mount_id, file_id);
  CloseFileHelper(mount_id, file_id2);
}

TEST_F(SmbProviderTest, CreateDirectoryFailsWithEmptyProto) {
  ProtoBlob empty_blob;
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->CreateDirectory(empty_blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryFailsWithInvalidMount) {
  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      999, GetDefaultDirectoryPath(), false /* recursive */);

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryFailsWithAlreadyExistingDirectory) {
  const int32_t mount_id = PrepareMount();
  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryFailsWithAlreadyExistingFile) {
  const int32_t mount_id = PrepareSingleFileMount();

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, GetDefaultFilePath(), false /* recursive */);

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryFailsWithNoParentWhenNotRecursive) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, "/test/path/invalid/path", false /* recursive */);

  // This fails since |recursive| is set to false, otherwise it would also
  // create the parent directories.
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectorySucceeds) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryCantAddTheSameDirectory) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), false /* recursive */);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));

  // Should fail attempting to add the same directory..
  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryCanAddSubDirectory) {
  const int32_t mount_id = PrepareMount();
  const std::string sub_dir = "/path/test";

  // Should fail adding the sub directory without the parent directory since
  // |recursive| is set to false.
  ProtoBlob sub_dir_blob = CreateCreateDirectoryOptionsBlob(
      mount_id, sub_dir, false /* recursive */);
  EXPECT_EQ(ERROR_NOT_FOUND,
            CastError(smbprovider_->CreateDirectory(sub_dir_blob)));

  // Add the parent directory.
  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(mount_id, "/path",
                                                    false /* recursive */);
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));

  // Should now succeed adding the sub directory.
  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(sub_dir_blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryCanCreateDirectoryRecursively) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(mount_id, "/1/2/3",
                                                    true /* recursive */);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/1")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/1/2")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/1/2/3")));
}

TEST_F(SmbProviderTest, CreateDirectoryRecursiveWithExistingParent) {
  const int32_t mount_id = PrepareMount();

  // Add a parent directory.
  fake_samba_->AddDirectory(GetDefaultFullPath("/1"));

  // Create a directory under the existing parent.
  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(mount_id, "/1/2/3",
                                                    true /* recursive */);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/1/2")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/1/2/3")));
}

TEST_F(SmbProviderTest, CreateDirectoryRecursiveFailsWithExistingPath) {
  const int32_t mount_id = PrepareMount();

  // Add the directories.
  fake_samba_->AddDirectory(GetDefaultFullPath("/1"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/1/2"));

  // Create the directory recursively.
  ProtoBlob blob =
      CreateCreateDirectoryOptionsBlob(mount_id, "/1/2", true /* recursive */);

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryCanCreateSingleDirectoryRecursively) {
  const int32_t mount_id = PrepareMount();

  // Create a single directory and have recursive set to true.
  ProtoBlob blob = CreateCreateDirectoryOptionsBlob(
      mount_id, GetDefaultDirectoryPath(), true /* recursive */);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, CreateDirectoryFailureOnCreatingSlash) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob blob =
      CreateCreateDirectoryOptionsBlob(mount_id, "/", false /* recursive */);

  // "/" should return error since the directory already exists.
  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CreateDirectory(blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsOnInvalidSource) {
  const int32_t mount_id = PrepareMount();

  ProtoBlob move_blob = CreateMoveEntryOptionsBlob(mount_id, "/dogs", "/cats");

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsToMoveADirectoryIntoItself) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("dogs"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/dogs", "/dogs/cats");

  EXPECT_EQ(ERROR_INVALID_OPERATION,
            CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsWhenTargetIsExistingFile) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/pic.jpg"));
  fake_samba_->AddFile(GetDefaultFullPath("/exists.txt"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/pic.jpg", "/exists.txt");

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsWhenSourceIsDirAndTargetIsNonEmptyDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/exists"));
  fake_samba_->AddFile(GetDefaultFullPath("/exists/1.txt"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/other"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/other", "/exists");

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsWhenSourceIsFileAndTargetIsExistingDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/source.jpg"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/exists"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/source.jpg", "/exists");

  EXPECT_EQ(ERROR_NOT_A_FILE, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsRenameFile) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/oldname.txt"));

  ProtoBlob move_blob = CreateMoveEntryOptionsBlob(
      mount_id, "/path/oldname.txt", "/path/newname.txt");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(
      fake_samba_->EntryExists(GetDefaultFullPath("/path/oldname.txt")));
  EXPECT_TRUE(
      fake_samba_->EntryExists(GetDefaultFullPath("/path/newname.txt")));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsRenameDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/oldname"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/path/oldname", "/path/newname");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/path/oldname")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path/newname")));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsRenameAndMoveFile) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/oldname.txt"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/oldname.txt", "/path/newname.txt");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/oldname.txt")));
  EXPECT_TRUE(
      fake_samba_->EntryExists(GetDefaultFullPath("/path/newname.txt")));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsRenameAndMoveDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/oldname"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/oldname", "/path/newname");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/oldname")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/path/newname")));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsMovingEmptyDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/target"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/dogs", "/target/dogs");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/target/dogs/")));
}

TEST_F(SmbProviderTest, MoveEntrySucceedsMoveNonEmptyDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/dogs/1.jpg"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs/labs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/target"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/dogs", "/target/dogs");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->MoveEntry(move_blob)));

  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs/1.jpg")));
  EXPECT_FALSE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs/labs")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/target/dogs/")));
  EXPECT_TRUE(
      fake_samba_->EntryExists(GetDefaultFullPath("/target/dogs/1.jpg")));
  EXPECT_TRUE(
      fake_samba_->EntryExists(GetDefaultFullPath("/target/dogs/labs")));
}

TEST_F(SmbProviderTest, MoveEntryFailsToMoveALockedDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/lockedDir"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/other"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/lockedDir", "/other/lockedDir");

  EXPECT_EQ(ERROR_ACCESS_DENIED, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, MoveEntryFailsToMoveIntoLockedDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/file.txt"));
  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/lockedDir"));

  ProtoBlob move_blob =
      CreateMoveEntryOptionsBlob(mount_id, "/file.txt", "/lockedDir/file.txt");

  EXPECT_EQ(ERROR_ACCESS_DENIED, CastError(smbprovider_->MoveEntry(move_blob)));
}

TEST_F(SmbProviderTest, CopyEntryFailsOnInvalidSource) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("newdir"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/file.txt", "/newdir/file.txt");

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->CopyEntry(copy_blob)));
}

TEST_F(SmbProviderTest, CopyEntryFailsOnFileWhenDestinationExists) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/file.txt"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/dir1"));
  fake_samba_->AddFile(GetDefaultFullPath("/dir1/file.txt"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/file.txt", "/dir1/file.txt");

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CopyEntry(copy_blob)));
}

TEST_F(SmbProviderTest, CopyEntryFailsOnDirectoryWhenDestinationExists) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/cats"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/cats/dogs"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/dogs", "/cats/dogs");

  EXPECT_EQ(ERROR_EXISTS, CastError(smbprovider_->CopyEntry(copy_blob)));
}

TEST_F(SmbProviderTest, CopyEntryFailsWhenDestinationIsInALockedDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/dog.jpg"));
  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/cats"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/dog.jpg", "/cats/dog.jpg");

  EXPECT_EQ(ERROR_ACCESS_DENIED, CastError(smbprovider_->CopyEntry(copy_blob)));
}

TEST_F(SmbProviderTest, CopyEntrySucceedsOnFile) {
  const std::vector<uint8_t> file_data = {10, 11, 12, 13, 14, 15};
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddFile(GetDefaultFullPath("/dog1.jpg"), kFileDate, file_data);
  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/dog1.jpg", "/dogs/dog1.jpg");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CopyEntry(copy_blob)));

  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/dog1.jpg")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs/dog1.jpg")));
}

TEST_F(SmbProviderTest, CopyEntrySucceedsOnDirectory) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/animals"));

  ProtoBlob copy_blob =
      CreateCopyEntryOptionsBlob(mount_id, "/dogs", "/animals/dogs");

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->CopyEntry(copy_blob)));

  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/dogs")));
  EXPECT_TRUE(fake_samba_->EntryExists(GetDefaultFullPath("/animals/dogs")));
}

// GetDeleteList succeeds on an empty directory.
TEST_F(SmbProviderTest, GetDeleteListSucceedsOnEmptyDirecotry) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/dogs"));

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/dogs");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_OK, CastError(error_code));

  DeleteListProto delete_list =
      GetDeleteListProtoFromFD(fd.get(), bytes_written);
  EXPECT_EQ(1, delete_list.entries_size());

  EXPECT_EQ("/dogs", delete_list.entries(0));
}

// GetDeleteList succeeds on a directory of files.
TEST_F(SmbProviderTest, GetDeleteListSucceedsOnADirOfFiles) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/1.jpg"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/3.png"));

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/path");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_OK, CastError(error_code));

  DeleteListProto delete_list =
      GetDeleteListProtoFromFD(fd.get(), bytes_written);
  EXPECT_EQ(4, delete_list.entries_size());

  EXPECT_EQ("/path/1.jpg", delete_list.entries(0));
  EXPECT_EQ("/path/2.txt", delete_list.entries(1));
  EXPECT_EQ("/path/3.png", delete_list.entries(2));
  EXPECT_EQ("/path", delete_list.entries(3));
}

// GetDeleteList succeeds on multiple levels of nested directories.
TEST_F(SmbProviderTest, GetDeleteListSucceedsOnNestedEmptyDirectories) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs/lab"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/cats"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/cats/blue"));

  ProtoBlob blob =
      CreateGetDeleteListOptionsBlob(mount_id, GetDefaultDirectoryPath());

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_OK, CastError(error_code));

  DeleteListProto delete_list =
      GetDeleteListProtoFromFD(fd.get(), bytes_written);
  EXPECT_EQ(5, delete_list.entries_size());

  EXPECT_EQ("/path/dogs/lab", delete_list.entries(0));
  EXPECT_EQ("/path/dogs", delete_list.entries(1));
  EXPECT_EQ("/path/cats/blue", delete_list.entries(2));
  EXPECT_EQ("/path/cats", delete_list.entries(3));
  EXPECT_EQ("/path", delete_list.entries(4));
}

// GetDeleteList succeeds on a dir with a file and a non-empty dir.
TEST_F(SmbProviderTest, GetDeleteListSucceedsDirWithAfileAndNonEmptyDir) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/dogs/1.jpg"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/path");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_OK, CastError(error_code));

  DeleteListProto delete_list =
      GetDeleteListProtoFromFD(fd.get(), bytes_written);
  EXPECT_EQ(4, delete_list.entries_size());

  EXPECT_EQ("/path/dogs/1.jpg", delete_list.entries(0));
  EXPECT_EQ("/path/dogs", delete_list.entries(1));
  EXPECT_EQ("/path/2.txt", delete_list.entries(2));
  EXPECT_EQ("/path", delete_list.entries(3));
}

// GetDeleteList fails if Directory cannot be opened.
TEST_F(SmbProviderTest, GetDeleteListFailsWhenADirectoryCannotBeOpened) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddLockedDirectory(GetDefaultFullPath("/path/dogs"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/2.txt"));

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/path");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_ACCESS_DENIED, CastError(error_code));
}

// GetDeleteList succeeds on a file.
TEST_F(SmbProviderTest, GetDeleteListSucceedsOnAFile) {
  const int32_t mount_id = PrepareMount();

  fake_samba_->AddDirectory(GetDefaultFullPath("/path"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/1.jpg"));

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/path/1.jpg");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_OK, CastError(error_code));

  DeleteListProto delete_list =
      GetDeleteListProtoFromFD(fd.get(), bytes_written);
  EXPECT_EQ(1, delete_list.entries_size());
  EXPECT_EQ("/path/1.jpg", delete_list.entries(0));
}

// GetDeleteList fails on a non-file, non-directory.
TEST_F(SmbProviderTest, GetDeleteListFailsOnNonFileNonDirectory) {
  int32_t mount_id = PrepareMount();
  const std::string printer_path = "/path/canon.cn";

  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddEntry(GetDefaultFullPath(printer_path), SMBC_PRINTER_SHARE);

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/path/cannon.cn");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_NOT_FOUND, error_code);
}

// GetDeleteList fails on non-existent path.
TEST_F(SmbProviderTest, GetDeleteListFailsOnNonExistantEntry) {
  int32_t mount_id = PrepareMount();

  ProtoBlob blob = CreateGetDeleteListOptionsBlob(mount_id, "/non-existent");

  int32_t error_code;
  brillo::dbus_utils::FileDescriptor fd;
  int32_t bytes_written;
  smbprovider_->GetDeleteList(blob, &error_code, &fd, &bytes_written);

  EXPECT_EQ(ERROR_NOT_FOUND, error_code);
}

TEST_F(SmbProviderTest, GetEntriesFailsWithNonExistentDirectory) {
  int32_t mount_id = PrepareMount();

  int32_t error_code;
  ProtoBlob entries;
  ReadDirectoryOptionsProto proto =
      CreateReadDirectoryOptionsProto(mount_id, GetDefaultDirectoryPath());
  GetEntries(
      proto,
      GetIterator<DirectoryIterator>(GetAddedFullDirectoryPath(), fake_samba_),
      &error_code, &entries);

  DirectoryEntryListProto entry_list =
      GetDirectoryEntryListProtoFromBlob(entries);
  EXPECT_EQ(error_code, ERROR_NOT_FOUND);
}

TEST_F(SmbProviderTest, GetEntriesSucceedsWithEmptyDirectory) {
  int32_t mount_id = PrepareMount();
  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());

  int32_t error_code;
  ProtoBlob entries;
  ReadDirectoryOptionsProto proto =
      CreateReadDirectoryOptionsProto(mount_id, GetDefaultDirectoryPath());
  GetEntries(
      proto,
      GetIterator<DirectoryIterator>(GetAddedFullDirectoryPath(), fake_samba_),
      &error_code, &entries);

  DirectoryEntryListProto entry_list =
      GetDirectoryEntryListProtoFromBlob(entries);
  EXPECT_EQ(error_code, ERROR_OK);
  EXPECT_EQ(entry_list.entries().size(), 0);
}

TEST_F(SmbProviderTest, GetEntriesSucceedsWithMultipleEntries) {
  int32_t mount_id = PrepareMount();
  fake_samba_->AddDirectory(GetAddedFullDirectoryPath());
  fake_samba_->AddDirectory(GetDefaultFullPath("/path/images"));
  fake_samba_->AddFile(GetDefaultFullPath("/path/dog.jpg"));

  int32_t error_code;
  ProtoBlob entries;
  ReadDirectoryOptionsProto proto =
      CreateReadDirectoryOptionsProto(mount_id, GetDefaultDirectoryPath());
  GetEntries(
      proto,
      GetIterator<DirectoryIterator>(GetAddedFullDirectoryPath(), fake_samba_),
      &error_code, &entries);

  DirectoryEntryListProto entry_list =
      GetDirectoryEntryListProtoFromBlob(entries);
  EXPECT_EQ(error_code, ERROR_OK);
  EXPECT_EQ(entry_list.entries().size(), 2);

  const DirectoryEntryProto& entry1 = entry_list.entries(0);
  EXPECT_EQ(entry1.name(), "images");
  EXPECT_TRUE(entry1.is_directory());

  const DirectoryEntryProto& entry2 = entry_list.entries(1);
  EXPECT_EQ(entry2.name(), "dog.jpg");
  EXPECT_FALSE(entry2.is_directory());
}

TEST_F(SmbProviderTest, GetSharesFailsOnEmptyProto) {
  ProtoBlob empty_blob;
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(empty_blob, &error, &result);
  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED, CastError(error));
}

TEST_F(SmbProviderTest, GetSharesFailsOnNonExistentServer) {
  ProtoBlob blob = CreateGetSharesOptionsBlob("smb://0.0.0.1");
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_NOT_FOUND, CastError(error));
}

TEST_F(SmbProviderTest, GetSharesSucceedsOnEmptyServer) {
  const std::string server_url = "smb://192.168.0.1";
  fake_samba_->AddServer(server_url);

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));
  EXPECT_TRUE(GetDirectoryEntryListProtoFromBlob(result).entries().empty());
}

TEST_F(SmbProviderTest, GetSharesSucceedsWithSingleShare) {
  const std::string server_url = "smb://192.168.0.1";
  const std::string share = "share1";

  fake_samba_->AddServer(server_url);
  fake_samba_->AddShare(server_url + "/" + share);

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));

  DirectoryEntryListProto dir_entry_list =
      GetDirectoryEntryListProtoFromBlob(result);
  EXPECT_EQ(dir_entry_list.entries().size(), 1);

  const DirectoryEntryProto& entry = dir_entry_list.entries(0);
  EXPECT_EQ(entry.name(), share);
  EXPECT_TRUE(entry.is_directory());
}

TEST_F(SmbProviderTest, GetSharesSucceedsWithMultipleShares) {
  const std::string server_url = "smb://192.168.0.1";
  const std::string share1 = "share1";
  const std::string share2 = "share2";

  fake_samba_->AddServer(server_url);
  fake_samba_->AddShare(server_url + "/" + share1);
  fake_samba_->AddShare(server_url + "/" + share2);

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));

  DirectoryEntryListProto dir_entry_list =
      GetDirectoryEntryListProtoFromBlob(result);
  EXPECT_EQ(dir_entry_list.entries().size(), 2);

  const DirectoryEntryProto& entry1 = dir_entry_list.entries(0);
  EXPECT_EQ(entry1.name(), share1);
  EXPECT_TRUE(entry1.is_directory());

  const DirectoryEntryProto& entry2 = dir_entry_list.entries(1);
  EXPECT_EQ(entry2.name(), share2);
  EXPECT_TRUE(entry2.is_directory());
}

TEST_F(SmbProviderTest, GetSharesDoesntReturnSelfAndParentEntries) {
  const std::string server_url = "smb://192.168.0.1";
  const std::string share1 = "share1";

  fake_samba_->AddServer(server_url);
  fake_samba_->AddShare(server_url + "/" + share1);

  // These shouldn't be returned by GetShares.
  fake_samba_->AddShare(server_url + "/.");
  fake_samba_->AddShare(server_url + "/..");

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));

  DirectoryEntryListProto dir_entry_list =
      GetDirectoryEntryListProtoFromBlob(result);
  EXPECT_EQ(dir_entry_list.entries().size(), 1);

  const DirectoryEntryProto& entry1 = dir_entry_list.entries(0);
  EXPECT_EQ(entry1.name(), share1);
  EXPECT_TRUE(entry1.is_directory());
}

TEST_F(SmbProviderTest, GetSharesDoesntReturnNonShareEntries) {
  const std::string server_url = "smb://192.168.0.1";
  const std::string share1 = "share1";

  fake_samba_->AddServer(server_url);
  fake_samba_->AddShare(server_url + "/" + share1);

  // These shouldn't be returned by GetShares since they aren't shares.
  fake_samba_->AddDirectory(server_url + "/dir");
  fake_samba_->AddFile(server_url + "/file");

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));

  DirectoryEntryListProto dir_entry_list =
      GetDirectoryEntryListProtoFromBlob(result);
  EXPECT_EQ(dir_entry_list.entries().size(), 1);

  const DirectoryEntryProto& entry1 = dir_entry_list.entries(0);
  EXPECT_EQ(entry1.name(), share1);
  EXPECT_TRUE(entry1.is_directory());
}

TEST_F(SmbProviderTest, GetSharesReturnsShareContainingDirectory) {
  const std::string server_url = "smb://192.168.0.1";
  const std::string share1 = "share1";

  fake_samba_->AddServer(server_url);
  fake_samba_->AddShare(server_url + "/" + share1);

  // Add a directory in the share.
  fake_samba_->AddDirectory(server_url + "/" + share1 + "/dir");

  ProtoBlob blob = CreateGetSharesOptionsBlob(server_url);
  int32_t error;
  ProtoBlob result;

  smbprovider_->GetShares(blob, &error, &result);
  EXPECT_EQ(ERROR_OK, CastError(error));

  DirectoryEntryListProto dir_entry_list =
      GetDirectoryEntryListProtoFromBlob(result);
  EXPECT_EQ(dir_entry_list.entries().size(), 1);

  const DirectoryEntryProto& entry1 = dir_entry_list.entries(0);
  EXPECT_EQ(entry1.name(), share1);
  EXPECT_TRUE(entry1.is_directory());
}

// Remount fails on an invalid protobuf.
TEST_F(SmbProviderTest, RemountFailsWithInvalidProto) {
  ProtoBlob empty_blob;

  EXPECT_EQ(ERROR_DBUS_PARSE_FAILED,
            CastError(smbprovider_->Remount(empty_blob)));
  EXPECT_EQ(0, mount_manager_->MountCount());
  ExpectNoOpenEntries();
}

// Remount fails on a share that doesn't exist.
TEST_F(SmbProviderTest, RemountFailsWithInvalidShare) {
  const int32_t mount_id = 1;
  ProtoBlob blob = CreateRemountOptionsBlob("smb://testshare/none", mount_id);

  EXPECT_EQ(ERROR_NOT_FOUND, CastError(smbprovider_->Remount(blob)));
  EXPECT_EQ(0, mount_manager_->MountCount());
  ExpectNoOpenEntries();
}

// Remount succeeds on a mountable share.
TEST_F(SmbProviderTest, RemountSucceedsOnValidShare) {
  fake_samba_->AddDirectory("smb://testshare");

  const int32_t mount_id = 1;
  ProtoBlob blob = CreateRemountOptionsBlob("smb://testshare", mount_id);

  EXPECT_EQ(ERROR_OK, CastError(smbprovider_->Remount(blob)));
  EXPECT_EQ(1, mount_manager_->MountCount());
  EXPECT_TRUE(mount_manager_->IsAlreadyMounted(mount_id));
}

}  // namespace smbprovider
