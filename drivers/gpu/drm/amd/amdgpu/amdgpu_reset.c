/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/devcoredump.h>
#include <generated/utsrelease.h>

#include "amdgpu_reset.h"
#include "aldebaran.h"
#include "sienna_cichlid.h"
#include "smu_v13_0_10.h"

int amdgpu_reset_init(struct amdgpu_device *adev)
{
	int ret = 0;

	switch (amdgpu_ip_version(adev, MP1_HWIP, 0)) {
	case IP_VERSION(13, 0, 2):
	case IP_VERSION(13, 0, 6):
		ret = aldebaran_reset_init(adev);
		break;
	case IP_VERSION(11, 0, 7):
		ret = sienna_cichlid_reset_init(adev);
		break;
	case IP_VERSION(13, 0, 10):
		ret = smu_v13_0_10_reset_init(adev);
		break;
	default:
		break;
	}

	return ret;
}

int amdgpu_reset_fini(struct amdgpu_device *adev)
{
	int ret = 0;

	switch (amdgpu_ip_version(adev, MP1_HWIP, 0)) {
	case IP_VERSION(13, 0, 2):
	case IP_VERSION(13, 0, 6):
		ret = aldebaran_reset_fini(adev);
		break;
	case IP_VERSION(11, 0, 7):
		ret = sienna_cichlid_reset_fini(adev);
		break;
	case IP_VERSION(13, 0, 10):
		ret = smu_v13_0_10_reset_fini(adev);
		break;
	default:
		break;
	}

	return ret;
}

int amdgpu_reset_prepare_hwcontext(struct amdgpu_device *adev,
				   struct amdgpu_reset_context *reset_context)
{
	struct amdgpu_reset_handler *reset_handler = NULL;

	if (adev->reset_cntl && adev->reset_cntl->get_reset_handler)
		reset_handler = adev->reset_cntl->get_reset_handler(
			adev->reset_cntl, reset_context);
	if (!reset_handler)
		return -EOPNOTSUPP;

	return reset_handler->prepare_hwcontext(adev->reset_cntl,
						reset_context);
}

int amdgpu_reset_perform_reset(struct amdgpu_device *adev,
			       struct amdgpu_reset_context *reset_context)
{
	int ret;
	struct amdgpu_reset_handler *reset_handler = NULL;

	if (adev->reset_cntl)
		reset_handler = adev->reset_cntl->get_reset_handler(
			adev->reset_cntl, reset_context);
	if (!reset_handler)
		return -EOPNOTSUPP;

	ret = reset_handler->perform_reset(adev->reset_cntl, reset_context);
	if (ret)
		return ret;

	return reset_handler->restore_hwcontext(adev->reset_cntl,
						reset_context);
}


void amdgpu_reset_destroy_reset_domain(struct kref *ref)
{
	struct amdgpu_reset_domain *reset_domain = container_of(ref,
								struct amdgpu_reset_domain,
								refcount);
	if (reset_domain->wq)
		destroy_workqueue(reset_domain->wq);

	kvfree(reset_domain);
}

struct amdgpu_reset_domain *amdgpu_reset_create_reset_domain(enum amdgpu_reset_domain_type type,
							     char *wq_name)
{
	struct amdgpu_reset_domain *reset_domain;

	reset_domain = kvzalloc(sizeof(struct amdgpu_reset_domain), GFP_KERNEL);
	if (!reset_domain) {
		DRM_ERROR("Failed to allocate amdgpu_reset_domain!");
		return NULL;
	}

	reset_domain->type = type;
	kref_init(&reset_domain->refcount);

	reset_domain->wq = create_singlethread_workqueue(wq_name);
	if (!reset_domain->wq) {
		DRM_ERROR("Failed to allocate wq for amdgpu_reset_domain!");
		amdgpu_reset_put_reset_domain(reset_domain);
		return NULL;

	}

	atomic_set(&reset_domain->in_gpu_reset, 0);
	atomic_set(&reset_domain->reset_res, 0);
	init_rwsem(&reset_domain->sem);

	return reset_domain;
}

void amdgpu_device_lock_reset_domain(struct amdgpu_reset_domain *reset_domain)
{
	atomic_set(&reset_domain->in_gpu_reset, 1);
	down_write(&reset_domain->sem);
}


void amdgpu_device_unlock_reset_domain(struct amdgpu_reset_domain *reset_domain)
{
	atomic_set(&reset_domain->in_gpu_reset, 0);
	up_write(&reset_domain->sem);
}

