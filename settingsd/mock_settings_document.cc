// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "settingsd/mock_settings_document.h"

#include "settingsd/identifier_utils.h"

namespace settingsd {

MockSettingsDocument::MockSettingsDocument(const std::string& source_id,
                                           const VersionStamp& version_stamp)
    : source_id_(source_id), version_stamp_(version_stamp) {}

MockSettingsDocument::~MockSettingsDocument() {}

const base::Value* MockSettingsDocument::GetValue(const Key& key) const {
  auto entry = key_value_map_.find(key);
  return entry != key_value_map_.end() ? entry->second.get() : nullptr;
}

std::set<Key> MockSettingsDocument::GetKeys(const Key& prefix) const {
  std::set<Key> result;
  for (const auto& entry : utils::GetRange(prefix, key_value_map_))
    result.insert(entry.first);
  return result;
}

std::set<Key> MockSettingsDocument::GetDeletions(const Key& prefix) const {
  std::set<Key> result;
  for (const auto& entry : utils::GetRange(prefix, deletions_))
    result.insert(entry);
  return result;
}

const std::string& MockSettingsDocument::GetSourceId() const {
  return source_id_;
}

const VersionStamp& MockSettingsDocument::GetVersionStamp() const {
  return version_stamp_;
}

bool MockSettingsDocument::HasKeysOrDeletions(const Key& prefix) const {
  return utils::HasKeys(prefix, key_value_map_) ||
         utils::HasKeys(prefix, deletions_);
}

void MockSettingsDocument::SetKey(const Key& key,
                                  std::unique_ptr<base::Value> value) {
  key_value_map_.insert(std::make_pair(key, std::move(value)));
}

void MockSettingsDocument::ClearKey(const Key& key) {
  key_value_map_.erase(key);
}

void MockSettingsDocument::ClearKeys() {
  key_value_map_.clear();
}

void MockSettingsDocument::SetDeletion(const Key& key) {
  deletions_.insert(key);
}

void MockSettingsDocument::ClearDeletion(const Key& key) {
  deletions_.erase(key);
}

void MockSettingsDocument::ClearDeletions() {
  deletions_.clear();
}

}  // namespace settingsd
