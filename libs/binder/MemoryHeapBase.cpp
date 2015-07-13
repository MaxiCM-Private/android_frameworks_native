/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "MemoryHeapBase"

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <cutils/log.h>
#include <cutils/ashmem.h>
#include <cutils/atomic.h>

#include <binder/MemoryHeapBase.h>

#ifdef USE_ION
#include <linux/ion.h>
#include <linux/pxa_ion.h>
#endif /* USE_ION */

#ifdef HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif


namespace android {

// ---------------------------------------------------------------------------

MemoryHeapBase::MemoryHeapBase()
    : mFD(-1), mSize(0), mBase(MAP_FAILED),
      mDevice(NULL), mNeedUnmap(false), mOffset(0), mDevFd(-1)
{
}

MemoryHeapBase::MemoryHeapBase(size_t size, uint32_t flags, char const * name)
    : mFD(-1), mSize(0), mBase(MAP_FAILED), mFlags(flags),
      mDevice(0), mNeedUnmap(false), mOffset(0), mDevFd(-1)
{
    const size_t pagesize = getpagesize();
    size = ((size + pagesize-1) & ~(pagesize-1));
    int fd = ashmem_create_region(name == NULL ? "MemoryHeapBase" : name, size);
    ALOGE_IF(fd<0, "error creating ashmem region: %s", strerror(errno));
    if (fd >= 0) {
        if (mapfd(fd, size) == NO_ERROR) {
            if (flags & READ_ONLY) {
                ashmem_set_prot_region(fd, PROT_READ);
            }
        }
    }
}

#ifdef USE_ION
MemoryHeapBase::MemoryHeapBase(const char* device, size_t size, uint32_t flags)
    : mFD(-1), mSize(0), mBase(MAP_FAILED), mFlags(flags),
      mDevice(0), mNeedUnmap(false), mOffset(0), mDevFd(-1)
{
    int open_flags = O_RDWR;
    if (flags & NO_CACHING)
        open_flags |= O_SYNC;

    int dev_fd = open(device, open_flags);
    ALOGE_IF(dev_fd<0, "error opening %s: %s", device, strerror(errno));
    if (dev_fd >= 0) {
        const size_t pagesize = getpagesize();
        size = ((size + pagesize-1) & ~(pagesize-1));
        status_t mapret = mapion(dev_fd, size);
        if (mapret == NO_ERROR) {
            mDevice = device;
        } else {
            ALOGE("mapion failed : %d", mapret);
        }
    }
}

MemoryHeapBase::MemoryHeapBase(int fd, size_t size, uint32_t flags, uint32_t offset)
    : mFD(-1), mSize(0), mBase(MAP_FAILED), mFlags(flags),
      mDevice(0), mNeedUnmap(false), mOffset(0), mDevFd(-1)
{
    const size_t pagesize = getpagesize();
    size = ((size + pagesize-1) & ~(pagesize-1));
    mapion(dup(fd), size, offset);
}

status_t MemoryHeapBase::mapion(int dev_fd, size_t size, uint32_t offset)
{
    struct ion_allocation_data req_alloc;
    struct ion_fd_data req_fd;
    int ret;

    if (size == 0) {
        ALOGE("mapion size = 0");
        return BAD_VALUE;
    }

    if ((mFlags & DONT_MAP_LOCALLY) == 0) {
        memset(&req_alloc, 0, sizeof(struct ion_allocation_data));
        req_alloc.len = size;
        req_alloc.align = PAGE_SIZE;
        if((mFlags & NO_CACHING) == 0){
            req_alloc.flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
        }
        // req_alloc.heap_id_mask = ION_HEAP_TYPE_DMA_MASK;
        req_alloc.heap_id_mask = ION_HEAP_CARVEOUT_MASK;
        ret = ioctl(dev_fd, ION_IOC_ALLOC, &req_alloc);
        if (ret < 0) {
            ALOGE("ION_IOC_ALLOC failed ret = %d : reason : %s", ret, strerror(errno));
            goto out;
        }
        memset(&req_fd, 0, sizeof(struct ion_fd_data));
        req_fd.handle = req_alloc.handle;
        ret = ioctl(dev_fd, ION_IOC_SHARE, &req_fd);
        if (ret < 0) {
            ALOGE("ION_IOC_SHARE failed = %d", ret);
            goto out;
        }

        void *base = (uint8_t*)mmap(0, size, PROT_READ|PROT_WRITE,
                                    MAP_SHARED, req_fd.fd, offset);
        if (base == MAP_FAILED) {
            ALOGE("mmap(fd=%d, size=%u) failed (%s)",
                 dev_fd, uint32_t(size), strerror(errno));
            goto out;
        }
        mBase = base;
        mNeedUnmap = true;
    } else {
        mBase = 0;
        mNeedUnmap = false;
    }
    /* buf fd is stored in mFD, node fd is stored in mDevFd */
    mFD = req_fd.fd;
    mDevFd = dev_fd;
    mSize = size;
    mOffset = offset;

    return NO_ERROR;
out:
    close(dev_fd);
    return -errno;
}

void MemoryHeapBase::dispose()
{
    int fd = android_atomic_or(-1, &mFD);
    if (fd >= 0) {
        if (mBase) {
            if (mNeedUnmap) {
                munmap(mBase, mSize);
            }

            if ((mDevFd > 0) && (fd > 0)) {
                /* using ION memory */
                struct ion_fd_data req_fd;
                struct ion_handle_data req;
                int ret;

                memset(&req_fd, 0, sizeof(struct ion_fd_data));
                req_fd.fd = fd; /* get buffer fd */
                ret = ioctl(mDevFd, ION_IOC_IMPORT, &req_fd);
                if (ret < 0) {
                    ALOGE("Failed to import ION buffer with buffer fd:%d, ret:%d",
                         fd, ret);
                    goto out;
                }
                memset(&req, 0, sizeof(struct ion_handle_data));
                req.handle = req_fd.handle;
                ret = ioctl(mDevFd, ION_IOC_FREE, &req);
                if (ret < 0) {
                    ALOGE("Failed to free ION buffer, ret:%d", ret);
                }
out:
                close(mDevFd);
            }
            mBase = 0;
            mSize = 0;
            close(fd);
            mFD = -1;
            mDevFd = -1;
        }
    }
}
#else

MemoryHeapBase::MemoryHeapBase(const char* device, size_t size, uint32_t flags)
    : mFD(-1), mSize(0), mBase(MAP_FAILED), mFlags(flags),
      mDevice(0), mNeedUnmap(false), mOffset(0)
{
    int open_flags = O_RDWR;
    if (flags & NO_CACHING)
        open_flags |= O_SYNC;

    int fd = open(device, open_flags);
    ALOGE_IF(fd<0, "error opening %s: %s", device, strerror(errno));
    if (fd >= 0) {
        const size_t pagesize = getpagesize();
        size = ((size + pagesize-1) & ~(pagesize-1));
        if (mapfd(fd, size) == NO_ERROR) {
            mDevice = device;
        }
    }
}

MemoryHeapBase::MemoryHeapBase(int fd, size_t size, uint32_t flags, uint32_t offset)
    : mFD(-1), mSize(0), mBase(MAP_FAILED), mFlags(flags),
      mDevice(0), mNeedUnmap(false), mOffset(0)
{
    const size_t pagesize = getpagesize();
    size = ((size + pagesize-1) & ~(pagesize-1));
    mapfd(dup(fd), size, offset);
}

void MemoryHeapBase::dispose()
{
    int fd = android_atomic_or(-1, &mFD);
    if (fd >= 0) {
        if (mNeedUnmap) {
            //ALOGD("munmap(fd=%d, base=%p, size=%lu)", fd, mBase, mSize);
            munmap(mBase, mSize);
        }
        mBase = 0;
        mSize = 0;
        close(fd);
    }
}
#endif

status_t MemoryHeapBase::mapfd(int fd, size_t size, uint32_t offset)
{
    if (size == 0) {
        // try to figure out the size automatically
#ifdef HAVE_ANDROID_OS
        // first try the PMEM ioctl
        pmem_region reg;
        int err = ioctl(fd, PMEM_GET_TOTAL_SIZE, &reg);
        if (err == 0)
            size = reg.len;
#endif
        if (size == 0) { // try fstat
            struct stat sb;
            if (fstat(fd, &sb) == 0)
                size = sb.st_size;
        }
        // if it didn't work, let mmap() fail.
    }

    if ((mFlags & DONT_MAP_LOCALLY) == 0) {
        void* base = (uint8_t*)mmap(0, size,
                PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset);
        if (base == MAP_FAILED) {
            ALOGE("mmap(fd=%d, size=%u) failed (%s)",
                    fd, uint32_t(size), strerror(errno));
            close(fd);
            return -errno;
        }
        //ALOGD("mmap(fd=%d, base=%p, size=%lu)", fd, base, size);
        mBase = base;
        mNeedUnmap = true;
    } else  {
        mBase = 0; // not MAP_FAILED
        mNeedUnmap = false;
    }
    mFD = fd;
    mSize = size;
    mOffset = offset;
    return NO_ERROR;
}

status_t MemoryHeapBase::init(int fd, void *base, int size, int flags, const char* device)
{
    if (mFD != -1) {
        return INVALID_OPERATION;
    }
    mFD = fd;
    mBase = base;
    mSize = size;
    mFlags = flags;
    mDevice = device;
    return NO_ERROR;
}

MemoryHeapBase::~MemoryHeapBase()
{
    dispose();
}


int MemoryHeapBase::getHeapID() const {
    return mFD;
}

void* MemoryHeapBase::getBase() const {
    return mBase;
}

size_t MemoryHeapBase::getSize() const {
    return mSize;
}

uint32_t MemoryHeapBase::getFlags() const {
    return mFlags;
}

const char* MemoryHeapBase::getDevice() const {
    return mDevice;
}

uint32_t MemoryHeapBase::getOffset() const {
    return mOffset;
}

// ---------------------------------------------------------------------------
}; // namespace android
