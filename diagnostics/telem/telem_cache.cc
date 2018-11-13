// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/telem/telem_cache.h"

#include <utility>

namespace diagnostics {

TelemCache::TelemCache() {
  default_tick_clock_ = std::make_unique<base::DefaultTickClock>();
  tick_clock_ = default_tick_clock_.get();
}

TelemCache::TelemCache(base::TickClock* tick_clock) : tick_clock_(tick_clock) {}

TelemCache::~TelemCache() = default;

bool TelemCache::IsValid(TelemetryItemEnum item,
                         base::TimeDelta acceptable_age) const {
  return cache_.count(item) &&
         (tick_clock_->NowTicks() - cache_.at(item).last_fetched_time_ticks) <
             acceptable_age;
}

const base::Value* TelemCache::GetParsedData(TelemetryItemEnum item) {
  if (!cache_.count(item)) {
    // Item does not yet exist in the cache.
    return nullptr;
  }
  return cache_.at(item).data.get();
}

void TelemCache::SetParsedData(TelemetryItemEnum item,
                               std::unique_ptr<base::Value> data) {
  TelemItem new_telem_item(std::move(data), tick_clock_->NowTicks());

  if (!cache_.count(item))
    cache_.emplace(item, std::move(new_telem_item));
  else
    cache_.at(item) = std::move(new_telem_item);
}

void TelemCache::Invalidate() {
  cache_.clear();
}

TelemCache::TelemItem::TelemItem(std::unique_ptr<base::Value> data_in,
                                 base::TimeTicks last_fetched_time_ticks_in)
    : data(std::move(data_in)),
      last_fetched_time_ticks(last_fetched_time_ticks_in) {}

}  // namespace diagnostics
