/*
 * Copyright (C) 2016-2018 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
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

#include <errno.h>
#include <pthread.h>
#include <inttypes.h>

#include <log/log.h>
#include <cutils/atomic.h>
#include <system/window.h>
#include <hardware/hardware.h>
#include <hardware/fb.h>

#if GRALLOC_VERSION_MAJOR == 1
#include <hardware/gralloc1.h>
#elif GRALLOC_VERSION_MAJOR == 0
#include <hardware/gralloc.h>
#endif

#include "mali_gralloc_module.h"
#include "mali_gralloc_private_interface_types.h"
#include "mali_gralloc_buffer.h"
#include "gralloc_helper.h"
#include "framebuffer_device.h"
#include "gralloc_buffer_priv.h"
#include "mali_gralloc_formats.h"
#include "mali_gralloc_usages.h"
#include "mali_gralloc_bufferaccess.h"
#include "mali_gralloc_reference.h"

#if GRALLOC_VERSION_MAJOR == 1
#include "mali_gralloc_public_interface.h"
#elif GRALLOC_VERSION_MAJOR == 0
#include "legacy/alloc_device.h"
#endif

#include "mali_gralloc_bufferallocation.h"
#include "mali_gralloc_bufferdescriptor.h"
#include "mali_gralloc_ion.h"

#ifndef GRALLOC_HARDWARE_GPU0
#define GRALLOC_HARDWARE_GPU0 "gpu0"
#endif

typedef struct alloc_device_t {
    struct hw_device_t common;
    int (*alloc)(struct alloc_device_t* dev,
            int w, int h, int format, int usage,
            buffer_handle_t* handle, int* stride);

    int (*free)(struct alloc_device_t* dev,
            buffer_handle_t handle);

    void (*dump)(struct alloc_device_t* dev, char *buff, int buff_len);

    void* reserved_proc[7];
} alloc_device_t;

static int mali_gralloc_alloc(alloc_device_t *dev,
                              int w, int h, int format, int usage,
                              buffer_handle_t *pHandle, int *pStride)
{
    if (!dev || !pHandle || !pStride || w <= 0 || h <= 0)
    {
        return -EINVAL;
    }

    buffer_descriptor_t buf_desc;
    memset(&buf_desc, 0, sizeof(buf_desc));
    buf_desc.signature = sizeof(buf_desc);
    buf_desc.width = w;
    buf_desc.height = h;
    buf_desc.producer_usage = (uint64_t)(uint32_t)usage;
    buf_desc.consumer_usage = (uint64_t)(uint32_t)usage;
    buf_desc.hal_format = (uint64_t)(uint32_t)format;
    buf_desc.layer_count = 1;
    buf_desc.fd_count = 1;
    buf_desc.format_type = MALI_GRALLOC_FORMAT_TYPE_USAGE;

    gralloc_buffer_descriptor_t descriptors[1];
    descriptors[0] = (gralloc_buffer_descriptor_t)(&buf_desc);

    mali_gralloc_module *m = reinterpret_cast<mali_gralloc_module *>(dev->common.module);
    bool shared = false;

    if (mali_gralloc_buffer_allocate(m, descriptors, 1, pHandle, &shared) < 0)
    {
        ALOGE("mali_gralloc_alloc: Failed to allocate buffer (%dx%d fmt=0x%x usage=0x%x)",
              w, h, format, usage);
        return -ENOMEM;
    }

    int stride = 0;
    mali_gralloc_query_getstride(*pHandle, &stride);
    *pStride = stride;

    return 0;
}

static int mali_gralloc_free(alloc_device_t *dev, buffer_handle_t handle)
{
    GRALLOC_UNUSED(dev);
    return mali_gralloc_buffer_free(handle);
}

static int mali_gralloc_alloc_close(struct hw_device_t *device)
{
    alloc_device_t *dev = reinterpret_cast<alloc_device_t *>(device);
    if (dev)
    {
        delete dev;
    }
    return 0;
}

static int alloc_device_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    GRALLOC_UNUSED(name);
    alloc_device_t *dev = new alloc_device_t;
    if (!dev)
    {
        return -ENOMEM;
    }

    memset(dev, 0, sizeof(*dev));
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = const_cast<hw_module_t *>(module);
    dev->common.close = mali_gralloc_alloc_close;
    dev->alloc = mali_gralloc_alloc;
    dev->free = mali_gralloc_free;

    private_module_t *p = reinterpret_cast<private_module_t *>(dev->common.module);
    if (p->ionfd == -1)
    {
        p->ionfd = mali_gralloc_ion_open();
    }

    *device = &dev->common;
    return 0;
}

static int mali_gralloc_module_device_open(const hw_module_t *module, const char *name,
                                           hw_device_t **device)
{
	int status = -EINVAL;

#if GRALLOC_VERSION_MAJOR == 1

	if (!strncmp(name, GRALLOC_HARDWARE_MODULE_ID, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = mali_gralloc_device_open(module, name, device);
	}
	else if (!strncmp(name, GRALLOC_HARDWARE_GPU0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = alloc_device_open(module, name, device);
	}

#else

	if (!strncmp(name, GRALLOC_HARDWARE_GPU0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = alloc_device_open(module, name, device);
	}

#endif
	else if (!strncmp(name, GRALLOC_HARDWARE_FB0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = framebuffer_device_open(module, name, device);
	}

	return status;
}

int gralloc_register_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_reference_retain(m, handle);
}

int gralloc_unregister_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_reference_release(m, handle, false);
}

int gralloc_lock(gralloc_module_t const *module, buffer_handle_t handle, int usage,
                 int l, int t, int w, int h, void **vaddr)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_lock(m, handle, (unsigned int)usage, l, t, w, h, vaddr);
}

int gralloc_lock_ycbcr(gralloc_module_t const *module, buffer_handle_t handle,
                       int usage, int l, int t, int w, int h, android_ycbcr *ycbcr)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_lock_ycbcr(m, handle, (unsigned int)usage, l, t, w, h, ycbcr);
}

int gralloc_unlock(gralloc_module_t const *module, buffer_handle_t handle)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_unlock(m, handle);
}

int gralloc_lock_async(gralloc_module_t const *module, buffer_handle_t handle,
                       int usage, int l, int t, int w, int h, void **vaddr,
                       int32_t fence_fd)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_lock_async(m, handle, (unsigned int)usage, l, t, w, h, vaddr, fence_fd);
}

int gralloc_lock_ycbcr_async(gralloc_module_t const *module, buffer_handle_t handle,
                             int usage, int l, int t, int w, int h,
                             android_ycbcr *ycbcr, int32_t fence_fd)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_lock_ycbcr_async(m, handle, (unsigned int)usage, l, t, w, h, ycbcr, fence_fd);
}

int gralloc_unlock_async(gralloc_module_t const *module, buffer_handle_t handle,
                         int32_t *fence_fd)
{
	const mali_gralloc_module *m = reinterpret_cast<const mali_gralloc_module *>(module);

	return mali_gralloc_unlock_async(m, handle, fence_fd);
}

struct hw_module_methods_t mali_gralloc_module_methods = { mali_gralloc_module_device_open };

/* Export the private module as HAL_MODULE_INFO_SYM symbol as required by Gralloc v0.3 / v1.0 */
struct private_module_t HAL_MODULE_INFO_SYM;
