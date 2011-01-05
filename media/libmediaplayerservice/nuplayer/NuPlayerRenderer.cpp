/*
 * Copyright (C) 2010 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerRenderer"
#include <utils/Log.h>

#include "NuPlayerRenderer.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {

NuPlayer::Renderer::Renderer(
        const sp<MediaPlayerBase::AudioSink> &sink,
        const sp<AMessage> &notify)
    : mAudioSink(sink),
      mNotify(notify),
      mNumFramesWritten(0),
      mDrainAudioQueuePending(false),
      mDrainVideoQueuePending(false),
      mAudioQueueGeneration(0),
      mVideoQueueGeneration(0),
      mAnchorTimeMediaUs(-1),
      mAnchorTimeRealUs(-1),
      mFlushingAudio(false),
      mFlushingVideo(false),
      mHasAudio(mAudioSink != NULL),
      mHasVideo(true),
      mSyncQueues(mHasAudio && mHasVideo) {
}

NuPlayer::Renderer::~Renderer() {
}

void NuPlayer::Renderer::queueBuffer(
        bool audio,
        const sp<ABuffer> &buffer,
        const sp<AMessage> &notifyConsumed) {
    sp<AMessage> msg = new AMessage(kWhatQueueBuffer, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setObject("buffer", buffer);
    msg->setMessage("notifyConsumed", notifyConsumed);
    msg->post();
}

void NuPlayer::Renderer::queueEOS(bool audio, status_t finalResult) {
    CHECK_NE(finalResult, (status_t)OK);

    sp<AMessage> msg = new AMessage(kWhatQueueEOS, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setInt32("finalResult", finalResult);
    msg->post();
}

void NuPlayer::Renderer::flush(bool audio) {
    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            CHECK(!mFlushingAudio);
            mFlushingAudio = true;
        } else {
            CHECK(!mFlushingVideo);
            mFlushingVideo = true;
        }
    }

    sp<AMessage> msg = new AMessage(kWhatFlush, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->post();
}

void NuPlayer::Renderer::signalTimeDiscontinuity() {
    CHECK(mAudioQueue.empty());
    CHECK(mVideoQueue.empty());
    mAnchorTimeMediaUs = -1;
    mAnchorTimeRealUs = -1;
    mSyncQueues = mHasAudio && mHasVideo;
}

void NuPlayer::Renderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatDrainAudioQueue:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mAudioQueueGeneration) {
                break;
            }

            mDrainAudioQueuePending = false;

            onDrainAudioQueue();

            postDrainAudioQueue();
            break;
        }

        case kWhatDrainVideoQueue:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mVideoQueueGeneration) {
                break;
            }

            mDrainVideoQueuePending = false;

            onDrainVideoQueue();

            postDrainVideoQueue();
            break;
        }

        case kWhatQueueBuffer:
        {
            onQueueBuffer(msg);
            break;
        }

        case kWhatQueueEOS:
        {
            onQueueEOS(msg);
            break;
        }

        case kWhatFlush:
        {
            onFlush(msg);
            break;
        }

        case kWhatAudioSinkChanged:
        {
            onAudioSinkChanged();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::Renderer::postDrainAudioQueue() {
    if (mDrainAudioQueuePending || mSyncQueues) {
        return;
    }

    if (mAudioQueue.empty()) {
        return;
    }

    mDrainAudioQueuePending = true;
    sp<AMessage> msg = new AMessage(kWhatDrainAudioQueue, id());
    msg->setInt32("generation", mAudioQueueGeneration);
    msg->post(10000);
}

void NuPlayer::Renderer::signalAudioSinkChanged() {
    (new AMessage(kWhatAudioSinkChanged, id()))->post();
}

void NuPlayer::Renderer::onDrainAudioQueue() {
    uint32_t numFramesPlayed;
    CHECK_EQ(mAudioSink->getPosition(&numFramesPlayed), (status_t)OK);

    ssize_t numFramesAvailableToWrite =
        mAudioSink->frameCount() - (mNumFramesWritten - numFramesPlayed);

    CHECK_GE(numFramesAvailableToWrite, 0);

    size_t numBytesAvailableToWrite =
        numFramesAvailableToWrite * mAudioSink->frameSize();

    while (numBytesAvailableToWrite > 0) {
        if (mAudioQueue.empty()) {
            break;
        }

        QueueEntry *entry = &*mAudioQueue.begin();

        if (entry->mBuffer == NULL) {
            // EOS

            notifyEOS(true /* audio */);

            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
            return;
        }

        if (entry->mOffset == 0) {
            int64_t mediaTimeUs;
            CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

            LOGV("rendering audio at media time %.2f secs", mediaTimeUs / 1E6);

            mAnchorTimeMediaUs = mediaTimeUs;

            uint32_t numFramesPlayed;
            CHECK_EQ(mAudioSink->getPosition(&numFramesPlayed), (status_t)OK);

            uint32_t numFramesPendingPlayout =
                mNumFramesWritten - numFramesPlayed;

            int64_t realTimeOffsetUs =
                (mAudioSink->latency() / 2  /* XXX */
                    + numFramesPendingPlayout
                        * mAudioSink->msecsPerFrame()) * 1000ll;

            // LOGI("realTimeOffsetUs = %lld us", realTimeOffsetUs);

            mAnchorTimeRealUs =
                ALooper::GetNowUs() + realTimeOffsetUs;
        }

        size_t copy = entry->mBuffer->size() - entry->mOffset;
        if (copy > numBytesAvailableToWrite) {
            copy = numBytesAvailableToWrite;
        }

        CHECK_EQ(mAudioSink->write(
                    entry->mBuffer->data() + entry->mOffset, copy),
                 (ssize_t)copy);

        entry->mOffset += copy;
        if (entry->mOffset == entry->mBuffer->size()) {
            entry->mNotifyConsumed->post();
            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
        }

        numBytesAvailableToWrite -= copy;
        mNumFramesWritten += copy / mAudioSink->frameSize();
    }

    notifyPosition();
}

