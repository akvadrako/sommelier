/*
 * Copyright (C) 2015-2018 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ITaskEventSource"

#include "ITaskEventSource.h"
#include "LogHelper.h"

namespace android {
namespace camera2 {

/**
 * Attach a Listening client to a particular event
 *
 * @param observer interface pointer to attach
 * @param event concrete event to listen to
 */
status_t
ITaskEventSource::attachListener(ITaskEventListener *observer,
                                 ITaskEventListener::PUTaskEventType event)
{
    LOG1("@%s: %p to event type %d", __FUNCTION__, observer, event);
    status_t status = NO_ERROR;
    if (observer == nullptr)
        return BAD_VALUE;
    std::lock_guard<std::mutex> l(mListenerLock);

    // Check if we have any listener registered to this event
    std::map<ITaskEventListener::PUTaskEventType, listener_list_t>::
            iterator itListener = mListeners.find(event);
    if (itListener == mListeners.end()) {
        // First time someone registers for this event
        listener_list_t theList;
        theList.push_back(observer);
        mListeners.insert(std::make_pair(event, theList));
        return NO_ERROR;
    }

    // Now we will have more than one listener to this event
    listener_list_t &theList = itListener->second;
    for (const auto &listener : theList)
        if (listener == observer) {
            LOGW("listener previously added, ignoring");
            return ALREADY_EXISTS;
        }

    theList.push_back(observer);

    itListener->second = theList;
    return status;
}

/**
 * Detach all bservers interface
 */
void
ITaskEventSource::cleanListener()
{
    LOG1("@%s", __FUNCTION__);

    std::lock_guard<std::mutex> l(mListenerLock);

    for (auto &listener : mListeners)
        listener.second.clear();

    mListeners.clear();
}


status_t
ITaskEventSource::notifyListeners(ITaskEventListener::PUTaskMessage *msg)
{
    LOG2("@%s", __FUNCTION__);
    status_t ret = NO_ERROR;
    std::lock_guard<std::mutex> l(mListenerLock);
    if (mListeners.size() > 0) {
        std::map<ITaskEventListener::PUTaskEventType, listener_list_t>::
                iterator it = mListeners.find(msg->event.type);
        if (it != mListeners.end()) {
            listener_list_t &theList = it->second;
            for (const auto &listener : theList)
                ret |= listener->notifyPUTaskEvent((ITaskEventListener::PUTaskMessage*)msg);
        }
    }

    return ret;
}

} // namespace camera2
} // namespace android
