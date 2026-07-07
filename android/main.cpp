/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * iptsd-runner — Android service for managing IPTS touch processing.
 *
 * This is the entry point for the iptsd-runner service. It:
 *   1. Sets up SIGTERM handler for clean shutdown
 *   2. Checks if any IPTS-related kernel modules are loaded
 *   3. Initializes the device manager (inotify, signalfd, timerfd, epoll)
 *   4. Performs an initial scan for IPTS devices
 *   5. Enters the main epoll loop to handle hotplug and child lifecycle
 *
 * On hardware without IPTS (no ipts/ithc modules loaded and no devices
 * found), the service exits cleanly to avoid wasting resources.
 *
 * Usage: iptsd-runner
 *
 * Intended to be started as an Android init service via .rc file.
 */

#include "device_manager.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "iptsd-runner"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define ALOGI(...) fprintf(stderr, "I: " __VA_ARGS__), fprintf(stderr, "\n")
#define ALOGE(...) fprintf(stderr, "E: " __VA_ARGS__), fprintf(stderr, "\n")
#define ALOGW(...) fprintf(stderr, "W: " __VA_ARGS__), fprintf(stderr, "\n")
#endif

/* Global device manager pointer for signal handler access */
static struct device_manager *g_mgr = NULL;

static void sigterm_handler(int sig)
{
	(void)sig;
	if (g_mgr)
		g_mgr->should_stop = 1;
}

int main(int argc, char *argv[])
{
	struct device_manager mgr;
	struct sigaction sa;
	int ret;

	(void)argc;
	(void)argv;

	ALOGI("iptsd-runner starting");

	/* Set up SIGTERM handler for clean shutdown from init */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/* Initialize the device manager */
	g_mgr = &mgr;
	ret = dm_init(&mgr);
	if (ret < 0) {
		ALOGE("Failed to initialize device manager: %s",
		      strerror(-ret));
		return EXIT_FAILURE;
	}

	/* Perform initial scan */
	ret = dm_scan_and_reconcile(&mgr);
	if (ret < 0) {
		ALOGE("Initial scan failed: %s", strerror(-ret));
		dm_destroy(&mgr);
		return EXIT_FAILURE;
	}

	/*
	 * If no IPTS modules are loaded AND no devices were found,
	 * this hardware doesn't have IPTS touch. Exit cleanly to
	 * avoid wasting memory and CPU on a non-Surface device.
	 */
	if (ret == 0 && !mgr.has_ipts_module && !mgr.has_ithc_module) {
		ALOGI("No IPTS modules loaded and no devices found — "
		      "this hardware does not appear to have IPTS touch. "
		      "Exiting.");
		dm_destroy(&mgr);
		return EXIT_SUCCESS;
	}

	if (ret == 0) {
		ALOGW("No IPTS devices found yet, but module(s) loaded. "
		      "Waiting for devices to appear...");
	} else {
		ALOGI("Initial scan found %d device(s)", ret);
	}

	ALOGI("Entering main loop");

	/* Run the main event loop (blocks until should_stop) */
	ret = dm_run(&mgr);

	/* Clean shutdown */
	ALOGI("Shutting down, killing all instances...");
	dm_kill_all(&mgr);
	dm_destroy(&mgr);

	ALOGI("iptsd-runner exited cleanly");
	return (ret < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
