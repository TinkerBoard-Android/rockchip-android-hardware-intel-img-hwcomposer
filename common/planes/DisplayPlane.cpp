/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#include <HwcTrace.h>
#include <Hwcomposer.h>
#include <DisplayPlane.h>
#include <GraphicBuffer.h>

namespace android {
namespace intel {

DisplayPlane::DisplayPlane(int index, int type, int disp)
    : mIndex(index),
      mType(type),
      mZOrder(-1),
      mDevice(disp),
      mInitialized(false),
      mDataBuffers(),
      mActiveBuffers(),
      mCacheCapacity(0),
      mIsProtectedBuffer(false),
      mTransform(PLANE_TRANSFORM_0),
      mCurrentDataBuffer(0),
      mUpdateMasks(0)
{
    CTRACE();
    memset(&mPosition, 0, sizeof(mPosition));
    memset(&mSrcCrop, 0, sizeof(mSrcCrop));
}

DisplayPlane::~DisplayPlane()
{
    WARN_IF_NOT_DEINIT();
}

bool DisplayPlane::initialize(uint32_t bufferCount)
{
    CTRACE();

    if (bufferCount < MIN_DATA_BUFFER_COUNT) {
        WTRACE("buffer count %d is too small", bufferCount);
        bufferCount = MIN_DATA_BUFFER_COUNT;
    }

    // create buffer cache, adding few extra slots as buffer rendering is async
    // buffer could still be queued in the display pipeline such that they
    // can't be unmapped]
    mCacheCapacity = bufferCount;
    mDataBuffers.setCapacity(bufferCount);
    mActiveBuffers.setCapacity(MIN_DATA_BUFFER_COUNT);
    mInitialized = true;
    return true;
}

void DisplayPlane::deinitialize()
{
    // invalidate cached data buffers
    if (mDataBuffers.size()) {
        // invalidateBufferCache will assert if object is not initialized
        // so invoking it only there is buffer to invalidate.
        invalidateBufferCache();
    }

    // invalidate active buffers
    if (mActiveBuffers.size()) {
        invalidateActiveBuffers();
    }

    mCurrentDataBuffer = 0;
    mInitialized = false;
}

void DisplayPlane::checkPosition(int& x, int& y, int& w, int& h)
{
    Drm *drm = Hwcomposer::getInstance().getDrm();
    drmModeModeInfo modeInfo;
    if (!drm->getModeInfo(mDevice, modeInfo)) {
        ETRACE("failed to get mode info");
        return;
    }
    drmModeModeInfoPtr mode = &modeInfo;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if ((x + w) > mode->hdisplay)
        w = mode->hdisplay - x;
    if ((y + h) > mode->vdisplay)
        h = mode->vdisplay - y;
}

void DisplayPlane::setPosition(int x, int y, int w, int h)
{
    ATRACE("Position = %d, %d - %dx%d", x, y, w, h);

    // if position is unchanged, skip it
    if (mPosition.x == x && mPosition.y == y &&
        mPosition.w == w && mPosition.h == h) {
        mUpdateMasks &= ~PLANE_POSITION_CHANGED;
        return;
    }

    mPosition.x = x;
    mPosition.y = y;
    mPosition.w = w;
    mPosition.h = h;

    mUpdateMasks |= PLANE_POSITION_CHANGED;
}

void DisplayPlane::setSourceCrop(int x, int y, int w, int h)
{
    ATRACE("Source crop = %d, %d - %dx%d", x, y, w, h);

    // if source crop is unchanged, skip it
    if (mSrcCrop.x == x && mSrcCrop.y == y &&
        mSrcCrop.w == w && mSrcCrop.h == h) {
        mUpdateMasks &= ~PLANE_SOURCE_CROP_CHANGED;
        return;
    }

    mSrcCrop.x = x;
    mSrcCrop.y = y;
    mSrcCrop.w = w;
    mSrcCrop.h = h;

    mUpdateMasks |= PLANE_SOURCE_CROP_CHANGED;
}

void DisplayPlane::setTransform(int trans)
{
    ATRACE("transform = %d", trans);

    if (mTransform == trans) {
        mUpdateMasks &= ~PLANE_TRANSFORM_CHANGED;
        return;
    }

    switch (trans) {
    case PLANE_TRANSFORM_90:
    case PLANE_TRANSFORM_180:
    case PLANE_TRANSFORM_270:
        mTransform = trans;
        break;
    default:
        mTransform = PLANE_TRANSFORM_0;
        break;
    }

    mUpdateMasks |= PLANE_TRANSFORM_CHANGED;
}

bool DisplayPlane::setDataBuffer(uint32_t handle)
{
    DataBuffer *buffer;
    BufferMapper *mapper;
    ssize_t index;
    bool ret;
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();

    RETURN_FALSE_IF_NOT_INIT();
    ATRACE("handle = %#x", handle);

    if (!handle) {
        WTRACE("invalid buffer handle");
        return false;
    }

    // do not need to update the buffer handle
    if (mCurrentDataBuffer != handle)
        mUpdateMasks |= PLANE_BUFFER_CHANGED;
    else
        mUpdateMasks &= ~PLANE_BUFFER_CHANGED;

    // if no update then do Not need set data buffer
    // TODO: this design assumes position/transform/sourcecrop are all set
    if (!mUpdateMasks)
        return true;

    buffer = bm->lockDataBuffer(handle);
    if (!buffer) {
        ETRACE("failed to get buffer");
        return false;
    }

    // update buffer's source crop
    buffer->setCrop(mSrcCrop.x, mSrcCrop.y, mSrcCrop.w, mSrcCrop.h);

    mIsProtectedBuffer = GraphicBuffer::isProtectedBuffer((GraphicBuffer*)buffer);
    // map buffer if it's not in cache
    index = mDataBuffers.indexOfKey(buffer->getKey());
    if (index < 0) {
        VTRACE("unmapped buffer, mapping...");
        mapper = mapBuffer(buffer);
        if (!mapper) {
            ETRACE("failed to map buffer %#x", handle);
            bm->unlockDataBuffer(buffer);
            return false;
        }
    } else {
        VTRACE("got mapper in saved data buffers and update source Crop");
        mapper = mDataBuffers.valueAt(index);
        mapper->setCrop(mSrcCrop.x, mSrcCrop.y, mSrcCrop.w, mSrcCrop.h);

    }

    // unlock buffer after getting mapper
    bm->unlockDataBuffer(buffer);
    buffer = NULL;

    ret = setDataBuffer(*mapper);
    if (ret) {
        mCurrentDataBuffer = handle;
        // update active buffers
        updateActiveBuffers(mapper);
    }
    return ret;
}

BufferMapper* DisplayPlane::mapBuffer(DataBuffer *buffer)
{
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();

    // invalidate buffer cache  if cache is full
    if ((int)mDataBuffers.size() >= mCacheCapacity) {
        invalidateBufferCache();
    }

    BufferMapper *mapper = bm->map(*buffer);
    if (!mapper) {
        ETRACE("failed to map buffer");
        return NULL;
    }

    // add it to data buffers
    ssize_t index = mDataBuffers.add(buffer->getKey(), mapper);
    if (index < 0) {
        ETRACE("failed to add mapper");
        bm->unmap(mapper);
        return NULL;
    }

    return mapper;
}

bool DisplayPlane::isActiveBuffer(BufferMapper *mapper)
{
    for (size_t i = 0; i < mActiveBuffers.size(); i++) {
        BufferMapper *activeMapper = mActiveBuffers.itemAt(i);
        if (!activeMapper)
            continue;
        if (activeMapper->getKey() == mapper->getKey())
            return true;
    }

    return false;
}

void DisplayPlane::updateActiveBuffers(BufferMapper *mapper)
{
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();

    // unmap the first entry (oldest buffer)
    if (mActiveBuffers.size() >= MIN_DATA_BUFFER_COUNT) {
        BufferMapper *oldest = mActiveBuffers.itemAt(0);
        bm->unmap(oldest);
        mActiveBuffers.removeAt(0);
    }

    // queue it to active buffers
    if (!isActiveBuffer(mapper)) {
        mapper->incRef();
        mActiveBuffers.push_back(mapper);
    }
}

void DisplayPlane::invalidateActiveBuffers()
{
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();
    BufferMapper* mapper;

    RETURN_VOID_IF_NOT_INIT();

    VTRACE("invalidating active buffers");

    for (size_t i = 0; i < mActiveBuffers.size(); i++) {
        mapper = mActiveBuffers.itemAt(i);
        // unmap it
        bm->unmap(mapper);
    }

    // clear recorded data buffers
    mActiveBuffers.clear();
}

void DisplayPlane::invalidateBufferCache()
{
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();
    BufferMapper* mapper;

    RETURN_VOID_IF_NOT_INIT();

    for (size_t i = 0; i < mDataBuffers.size(); i++) {
        mapper = mDataBuffers.valueAt(i);
        bm->unmap(mapper);
    }

    mDataBuffers.clear();
    // reset current buffer
    mCurrentDataBuffer = 0;
}

bool DisplayPlane::assignToDevice(int disp)
{
    RETURN_FALSE_IF_NOT_INIT();
    ATRACE("disp = %d", disp);

    mDevice = disp;
    return true;
}

bool DisplayPlane::flip(void *ctx)
{
    RETURN_FALSE_IF_NOT_INIT();

    // don't flip if no updates
    if (!mUpdateMasks)
        return false;
    else
        return true;
}

bool DisplayPlane::reset()
{
    // reclaim all allocated resources
    if (mDataBuffers.size() > 0) {
        invalidateBufferCache();
    }

    if (mActiveBuffers.size() > 0) {
        invalidateActiveBuffers();
    }

    return true;
}

void DisplayPlane::setZOrder(int zorder)
{
    mZOrder = zorder;
}

int DisplayPlane::getZOrder() const
{
    return mZOrder;
}

} // namespace intel
} // namespace android
