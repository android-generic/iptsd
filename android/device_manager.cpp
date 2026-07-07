/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Device manager for iptsd-runner.
 *
 * Orchestrates scanning for IPTS hidraw devices, spawning one iptsd
 * process per device, monitoring children via signalfd/SIGCHLD, and
 * reacting to device hotplug via inotify on /dev/.
 *
 * The main loop uses epoll to multiplex:
 *   - inotify fd  → hidraw device add/remove
 *   - signal fd   → SIGCHLD (child died)
 *   - timer fd    → debounced rescan after device change
 *   - resume fd   → periodic CLOCK_BOOTTIME timer for resume detection
 *
 * Resume workaround:
 *   The ipts (and potentially ithc) kernel driver can fail to
 *   reinitialize after suspend. When this happens the hidraw device
 *   stays but becomes non-functional. The workaround is to reload
 *   the kernel module(s) on resume. This behavior is guarded by
 *   the property persist.iptsd.reload_on_resume (default "1").
 *   Set to "0" when the driver bug is fixed upstream.
 */

#include "device_manager.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <cutils/properties.h>
#define LOG_TAG "iptsd-runner"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define ALOGI(...) fprintf(stderr, "I: " __VA_ARGS__), fprintf(stderr, "\n")
#define ALOGE(...) fprintf(stderr, "E: " __VA_ARGS__), fprintf(stderr, "\n")
#define ALOGW(...) fprintf(stderr, "W: " __VA_ARGS__), fprintf(stderr, "\n")
#define ALOGD(...) fprintf(stderr, "D: " __VA_ARGS__), fprintf(stderr, "\n")
#endif

/* Debounce delay for rescans after inotify events (milliseconds) */
#define RESCAN_DEBOUNCE_MS 500

/* Delay before restarting a crashed instance (seconds) */
#define RESTART_DELAY_S 1

/* Maximum consecutive restart failures before giving up on a device */
#define MAX_RESTART_FAILURES 10

/* Resume detection polling interval (seconds) */
#define RESUME_POLL_INTERVAL_S 2

/*
 * Minimum suspend time threshold (seconds) to trigger resume handling.
 * Short sleeps (< 1s) don't typically cause driver issues.
 */
#define SUSPEND_THRESHOLD_S 1

/*
 * Delay after setting the module reload property, waiting for init
 * to process the modprobe commands and for hidraw to appear (seconds).
 */
#define MODULE_RELOAD_DELAY_S 3

/*
 * Android property used to trigger IPTS module reload from init.
 * The .rc file should have triggers on this property.
 * Uses vendor. prefix for Treble compliance.
 */
#define PROP_RELOAD_IPTS "vendor.iptsd.reload_driver"

/*
 * Android property to enable/disable module reload on resume.
 * "1" = reload on every resume (workaround for driver bug).
 * "0" (default) = skip module reload, only rescan devices.
 * Uses persist.vendor. prefix for Treble compliance.
 */
#define PROP_RELOAD_ON_RESUME "persist.vendor.iptsd.reload_on_resume"

/*
 * Candidate paths for the iptsd binary.
 * Supports both /vendor (separate vendor partition) and
 * /system/vendor (legacy layout, e.g. Android-x86/BlissOS).
 */
static const char *iptsd_bin_candidates[] = {
	"/vendor/bin/iptsd",
	"/system/vendor/bin/iptsd",
	NULL,
};

/* ---- helpers ---- */

static const char *type_str(enum ipts_device_type type)
{
	switch (type) {
	case IPTS_DEVICE_TYPE_TOUCHSCREEN:
		return "touchscreen";
	case IPTS_DEVICE_TYPE_TOUCHPAD:
		return "touchpad";
	default:
		return "unknown";
	}
}

/**
 * Compute the difference between CLOCK_BOOTTIME and CLOCK_MONOTONIC.
 * This represents the total time spent in suspend.
 * When this increases, a suspend/resume cycle has occurred.
 */
static void get_suspend_delta(struct timespec *delta)
{
	struct timespec boottime, monotonic;

	clock_gettime(CLOCK_BOOTTIME, &boottime);
	clock_gettime(CLOCK_MONOTONIC, &monotonic);

	delta->tv_sec = boottime.tv_sec - monotonic.tv_sec;
	delta->tv_nsec = boottime.tv_nsec - monotonic.tv_nsec;

	if (delta->tv_nsec < 0) {
		delta->tv_sec--;
		delta->tv_nsec += 1000000000L;
	}
}