void NuPlayer::Renderer::postDrainVideoQueue() {
    if (mDrainVideoQueuePending || mSyncQueues) {
        return;
    }

    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry &entry = *mVideoQueue.begin();

    sp<AMessage> msg = new AMessage(kWhatDrainVideoQueue, id());
    msg->setInt32("generation", mVideoQueueGeneration);

    int64_t delayUs;

    if (entry.mBuffer == NULL) {
        // EOS doesn't carry a timestamp.
        delayUs = 0;
    } else {
        int64_t mediaTimeUs;
        CHECK(entry.mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

        if (mAnchorTimeMediaUs < 0) {
            delayUs = 0;

            if (!mHasAudio) {
                mAnchorTimeMediaUs = mediaTimeUs;
                mAnchorTimeRealUs = ALooper::GetNowUs();
            }
        } else {
            int64_t realTimeUs =
                (mediaTimeUs - mAnchorTimeMediaUs) + mAnchorTimeRealUs;

            delayUs = realTimeUs - ALooper::GetNowUs();
        }
    }

    msg->post(delayUs);

    mDrainVideoQueuePending = true;
}

void NuPlayer::Renderer::onDrainVideoQueue() {
    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry *entry = &*mVideoQueue.begin();

    if (entry->mBuffer == NULL) {
        // EOS

        notifyEOS(false /* audio */);

        mVideoQueue.erase(mVideoQueue.begin());
        entry = NULL;
        return;
    }

#if 0
    int64_t mediaTimeUs;
    CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

    LOGI("rendering video at media time %.2f secs", mediaTimeUs / 1E6);
#endif

    entry->mNotifyConsumed->setInt32("render", true);
    entry->mNotifyConsumed->post();
    mVideoQueue.erase(mVideoQueue.begin());
    entry = NULL;

    notifyPosition();
}

void NuPlayer::Renderer::notifyEOS(bool audio) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatEOS);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->post();
}

