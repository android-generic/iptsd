/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef IPTSD_ANDROID_IPTS_DETECTOR_H
#define IPTSD_ANDROID_IPTS_DETECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Type of IPTS device detected.
 */
enum ipts_device_type {
	IPTS_DEVICE_TYPE_UNKNOWN = 0,
	IPTS_DEVICE_TYPE_TOUCHSCREEN = 1,
	IPTS_DEVICE_TYPE_TOUCHPAD = 2,
};

/**
 * Result of probing a hidraw device for IPTS compatibility.
 */
struct ipts_device_info {
	int is_ipts;                    /* Non-zero if this is an IPTS device */
	enum ipts_device_type type;     /* Touchscreen or touchpad */
	uint16_t vendor;                /* HID vendor ID */
	uint16_t product;               /* HID product ID */
};

/**
 * Probe a hidraw device to determine if it is an IPTS touch device.
 *
 * Opens the device, reads the HID descriptor via ioctl, parses it to look for
 * IPTS-specific reports (modesetting feature report and touch data input reports),
 * and determines the device type (touchscreen vs touchpad).
 *
 * @param path  Path to the hidraw device node (e.g. "/dev/hidraw0")
 * @param info  Output structure filled with detection results
 * @return 0 on success (info is filled), negative errno on error
 *         (device could not be opened/queried — info is zeroed)
 */
int ipts_detect_device(const char *path, struct ipts_device_info *info);

#ifdef __cplusplus
}
#endif

#endif /* IPTSD_ANDROID_IPTS_DETECTOR_H */