#ifndef CONFIG_DEV_COREDUMP
void amdgpu_coredump(struct amdgpu_device *adev, bool vram_lost,
		     struct amdgpu_reset_context *reset_context)
{
}
#else
static ssize_t
amdgpu_devcoredump_read(char *buffer, loff_t offset, size_t count,
			void *data, size_t datalen)
{
	struct drm_printer p;
	struct amdgpu_coredump_info *coredump = data;
	struct drm_print_iterator iter;
	int i;

	iter.data = buffer;
	iter.offset = 0;
	iter.start = offset;
	iter.remain = count;

	p = drm_coredump_printer(&iter);

	drm_printf(&p, "**** AMDGPU Device Coredump ****\n");
	drm_printf(&p, "version: " AMDGPU_COREDUMP_VERSION "\n");
	drm_printf(&p, "kernel: " UTS_RELEASE "\n");
	drm_printf(&p, "module: " KBUILD_MODNAME "\n");
	drm_printf(&p, "time: %lld.%09ld\n", coredump->reset_time.tv_sec,
			coredump->reset_time.tv_nsec);

	if (coredump->reset_task_info.pid)
		drm_printf(&p, "process_name: %s PID: %d\n",
			   coredump->reset_task_info.process_name,
			   coredump->reset_task_info.pid);

	if (coredump->ring) {
		drm_printf(&p, "\nRing timed out details\n");
		drm_printf(&p, "IP Type: %d Ring Name: %s\n",
			   coredump->ring->funcs->type,
			   coredump->ring->name);
	}

	if (coredump->adev) {
		struct amdgpu_vm_fault_info *fault_info =
			&coredump->adev->vm_manager.fault_info;

		drm_printf(&p, "\n[%s] Page fault observed\n",
			   fault_info->vmhub ? "mmhub" : "gfxhub");
		drm_printf(&p, "Faulty page starting at address: 0x%016llx\n",
			   fault_info->addr);
		drm_printf(&p, "Protection fault status register: 0x%x\n\n",
			   fault_info->status);
	}

	drm_printf(&p, "Ring buffer information\n");
	for (int i = 0; i < coredump->adev->num_rings; i++) {
		int j = 0;
		struct amdgpu_ring *ring = coredump->adev->rings[i];

		drm_printf(&p, "ring name: %s\n", ring->name);
		drm_printf(&p, "Rptr: 0x%llx Wptr: 0x%llx RB mask: %x\n",
			   amdgpu_ring_get_rptr(ring),
			   amdgpu_ring_get_wptr(ring),
			   ring->buf_mask);
		drm_printf(&p, "Ring size in dwords: %d\n",
			   ring->ring_size / 4);
		drm_printf(&p, "Ring contents\n");
		drm_printf(&p, "Offset \t Value\n");

		while (j < ring->ring_size) {
			drm_printf(&p, "0x%x \t 0x%x\n", j, ring->ring[j/4]);
			j += 4;
		}
	}

	if (coredump->reset_vram_lost)
		drm_printf(&p, "VRAM is lost due to GPU reset!\n");
	if (coredump->adev->reset_info.num_regs) {
		drm_printf(&p, "AMDGPU register dumps:\nOffset:     Value:\n");

		for (i = 0; i < coredump->adev->reset_info.num_regs; i++)
			drm_printf(&p, "0x%08x: 0x%08x\n",
				   coredump->adev->reset_info.reset_dump_reg_list[i],
				   coredump->adev->reset_info.reset_dump_reg_value[i]);
	}

	return count - iter.remain;
}

static void amdgpu_devcoredump_free(void *data)
{
	kfree(data);
}

void amdgpu_coredump(struct amdgpu_device *adev, bool vram_lost,
		     struct amdgpu_reset_context *reset_context)
{
	struct amdgpu_coredump_info *coredump;
	struct drm_device *dev = adev_to_drm(adev);
	struct amdgpu_job *job = reset_context->job;
	struct drm_sched_job *s_job;

	coredump = kzalloc(sizeof(*coredump), GFP_NOWAIT);

	if (!coredump) {
		DRM_ERROR("%s: failed to allocate memory for coredump\n", __func__);
		return;
	}

	coredump->reset_vram_lost = vram_lost;

	if (reset_context->job && reset_context->job->vm) {
		struct amdgpu_task_info *ti;
		struct amdgpu_vm *vm = reset_context->job->vm;

		ti = amdgpu_vm_get_task_info_vm(vm);
		if (ti) {
			coredump->reset_task_info = *ti;
			amdgpu_vm_put_task_info(ti);
		}
	}

	if (job) {
		s_job = &job->base;
		coredump->ring = to_amdgpu_ring(s_job->sched);
	}

	coredump->adev = adev;

	ktime_get_ts64(&coredump->reset_time);

	dev_coredumpm(dev->dev, THIS_MODULE, coredump, 0, GFP_NOWAIT,
		      amdgpu_devcoredump_read, amdgpu_devcoredump_free);
}
#endif