/**
 * Check if a kernel module is currently loaded by scanning /proc/modules.
 *
 * @param module_name  Name of the module to check (e.g. "ipts", "ithc")
 * @return 1 if loaded, 0 if not loaded or error
 */
static int is_module_loaded(const char *module_name)
{
	FILE *fp;
	char line[256];
	size_t name_len = strlen(module_name);

	fp = fopen("/proc/modules", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		/*
		 * /proc/modules format: "name size refcount deps state addr"
		 * The module name is the first whitespace-delimited token.
		 */
		if (strncmp(line, module_name, name_len) == 0 &&
		    (line[name_len] == ' ' || line[name_len] == '\t')) {
			fclose(fp);
			return 1;
		}
	}

	fclose(fp);
	return 0;
}

/**
 * Read the reload_on_resume property (Android) or return default (non-Android).
 */
static int get_reload_on_resume(void)
{
#ifdef __ANDROID__
	char val[PROPERTY_VALUE_MAX] = {0};

	property_get(PROP_RELOAD_ON_RESUME, val, "0");
	return (val[0] != '0');
#else
	/* Non-Android: default enabled, can be overridden via env */
	const char *env = getenv("IPTSD_RELOAD_ON_RESUME");

	if (env && env[0] == '0')
		return 0;
	return 1;
#endif
}

/**
 * Check if a device path is already tracked by an active instance.
 */
static struct iptsd_instance *find_instance_by_path(struct device_manager *mgr,
						    const char *path)
{
	int i;

	for (i = 0; i < mgr->instance_count; i++) {
		if (mgr->instances[i].active &&
		    strcmp(mgr->instances[i].device_path, path) == 0)
			return &mgr->instances[i];
	}

	return NULL;
}

/**
 * Find an active instance by PID.
 */
static struct iptsd_instance *find_instance_by_pid(struct device_manager *mgr,
						   pid_t pid)
{
	int i;

	for (i = 0; i < mgr->instance_count; i++) {
		if (mgr->instances[i].active &&
		    mgr->instances[i].pid == pid)
			return &mgr->instances[i];
	}

	return NULL;
}

/**
 * Allocate a new instance slot.
 */
static struct iptsd_instance *alloc_instance(struct device_manager *mgr)
{
	int i;

	/* Try to reuse an inactive slot */
	for (i = 0; i < mgr->instance_count; i++) {
		if (!mgr->instances[i].active)
			return &mgr->instances[i];
	}

	/* Allocate a new slot */
	if (mgr->instance_count >= IPTSD_MAX_INSTANCES)
		return NULL;

	return &mgr->instances[mgr->instance_count++];
}

/**
 * Spawn an iptsd process for the given device path.
 */
static int spawn_iptsd(struct device_manager *mgr, const char *device_path,
		       const struct ipts_device_info *info)
{
	pid_t pid;
	struct iptsd_instance *inst;

	/* Don't spawn if already tracked */
	if (find_instance_by_path(mgr, device_path))
		return 0;

	inst = alloc_instance(mgr);
	if (!inst) {
		ALOGE("Maximum number of instances (%d) reached, cannot spawn for %s",
		      IPTSD_MAX_INSTANCES, device_path);
		return -ENOMEM;
	}

	pid = fork();
	if (pid < 0) {
		ALOGE("fork() failed for %s: %s", device_path, strerror(errno));
		return -errno;
	}

	if (pid == 0) {
		/* Child — exec iptsd */
		execl(mgr->iptsd_bin, "iptsd", device_path, (char *)NULL);

		/* If exec fails, log and exit */
		ALOGE("execl(%s, %s) failed: %s",
		      mgr->iptsd_bin, device_path, strerror(errno));
		_exit(127);
	}

	/* Parent — track the instance */
	memset(inst, 0, sizeof(*inst));
	inst->active = 1;
	inst->pid = pid;
	snprintf(inst->device_path, sizeof(inst->device_path), "%s", device_path);
	inst->type = info->type;
	inst->vendor = info->vendor;
	inst->product = info->product;

	ALOGI("Spawned iptsd (pid %d) for %s [%04x:%04x %s]",
	      pid, device_path, info->vendor, info->product,
	      type_str(info->type));

	return 0;
}