void NuPlayer::Renderer::onQueueBuffer(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    sp<RefBase> obj;
    CHECK(msg->findObject("buffer", &obj));
    sp<ABuffer> buffer = static_cast<ABuffer *>(obj.get());

    sp<AMessage> notifyConsumed;
    CHECK(msg->findMessage("notifyConsumed", &notifyConsumed));

    QueueEntry entry;
    entry.mBuffer = buffer;
    entry.mNotifyConsumed = notifyConsumed;
    entry.mOffset = 0;
    entry.mFinalResult = OK;

    if (audio) {
        mAudioQueue.push_back(entry);
        postDrainAudioQueue();
    } else {
        mVideoQueue.push_back(entry);
        postDrainVideoQueue();
    }

    if (mSyncQueues && !mAudioQueue.empty() && !mVideoQueue.empty()) {
        int64_t firstAudioTimeUs;
        int64_t firstVideoTimeUs;
        CHECK((*mAudioQueue.begin()).mBuffer->meta()
                ->findInt64("timeUs", &firstAudioTimeUs));
        CHECK((*mVideoQueue.begin()).mBuffer->meta()
                ->findInt64("timeUs", &firstVideoTimeUs));

        int64_t diff = firstVideoTimeUs - firstAudioTimeUs;

        LOGV("queueDiff = %.2f secs", diff / 1E6);

        if (diff > 100000ll) {
            // Audio data starts More than 0.1 secs before video.
            // Drop some audio.

            (*mAudioQueue.begin()).mNotifyConsumed->post();
            mAudioQueue.erase(mAudioQueue.begin());
            return;
        }

        syncQueuesDone();
    }
}

void NuPlayer::Renderer::syncQueuesDone() {
    if (!mSyncQueues) {
        return;
    }

    mSyncQueues = false;

    if (!mAudioQueue.empty()) {
        postDrainAudioQueue();
    }

    if (!mVideoQueue.empty()) {
        postDrainVideoQueue();
    }
}

void NuPlayer::Renderer::onQueueEOS(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    int32_t finalResult;
    CHECK(msg->findInt32("finalResult", &finalResult));

    QueueEntry entry;
    entry.mOffset = 0;
    entry.mFinalResult = finalResult;

    if (audio) {
        mAudioQueue.push_back(entry);
        postDrainAudioQueue();
    } else {
        mVideoQueue.push_back(entry);
        postDrainVideoQueue();
    }
}

void NuPlayer::Renderer::onFlush(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    // If we're currently syncing the queues, i.e. dropping audio while
    // aligning the first audio/video buffer times and only one of the
    // two queues has data, we may starve that queue by not requesting
    // more buffers from the decoder. If the other source then encounters
    // a discontinuity that leads to flushing, we'll never find the
    // corresponding discontinuity on the other queue.
    // Therefore we'll stop syncing the queues if at least one of them
    // is flushed.
    syncQueuesDone();

    if (audio) {
        flushQueue(&mAudioQueue);

        Mutex::Autolock autoLock(mFlushLock);
        mFlushingAudio = false;

        mDrainAudioQueuePending = false;
        ++mAudioQueueGeneration;
    } else {
        flushQueue(&mVideoQueue);

        Mutex::Autolock autoLock(mFlushLock);
        mFlushingVideo = false;

        mDrainVideoQueuePending = false;
        ++mVideoQueueGeneration;
    }

    notifyFlushComplete(audio);
}

void NuPlayer::Renderer::flushQueue(List<QueueEntry> *queue) {
    while (!queue->empty()) {
        QueueEntry *entry = &*queue->begin();

        if (entry->mBuffer != NULL) {
            entry->mNotifyConsumed->post();
        }

        queue->erase(queue->begin());
        entry = NULL;
    }
}

void NuPlayer::Renderer::notifyFlushComplete(bool audio) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushComplete);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->post();
}

bool NuPlayer::Renderer::dropBufferWhileFlushing(
        bool audio, const sp<AMessage> &msg) {
    bool flushing = false;

    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            flushing = mFlushingAudio;
        } else {
            flushing = mFlushingVideo;
        }
    }

    if (!flushing) {
        return false;
    }

    sp<AMessage> notifyConsumed;
    if (msg->findMessage("notifyConsumed", &notifyConsumed)) {
        notifyConsumed->post();
    }

    return true;
}

void NuPlayer::Renderer::onAudioSinkChanged() {
    CHECK(!mDrainAudioQueuePending);
    mNumFramesWritten = 0;
}

void NuPlayer::Renderer::notifyPosition() {
    if (mAnchorTimeRealUs < 0 || mAnchorTimeMediaUs < 0) {
        return;
    }

    int64_t nowUs = ALooper::GetNowUs();
    int64_t positionUs = (nowUs - mAnchorTimeRealUs) + mAnchorTimeMediaUs;

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatPosition);
    notify->setInt64("positionUs", positionUs);
    notify->post();
}

}  // namespace android

