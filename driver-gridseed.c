/*
 * Copyright 2014 Luke Dashjr
 * Copyright 2014 Nate Woolls
 * Copyright 2014 GridSeed Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdbool.h>

#include "deviceapi.h"
#include "lowlevel.h"
#include "lowl-vcom.h"
#include "gc3355.h"

#define GRIDSEED_DEFAULT_FREQUENCY  600
// 60Kh/s at 700MHz in ms
#define GRIDSEED_HASH_SPEED			0.08571428571429

BFG_REGISTER_DRIVER(gridseed_drv)

static const struct bfg_set_device_definition gridseed_set_device_funcs_probe[];
static const struct bfg_set_device_definition gridseed_set_device_funcs_live[];

/*
 * helper functions
 */

static
struct cgpu_info *gridseed_alloc_device(const char *path, struct device_drv *driver, struct gc3355_info *info)
{
	struct cgpu_info *device = calloc(1, sizeof(struct cgpu_info));
	if (unlikely(!device))
		quit(1, "Failed to malloc cgpu_info");
	
	device->drv = driver;
	device->device_path = strdup(path);
	device->device_fd = -1;
	device->threads = 1;
	device->device_data = info;
	device->set_device_funcs = gridseed_set_device_funcs_live;
	
	return device;
}

static
struct gc3355_info *gridseed_alloc_info()
{
	struct gc3355_info *info = calloc(1, sizeof(struct gc3355_info));
	if (unlikely(!info))
		quit(1, "Failed to malloc gc3355_info");
	
	info->freq = GRIDSEED_DEFAULT_FREQUENCY;
	
	return info;
}

static
void gridseed_empty_work(int fd)
{
	unsigned char buf[GC3355_READ_SIZE];
	gc3355_read(fd, (char *)buf, GC3355_READ_SIZE);
}

static
struct thr_info *gridseed_thread_by_chip(const struct cgpu_info * const device, uint32_t const chip)
{
	const struct cgpu_info *proc = device_proc_by_id(device, chip);
	if (unlikely(!proc))
		proc = device;
	return proc->thr[0];
}

static
int64_t gridseed_calculate_chip_hashes(struct thr_info *thr)
{
	struct cgpu_info *device = thr->cgpu;
	struct gc3355_info *info = device->device_data;
	struct timeval old_scanhash_time = info->scanhash_time;

	timer_set_now(&info->scanhash_time);
	int elapsed_ms = ms_tdiff(&info->scanhash_time, &old_scanhash_time);

	return GRIDSEED_HASH_SPEED * (double)elapsed_ms * (double)(info->freq);
}

static
int64_t gridseed_hashes_done(struct thr_info *thr)
{
	struct cgpu_info *device = thr->cgpu;
	int64_t chip_hashes = gridseed_calculate_chip_hashes(thr);
	int64_t total_hashes = 0;

	for_each_managed_proc(proc, device)
	{
		total_hashes += chip_hashes;
		hashes_done2(proc->thr[0], chip_hashes, NULL);
	}

	return total_hashes;
}

/*
 * device detection
 */

static
bool gridseed_detect_custom(const char *path, struct device_drv *driver, struct gc3355_info *info)
{
	int fd = gc3355_open(path);
	if(fd < 0)
		return false;
	
	gridseed_empty_work(fd);
	
	int64_t fw_version = gc3355_get_firmware_version(fd);
	
	if (fw_version == -1)
	{
		applog(LOG_DEBUG, "%s: Invalid detect response from %s", gridseed_drv.dname, path);
		gc3355_close(fd);
		return false;
	}
	
	if (serial_claim_v(path, driver))
		return false;
	
	info->chips = GC3355_ORB_DEFAULT_CHIPS;
	if((fw_version & 0xffff) == 0x1402)
		info->chips = GC3355_BLADE_DEFAULT_CHIPS;
	
	//pick up any user-defined settings passed in via --set
	drv_set_defaults(driver, gridseed_set_device_funcs_probe, info, path, detectone_meta_info.serial, 1);
	
	struct cgpu_info *device = gridseed_alloc_device(path, driver, info);
	device->device_fd = fd;
	device->procs = info->chips;
	
	if (!add_cgpu(device))
		return false;
	
	gc3355_init_miner(device->device_fd, info->freq);
	
	applog(LOG_INFO, "Found %"PRIpreprv" at %s", device->proc_repr, path);
	applog(LOG_DEBUG, "%"PRIpreprv": Init: firmware=%"PRId64", chips=%d", device->proc_repr, fw_version, info->chips);
	
	return true;
}

static
bool gridseed_detect_one(const char *path)
{
	struct gc3355_info *info = gridseed_alloc_info();
	
	if (!gridseed_detect_custom(path, &gridseed_drv, info))
	{
		free(info);
		return false;
	}
	return true;
}

static
bool gridseed_lowl_probe(const struct lowlevel_device_info * const info)
{
	return vcom_lowl_probe_wrapper(info, gridseed_detect_one);
}

/*
 * setup & shutdown
 */

static
bool gridseed_thread_prepare(struct thr_info *thr)
{
	thr->cgpu_data = calloc(1, sizeof(*thr->cgpu_data));
	
	struct cgpu_info *device = thr->cgpu;
	device->min_nonce_diff = 1./0x10000;

	return true;
}