/**
 * Arm the debounce timer to trigger a rescan after RESCAN_DEBOUNCE_MS.
 */
static void arm_rescan_timer(struct device_manager *mgr)
{
	struct itimerspec ts;

	memset(&ts, 0, sizeof(ts));
	ts.it_value.tv_sec = RESCAN_DEBOUNCE_MS / 1000;
	ts.it_value.tv_nsec = (RESCAN_DEBOUNCE_MS % 1000) * 1000000L;

	timerfd_settime(mgr->timer_fd, 0, &ts, NULL);
}

/**
 * Request IPTS-related kernel module reload via Android property trigger.
 *
 * Sets PROP_RELOAD_IPTS property to a value indicating which modules
 * to reload. The .rc file handles the actual rmmod + modprobe via init,
 * which runs modprobe in the vendor_modprobe SELinux domain.
 *
 * @param mgr  Device manager (used to determine which modules are loaded)
 */
static void request_module_reload(struct device_manager *mgr)
{
	/* Re-check which modules are loaded right now */
	dm_check_modules(mgr);

	if (!mgr->has_ipts_module && !mgr->has_ithc_module) {
		ALOGW("No IPTS modules loaded, skipping reload");
		return;
	}

#ifdef __ANDROID__
	/*
	 * Property value encodes which modules to reload:
	 *   "ipts"     - reload ipts only
	 *   "ithc"     - reload ithc only
	 *   "ipts,ithc" - reload both
	 */
	char val[64] = {0};

	if (mgr->has_ipts_module && mgr->has_ithc_module)
		snprintf(val, sizeof(val), "%s,%s",
			 IPTS_MODULE_IPTS, IPTS_MODULE_ITHC);
	else if (mgr->has_ipts_module)
		snprintf(val, sizeof(val), "%s", IPTS_MODULE_IPTS);
	else
		snprintf(val, sizeof(val), "%s", IPTS_MODULE_ITHC);

	ALOGI("Requesting module reload via property %s=%s",
	      PROP_RELOAD_IPTS, val);
	property_set(PROP_RELOAD_IPTS, val);
#else
	ALOGI("Requesting module reload (non-Android: using system())");
	if (mgr->has_ipts_module) {
		int ret = system("modprobe -r ipts 2>/dev/null;"
				 " modprobe ipts 2>/dev/null");
		if (ret != 0)
			ALOGW("ipts module reload returned %d", ret);
	}
	if (mgr->has_ithc_module) {
		int ret = system("modprobe -r ithc 2>/dev/null;"
				 " modprobe ithc 2>/dev/null");
		if (ret != 0)
			ALOGW("ithc module reload returned %d", ret);
	}
#endif
}

/* ---- public API ---- */

int dm_check_modules(struct device_manager *mgr)
{
	mgr->has_ipts_module = is_module_loaded(IPTS_MODULE_IPTS);
	mgr->has_ithc_module = is_module_loaded(IPTS_MODULE_ITHC);

	ALOGD("Module check: ipts=%s ithc=%s",
	      mgr->has_ipts_module ? "loaded" : "not loaded",
	      mgr->has_ithc_module ? "loaded" : "not loaded");

	return (mgr->has_ipts_module || mgr->has_ithc_module);
}

