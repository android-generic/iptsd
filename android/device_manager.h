/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef IPTSD_ANDROID_DEVICE_MANAGER_H
#define IPTSD_ANDROID_DEVICE_MANAGER_H

#include "ipts_detector.h"

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent IPTS instances */
#define IPTSD_MAX_INSTANCES 8

/*
 * IPTS-related kernel modules.
 * Both create hidraw devices with the same IPTS HID descriptor protocol.
 *   - ipts:  MEI-based driver (Surface Pro 4 through Surface Pro 7)
 *   - ithc:  PCI-based Intel Touch Host Controller (Surface Pro 7+ and newer)
 */
#define IPTS_MODULE_IPTS "ipts"
#define IPTS_MODULE_ITHC "ithc"

/**
 * Represents a single running iptsd instance managing one hidraw device.
 */
struct iptsd_instance {
	int active;                    /* Non-zero if this slot is in use */
	pid_t pid;                     /* PID of the spawned iptsd process */
	char device_path[280];         /* e.g. "/dev/hidraw0" */
	enum ipts_device_type type;    /* Touchscreen or touchpad */
	uint16_t vendor;
	uint16_t product;
};

/**
 * The device manager maintains a table of IPTS instances and provides
 * operations to scan for devices, spawn/kill iptsd processes, and
 * react to child death or device hotplug.
 */
struct device_manager {
	struct iptsd_instance instances[IPTSD_MAX_INSTANCES];
	int instance_count;

	int inotify_fd;         /* inotify fd watching /dev/ */
	int signal_fd;          /* signalfd for SIGCHLD */
	int timer_fd;           /* timerfd for debouncing rescans */
	int resume_fd;          /* timerfd (CLOCK_BOOTTIME) for resume detection */
	int epoll_fd;           /* epoll fd for the main loop */

	char iptsd_bin[128];    /* resolved path to iptsd binary */

	/* Which IPTS-related modules are loaded (detected at init) */
	int has_ipts_module;    /* ipts (MEI-based) */
	int has_ithc_module;    /* ithc (PCI-based) */

	/*
	 * Resume detection: CLOCK_BOOTTIME includes suspend time,
	 * CLOCK_MONOTONIC does not. When their difference increases,
	 * a suspend/resume cycle has occurred.
	 */
	struct timespec last_suspend_delta;

	/*
	 * Module reload on resume is guarded by persist.iptsd.reload_on_resume.
	 * Default = 1 (enabled). Set to 0 to disable when the driver bug
	 * is fixed upstream.
	 */
	int reload_on_resume;

	volatile int should_stop;
};

/**
 * Initialize the device manager.
 *
 * Sets up inotify watch on /dev/, signalfd for SIGCHLD, timerfd for
 * debouncing, resume detection timer, and an epoll instance.
 * Detects which IPTS modules are loaded.
 *
 * @param mgr  Device manager to initialize (caller-allocated)
 * @return 0 on success, negative errno on error
 */
int dm_init(struct device_manager *mgr);

/**
 * Check whether any IPTS-related kernel modules are loaded.
 *
 * @param mgr  Device manager (updates has_ipts_module / has_ithc_module)
 * @return 1 if at least one module is loaded, 0 otherwise
 */
int dm_check_modules(struct device_manager *mgr);

/**
 * Scan /dev/hidraw* for IPTS devices and spawn iptsd for any new ones.
 *
 * Also cleans up instances whose devices no longer exist.
 *
 * @param mgr  Device manager
 * @return Number of active instances after scan, or negative errno
 */
int dm_scan_and_reconcile(struct device_manager *mgr);

/**
 * Handle a SIGCHLD by reaping dead children and marking their slots inactive.
 *
 * @param mgr  Device manager
 * @return Number of children reaped
 */
int dm_reap_children(struct device_manager *mgr);

/**
 * Kill all running iptsd instances.
 *
 * Sends SIGTERM to all tracked children and waits for them.
 *
 * @param mgr  Device manager
 */
void dm_kill_all(struct device_manager *mgr);

/**
 * Handle a detected resume event.
 *
 * If reload_on_resume is enabled, kills all iptsd instances and triggers
 * a reload of the loaded IPTS kernel module(s) (ipts and/or ithc) via
 * Android property. If disabled, only rescans for devices.
 *
 * @param mgr  Device manager
 */
void dm_handle_resume(struct device_manager *mgr);

/**
 * Run the main event loop.
 *
 * Blocks until should_stop is set (via signal handler).
 * Handles inotify events (device add/remove), SIGCHLD (child death),
 * timer expiry (debounced rescan), and resume detection.
 *
 * @param mgr  Device manager
 * @return 0 on clean shutdown, negative errno on error
 */
int dm_run(struct device_manager *mgr);

/**
 * Clean up the device manager.
 *
 * Closes all file descriptors. Does NOT kill children — call dm_kill_all first.
 *
 * @param mgr  Device manager
 */
void dm_destroy(struct device_manager *mgr);

#ifdef __cplusplus
}
#endif

#endif /* IPTSD_ANDROID_DEVICE_MANAGER_H */