static
void gridseed_thread_shutdown(struct thr_info *thr)
{
	struct cgpu_info *device = thr->cgpu;

	gc3355_close(device->device_fd);
	free(thr->cgpu_data);
}

/*
 * scanhash mining loop
 */

// send work to the device
static
bool gridseed_prepare_work(struct thr_info __maybe_unused *thr, struct work *work)
{
	struct cgpu_info *device = thr->cgpu;
	struct gc3355_info *info = device->device_data;
	unsigned char cmd[156];
	
	timer_set_now(&info->scanhash_time);

	gc3355_scrypt_reset(device->device_fd);
	gc3355_scrypt_prepare_work(cmd, work);

	// See https://github.com/gridseed/gc3355-doc/blob/master/GC3355_DataSheet.pdf
	// WAIT: Before start a new transaction, WAIT Cycle must be inserted.
	// WAIT Cycle value is programmable register in UART and default wait
	// time is UART receive 32 bits time (One DATA Cycle).
	// Note: prevents register corruption
	cgsleep_ms(100);
	
	// send work
	if (sizeof(cmd) != gc3355_write(device->device_fd, cmd, sizeof(cmd)))
	{
		applog(LOG_ERR, "%s: Failed to send work", device->dev_repr);
		dev_error(device, REASON_DEV_COMMS_ERROR);
		return false;
	}

	// after sending work to the device, minerloop_scanhash-based
	// drivers must set work->blk.nonce to the last nonce to hash
	work->blk.nonce = 0xffffffff;

	return true;
}

static
void gridseed_submit_nonce(struct thr_info * const thr, const unsigned char buf[GC3355_READ_SIZE], struct work * const work)
{
	struct cgpu_info *device = thr->cgpu;
	
	uint32_t nonce = *(uint32_t *)(buf + 4);
	nonce = le32toh(nonce);
	uint32_t chip = nonce / (work->blk.nonce / device->procs);
	
	struct thr_info *proc_thr = gridseed_thread_by_chip(device, chip);
	
	submit_nonce(proc_thr, work, nonce);
}

// read from device for nonce or command
// unless the device can target specific nonce ranges, the scanhash routine should loop
// until the device has processed the work item, scanning the full nonce range
// return the total number of hashes done
static
int64_t gridseed_scanhash(struct thr_info *thr, struct work *work, int64_t __maybe_unused max_nonce)
{
	struct cgpu_info *device = thr->cgpu;
	unsigned char buf[GC3355_READ_SIZE];
	int read = 0;
	int fd = device->device_fd;
	int64_t total_hashes = 0;

	while (!thr->work_restart                                                   // true when new work is available (miner.c)
	    && ((read = gc3355_read(fd, (char *)buf, GC3355_READ_SIZE)) >= 0)       // only check for failure - allow 0 bytes
	    && (total_hashes < max_nonce))                                          // false when we've had time to scan a range
	{
		total_hashes += gridseed_hashes_done(thr);

		if (read == 0)
			continue;

		if ((buf[0] == 0x55) && (buf[1] == 0x20))
			gridseed_submit_nonce(thr, buf, work);
		else
			applog(LOG_ERR, "%"PRIpreprv": Unrecognized response", device->proc_repr);
	}

	if (read == -1)
	{
		applog(LOG_ERR, "%s: Failed to read result", device->dev_repr);
		dev_error(device, REASON_DEV_COMMS_ERROR);
	}

	gridseed_hashes_done(thr);

	return 0;
}

/*
 * specify settings / options
 */

// support for --set-device
// must be set before probing the device

static
const char *gridseed_set_clock(struct cgpu_info * const device, const char * const option, const char * const setting, char * const replybuf, enum bfg_set_device_replytype * const success)
{
	struct gc3355_info * const info = device->device_data;
	int val = atoi(setting);

	info->freq = val;
	// below required as we may already be mining
	gc3355_set_pll_freq(device->device_fd, val);

	return NULL;
}

static
const char *gridseed_set_chips(struct cgpu_info * const device, const char * const option, const char * const setting, char * const replybuf, enum bfg_set_device_replytype * const success)
{
	struct gc3355_info * const info = device->device_data;
	int val = atoi(setting);
	
	info->chips = val;
	
	return NULL;
}

// for setting clock and chips during probe / detect
static
const struct bfg_set_device_definition gridseed_set_device_funcs_probe[] = {
	{ "clock", gridseed_set_clock, NULL },
	{ "chips", gridseed_set_chips, NULL },
	{ NULL },
};

// for setting clock while mining
static
const struct bfg_set_device_definition gridseed_set_device_funcs_live[] = {
	{ "clock", gridseed_set_clock, NULL },
	{ NULL },
};

struct device_drv gridseed_drv =
{
	// metadata
	.dname = "gridseed",
	.name = "GSD",
	.supported_algos = POW_SCRYPT,
	
	// detect device
	.lowl_probe = gridseed_lowl_probe,
	
	// initialize device
	.thread_prepare = gridseed_thread_prepare,
	
	// specify mining type - scanhash
	.minerloop = minerloop_scanhash,
	
	// scanhash mining hooks
	.prepare_work = gridseed_prepare_work,
	.scanhash = gridseed_scanhash,
	
	// teardown device
	.thread_shutdown = gridseed_thread_shutdown,
};