int dm_init(struct device_manager *mgr)
{
	sigset_t mask;
	struct epoll_event ev;

	memset(mgr, 0, sizeof(*mgr));
	mgr->inotify_fd = -1;
	mgr->signal_fd = -1;
	mgr->timer_fd = -1;
	mgr->resume_fd = -1;
	mgr->epoll_fd = -1;

	/* Check which IPTS modules are loaded */
	dm_check_modules(mgr);

	/* Read reload_on_resume setting */
	mgr->reload_on_resume = get_reload_on_resume();
	ALOGI("Module reload on resume: %s",
	      mgr->reload_on_resume ? "enabled" : "disabled");

	/* Resolve the iptsd binary path */
	mgr->iptsd_bin[0] = '\0';
	for (const char **p = iptsd_bin_candidates; *p != NULL; p++) {
		if (access(*p, X_OK) == 0) {
			snprintf(mgr->iptsd_bin, sizeof(mgr->iptsd_bin),
				 "%s", *p);
			ALOGI("Found iptsd binary at %s", mgr->iptsd_bin);
			break;
		}
	}

	if (mgr->iptsd_bin[0] == '\0') {
		ALOGE("Could not find iptsd binary in any candidate path");
		return -ENOENT;
	}

	/* Initialize resume detection baseline */
	get_suspend_delta(&mgr->last_suspend_delta);

	/* Create inotify instance watching /dev/ for CREATE/DELETE */
	mgr->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (mgr->inotify_fd < 0) {
		ALOGE("inotify_init1 failed: %s", strerror(errno));
		return -errno;
	}

	if (inotify_add_watch(mgr->inotify_fd, "/dev/",
			      IN_CREATE | IN_DELETE) < 0) {
		ALOGE("inotify_add_watch(/dev/) failed: %s", strerror(errno));
		goto err;
	}

	/* Block SIGCHLD and create signalfd */
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		ALOGE("sigprocmask failed: %s", strerror(errno));
		goto err;
	}

	mgr->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (mgr->signal_fd < 0) {
		ALOGE("signalfd failed: %s", strerror(errno));
		goto err;
	}

	/* Create timerfd for debouncing rescans */
	mgr->timer_fd = timerfd_create(CLOCK_MONOTONIC,
				       TFD_NONBLOCK | TFD_CLOEXEC);
	if (mgr->timer_fd < 0) {
		ALOGE("timerfd_create failed: %s", strerror(errno));
		goto err;
	}

	/*
	 * Create periodic timerfd using CLOCK_BOOTTIME for resume detection.
	 *
	 * CLOCK_BOOTTIME includes time spent in suspend, while
	 * CLOCK_MONOTONIC does not. By comparing the two, we can detect
	 * when a suspend/resume cycle has occurred.
	 */
	mgr->resume_fd = timerfd_create(CLOCK_BOOTTIME,
					TFD_NONBLOCK | TFD_CLOEXEC);
	if (mgr->resume_fd < 0) {
		ALOGE("timerfd_create(CLOCK_BOOTTIME) failed: %s",
		      strerror(errno));
		goto err;
	}

	{
		struct itimerspec rts;

		memset(&rts, 0, sizeof(rts));
		rts.it_value.tv_sec = RESUME_POLL_INTERVAL_S;
		rts.it_interval.tv_sec = RESUME_POLL_INTERVAL_S;
		timerfd_settime(mgr->resume_fd, 0, &rts, NULL);
	}

	/* Create epoll and register all fds */
	mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (mgr->epoll_fd < 0) {
		ALOGE("epoll_create1 failed: %s", strerror(errno));
		goto err;
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;

	ev.data.fd = mgr->inotify_fd;
	if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->inotify_fd, &ev) < 0)
		goto err;

	ev.data.fd = mgr->signal_fd;
	if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->signal_fd, &ev) < 0)
		goto err;

	ev.data.fd = mgr->timer_fd;
	if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->timer_fd, &ev) < 0)
		goto err;

	ev.data.fd = mgr->resume_fd;
	if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->resume_fd, &ev) < 0)
		goto err;

	ALOGI("Device manager initialized (resume detection enabled)");
	return 0;

err:
	dm_destroy(mgr);
	return -errno;
}

int dm_scan_and_reconcile(struct device_manager *mgr)
{
	DIR *dir;
	struct dirent *entry;
	char path[280];
	struct ipts_device_info info;
	int active_count = 0;
	int i;

	ALOGD("Scanning for IPTS devices...");

	/* Mark devices that no longer exist as dead */
	for (i = 0; i < mgr->instance_count; i++) {
		if (!mgr->instances[i].active)
			continue;

		if (access(mgr->instances[i].device_path, F_OK) != 0) {
			ALOGI("Device %s disappeared, marking instance (pid %d) for cleanup",
			      mgr->instances[i].device_path,
			      mgr->instances[i].pid);

			/* Send SIGTERM — the instance will be reaped via SIGCHLD */
			kill(mgr->instances[i].pid, SIGTERM);
		}
	}

	/* Scan /dev/ for hidraw devices */
	dir = opendir("/dev");
	if (!dir) {
		ALOGE("opendir(/dev) failed: %s", strerror(errno));
		return -errno;
	}

	while ((entry = readdir(dir)) != NULL) {
		/* Filter for hidrawN entries */
		if (strncmp(entry->d_name, "hidraw", 6) != 0)
			continue;

		/* Verify remaining chars are digits */
		const char *p = entry->d_name + 6;
		int valid = 1;

		while (*p) {
			if (*p < '0' || *p > '9') {
				valid = 0;
				break;
			}
			p++;
		}

		if (!valid || p == entry->d_name + 6)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);

		/* Skip if already tracked and running */
		if (find_instance_by_path(mgr, path))
			continue;

		/* Probe the device */
		memset(&info, 0, sizeof(info));
		if (ipts_detect_device(path, &info) < 0) {
			ALOGD("Could not probe %s (permission denied or not readable)",
			      path);
			continue;
		}

		if (!info.is_ipts) {
			ALOGD("%s is not an IPTS device", path);
			continue;
		}

		ALOGI("Found IPTS %s at %s [%04x:%04x]",
		      type_str(info.type), path, info.vendor, info.product);

		spawn_iptsd(mgr, path, &info);
	}

	closedir(dir);

	/* Count active instances */
	for (i = 0; i < mgr->instance_count; i++) {
		if (mgr->instances[i].active)
			active_count++;
	}

	ALOGI("Scan complete: %d active instance(s)", active_count);
	return active_count;
}

int dm_reap_children(struct device_manager *mgr)
{
	int reaped = 0;
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		struct iptsd_instance *inst = find_instance_by_pid(mgr, pid);

		if (inst) {
			if (WIFEXITED(status)) {
				ALOGI("iptsd (pid %d) for %s exited with code %d",
				      pid, inst->device_path,
				      WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				ALOGW("iptsd (pid %d) for %s killed by signal %d",
				      pid, inst->device_path,
				      WTERMSIG(status));
			}

			inst->active = 0;
			inst->pid = 0;
		} else {
			ALOGW("Reaped unknown child pid %d", pid);
		}

		reaped++;
	}

	return reaped;
}

void dm_kill_all(struct device_manager *mgr)
{
	int i;
	int had_children = 0;

	for (i = 0; i < mgr->instance_count; i++) {
		if (!mgr->instances[i].active)
			continue;

		ALOGI("Killing iptsd (pid %d) for %s",
		      mgr->instances[i].pid,
		      mgr->instances[i].device_path);

		kill(mgr->instances[i].pid, SIGTERM);
		had_children = 1;
	}

	/* Wait for all children to exit */
	if (had_children) {
		int retries = 50; /* 5 seconds max */

		while (retries-- > 0) {
			pid_t pid = waitpid(-1, NULL, WNOHANG);

			if (pid <= 0) {
				usleep(100000); /* 100ms */
				continue;
			}

			struct iptsd_instance *inst =
				find_instance_by_pid(mgr, pid);

			if (inst) {
				inst->active = 0;
				inst->pid = 0;
			}
		}

		/* Force-kill any stragglers */
		for (i = 0; i < mgr->instance_count; i++) {
			if (mgr->instances[i].active) {
				ALOGW("Force-killing iptsd (pid %d) for %s",
				      mgr->instances[i].pid,
				      mgr->instances[i].device_path);
				kill(mgr->instances[i].pid, SIGKILL);
				waitpid(mgr->instances[i].pid, NULL, 0);
				mgr->instances[i].active = 0;
				mgr->instances[i].pid = 0;
			}
		}
	}

	mgr->instance_count = 0;
}

void dm_handle_resume(struct device_manager *mgr)
{
	/*
	 * Re-read the property each time so it can be toggled at runtime
	 * without restarting the service.
	 */
	mgr->reload_on_resume = get_reload_on_resume();

	if (mgr->reload_on_resume) {
		ALOGI("=== Resume detected — reloading IPTS driver module(s) ===");

		/* Step 1: Kill all running iptsd instances */
		dm_kill_all(mgr);

		/* Step 2: Request module reload via init property trigger */
		request_module_reload(mgr);

		/*
		 * Step 3: Wait for init to process the modprobe and for
		 * new hidraw devices to appear.
		 */
		ALOGI("Waiting %d seconds for driver reload...",
		      MODULE_RELOAD_DELAY_S);
		sleep(MODULE_RELOAD_DELAY_S);

		/* Step 4: Rescan for IPTS devices */
		dm_scan_and_reconcile(mgr);
	} else {
		ALOGI("=== Resume detected — module reload disabled, rescanning ===");

		/*
		 * Without module reload, just kill stale instances and rescan.
		 * This handles the normal case where the driver resumes fine
		 * but the hidraw path may have changed.
		 */
		dm_kill_all(mgr);

		/* Brief delay for devices to stabilize */
		usleep(500000); /* 500ms */

		dm_scan_and_reconcile(mgr);
	}

	/* Update baseline so we don't trigger again immediately */
	get_suspend_delta(&mgr->last_suspend_delta);
}

int dm_run(struct device_manager *mgr)
{
	struct epoll_event events[8];
	int nfds, i;

	while (!mgr->should_stop) {
		nfds = epoll_wait(mgr->epoll_fd, events, 8, -1);

		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			ALOGE("epoll_wait failed: %s", strerror(errno));
			return -errno;
		}

		for (i = 0; i < nfds; i++) {
			int fd = events[i].data.fd;

			if (fd == mgr->signal_fd) {
				/* Drain signalfd */
				struct signalfd_siginfo si;

				while (read(mgr->signal_fd, &si, sizeof(si)) > 0)
					; /* drained */

				int reaped = dm_reap_children(mgr);

				if (reaped > 0) {
					/*
					 * A child died — schedule a rescan.
					 * This handles suspend/resume: the kernel
					 * recreates hidraw devices, iptsd exits on
					 * read error, we reap + rescan.
					 */
					ALOGI("Reaped %d child(ren), scheduling rescan",
					      reaped);
					arm_rescan_timer(mgr);
				}

			} else if (fd == mgr->inotify_fd) {
				/* Drain inotify events */
				char buf[4096]
					__attribute__((aligned(__alignof__(struct inotify_event))));
				ssize_t len;
				int hidraw_event = 0;

				while ((len = read(mgr->inotify_fd, buf,
						   sizeof(buf))) > 0) {
					const struct inotify_event *ev;
					char *ptr;

					for (ptr = buf; ptr < buf + len;
					     ptr += sizeof(struct inotify_event) +
						    ev->len) {
						ev = (const struct inotify_event *)ptr;

						if (ev->len > 0 &&
						    strncmp(ev->name, "hidraw", 6) == 0)
							hidraw_event = 1;
					}
				}

				if (hidraw_event) {
					ALOGD("hidraw device change detected, scheduling rescan");
					arm_rescan_timer(mgr);
				}

			} else if (fd == mgr->timer_fd) {
				/* Drain timerfd */
				uint64_t expirations;

				if (read(mgr->timer_fd, &expirations,
					 sizeof(expirations)) > 0) {
					ALOGD("Rescan timer fired");
					dm_scan_and_reconcile(mgr);
				}

			} else if (fd == mgr->resume_fd) {
				/* Resume detection timer fired */
				uint64_t expirations;

				if (read(mgr->resume_fd, &expirations,
					 sizeof(expirations)) > 0) {
					struct timespec now_delta;

					get_suspend_delta(&now_delta);

					long delta_secs =
						now_delta.tv_sec -
						mgr->last_suspend_delta.tv_sec;

					if (delta_secs >= SUSPEND_THRESHOLD_S) {
						ALOGI("Suspend detected: "
						      "suspend time increased by %ld seconds",
						      delta_secs);
						mgr->last_suspend_delta = now_delta;
						dm_handle_resume(mgr);
					} else {
						mgr->last_suspend_delta = now_delta;
					}
				}
			}
		}
	}

	return 0;
}

void dm_destroy(struct device_manager *mgr)
{
	if (mgr->epoll_fd >= 0)
		close(mgr->epoll_fd);
	if (mgr->resume_fd >= 0)
		close(mgr->resume_fd);
	if (mgr->timer_fd >= 0)
		close(mgr->timer_fd);
	if (mgr->signal_fd >= 0)
		close(mgr->signal_fd);
	if (mgr->inotify_fd >= 0)
		close(mgr->inotify_fd);

	mgr->epoll_fd = -1;
	mgr->resume_fd = -1;
	mgr->timer_fd = -1;
	mgr->signal_fd = -1;
	mgr->inotify_fd = -1;
}
