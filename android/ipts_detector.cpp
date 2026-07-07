/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * IPTS device detector — lightweight HID descriptor parser.
 *
 * Reimplements the detection logic from iptsd's C++ code
 * (ipts::Device + ipts::Descriptor + hid::Parser) in plain C so that
 * iptsd-runner can detect IPTS devices without pulling in the full
 * iptsd dependency tree (Eigen, fmt, GSL, spdlog, …).
 *
 * Detection criteria (must satisfy ALL):
 *   1. Device has a Feature report with Usage Page 0xFF00 (vendor),
 *      Usage 0xC8 (set-mode), size == 1 byte.
 *   2. Device has at least one Input report containing both
 *      Usage Page 0x0D / Usage 0x56 (Scan Time) AND
 *      Usage Page 0x0D / Usage 0x61 (Gesture Data).
 *
 * Device type (touchscreen vs touchpad) is determined by searching
 * for Application collections with:
 *   - Usage Page 0x0D / Usage 0x04 → Touchscreen
 *   - Usage Page 0x0D / Usage 0x05 → Touchpad
 */

#include "ipts_detector.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- HID descriptor item constants ---- */

/* Item sizes (lower 2 bits of header byte) */
#define HID_ITEM_SIZE_0 0
#define HID_ITEM_SIZE_1 1
#define HID_ITEM_SIZE_2 2
#define HID_ITEM_SIZE_4 3

/* Item tags (upper 6 bits of header byte, pre-shifted >> 2) */
/* Main */
#define HID_TAG_INPUT       0x08
#define HID_TAG_OUTPUT      0x09
#define HID_TAG_FEATURE     0x0B
#define HID_TAG_COLLECTION  0x0A
#define HID_TAG_END_COLL    0x0C

/* Global */
#define HID_TAG_USAGE_PAGE    0x00
#define HID_TAG_LOGICAL_MIN   0x01
#define HID_TAG_LOGICAL_MAX   0x02
#define HID_TAG_REPORT_SIZE   0x07
#define HID_TAG_REPORT_ID     0x08
#define HID_TAG_REPORT_COUNT  0x09

/* Local */
#define HID_TAG_USAGE         0x00
#define HID_TAG_USAGE_MIN     0x01
#define HID_TAG_USAGE_MAX     0x02

/* Item types */
#define HID_TYPE_MAIN   0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL  2

/* ---- IPTS-specific usage constants ---- */
#define USAGE_PAGE_DIGITIZER  0x000D
#define USAGE_PAGE_VENDOR     0xFF00

#define USAGE_TOUCHSCREEN     0x04
#define USAGE_TOUCHPAD        0x05
#define USAGE_SCAN_TIME       0x56
#define USAGE_GESTURE_DATA    0x61
#define USAGE_SET_MODE        0xC8

/* ---- HID report types ---- */
#define REPORT_TYPE_INPUT   0
#define REPORT_TYPE_OUTPUT  1
#define REPORT_TYPE_FEATURE 2

/* ---- Parsed report tracking ---- */
#define MAX_REPORTS     64
#define MAX_USAGES_PER_REPORT 32
#define MAX_COLLECTION_DEPTH 16

struct parsed_usage {
	uint32_t usage; /* (usage_page << 16) | usage_id */
};

struct parsed_report {
	uint8_t type;       /* REPORT_TYPE_* */
	uint8_t report_id;
	int has_report_id;
	uint32_t total_bits; /* sum of (report_count * report_size) for all fields */

	struct parsed_usage usages[MAX_USAGES_PER_REPORT];
	int usage_count;
};

struct parser_state {
	/* Global state */
	uint32_t usage_page;   /* shifted left 16 already */
	uint8_t report_id;
	int has_report_id;
	uint32_t report_count;
	uint32_t report_size;

	/* Local state */
	uint32_t usage;

	/* Collection stack — track usage of each open collection */
	uint32_t collection_usage[MAX_COLLECTION_DEPTH];
	int collection_depth;

	/* Results */
	struct parsed_report reports[MAX_REPORTS];
	int report_count_total;

	/* Collection-level flags */
	int has_touchscreen_collection;
	int has_touchpad_collection;
};

static void parser_init(struct parser_state *s)
{
	memset(s, 0, sizeof(*s));
}

static struct parsed_report *find_or_create_report(struct parser_state *s,
						   uint8_t type)
{
	int i;

	for (i = 0; i < s->report_count_total; i++) {
		struct parsed_report *r = &s->reports[i];

		if (r->type == type &&
		    r->has_report_id == s->has_report_id &&
		    (!s->has_report_id || r->report_id == s->report_id))
			return r;
	}

	if (s->report_count_total >= MAX_REPORTS)
		return NULL;

	struct parsed_report *r = &s->reports[s->report_count_total++];

	memset(r, 0, sizeof(*r));
	r->type = type;
	r->report_id = s->report_id;
	r->has_report_id = s->has_report_id;

	return r;
}

static void add_field_to_report(struct parser_state *s, uint8_t report_type)
{
	struct parsed_report *r = find_or_create_report(s, report_type);

	if (!r)
		return;

	/* Track total size */
	r->total_bits += s->report_count * s->report_size;

	/* Track usage */
	if (s->usage && r->usage_count < MAX_USAGES_PER_REPORT) {
		r->usages[r->usage_count].usage = s->usage;
		r->usage_count++;
	}

	/* Reset local state */
	s->usage = 0;
}

static uint32_t make_extended_usage(struct parser_state *s, int item_size,
				    uint32_t payload)
{
	if (item_size == HID_ITEM_SIZE_4)
		return payload;

	return s->usage_page | payload;
}

static int parse_hid_descriptor(const uint8_t *desc, uint32_t desc_size,
				struct parser_state *state)
{
	uint32_t pos = 0;

	parser_init(state);

	while (pos < desc_size) {
		uint8_t header = desc[pos++];
		int item_size_code = header & 0x03;
		int item_type = (header >> 2) & 0x03;
		int item_tag_raw = (header >> 4) & 0x0F;

		/* Determine payload size */
		int payload_size = 0;

		switch (item_size_code) {
		case HID_ITEM_SIZE_0:
			payload_size = 0;
			break;
		case HID_ITEM_SIZE_1:
			payload_size = 1;
			break;
		case HID_ITEM_SIZE_2:
			payload_size = 2;
			break;
		case HID_ITEM_SIZE_4:
			payload_size = 4;
			break;
		}

		if (pos + payload_size > desc_size)
			break;

		/* Read payload */
		uint32_t payload = 0;

		switch (payload_size) {
		case 1:
			payload = desc[pos];
			break;
		case 2:
			payload = desc[pos] | ((uint32_t)desc[pos + 1] << 8);
			break;
		case 4:
			payload = desc[pos] |
				  ((uint32_t)desc[pos + 1] << 8) |
				  ((uint32_t)desc[pos + 2] << 16) |
				  ((uint32_t)desc[pos + 3] << 24);
			break;
		}

		pos += payload_size;

		/* Process by type+tag */
		switch (item_type) {
		case HID_TYPE_MAIN:
			switch (item_tag_raw) {
			case 0x08: /* Input (tag 0x08 in main) */
				add_field_to_report(state, REPORT_TYPE_INPUT);
				break;
			case 0x09: /* Output */
				add_field_to_report(state, REPORT_TYPE_OUTPUT);
				break;
			case 0x0B: /* Feature */
				add_field_to_report(state, REPORT_TYPE_FEATURE);
				break;
			case 0x0A: /* Collection */
				if (state->collection_depth < MAX_COLLECTION_DEPTH) {
					state->collection_usage[state->collection_depth] =
						state->usage;
					state->collection_depth++;
				}

				/*
				 * Check for Application collections (type 0x01)
				 * with digitizer usages.
				 */
				if (payload == 0x01) {
					uint32_t u = state->usage;
					uint32_t ts_usage =
						((uint32_t)USAGE_PAGE_DIGITIZER << 16) |
						USAGE_TOUCHSCREEN;
					uint32_t tp_usage =
						((uint32_t)USAGE_PAGE_DIGITIZER << 16) |
						USAGE_TOUCHPAD;

					if (u == ts_usage)
						state->has_touchscreen_collection = 1;
					if (u == tp_usage)
						state->has_touchpad_collection = 1;
				}

				/* Reset local state */
				state->usage = 0;
				break;
			case 0x0C: /* End Collection */
				if (state->collection_depth > 0)
					state->collection_depth--;
				break;
			}
			break;

		case HID_TYPE_GLOBAL:
			switch (item_tag_raw) {
			case 0x00: /* Usage Page */
				state->usage_page = payload << 16;
				break;
			case 0x07: /* Report Size */
				state->report_size = payload;
				break;
			case 0x08: /* Report ID */
				state->report_id = (uint8_t)payload;
				state->has_report_id = 1;
				break;
			case 0x09: /* Report Count */
				state->report_count = payload;
				break;
			}
			break;

		case HID_TYPE_LOCAL:
			switch (item_tag_raw) {
			case 0x00: /* Usage */
				state->usage = make_extended_usage(
					state, item_size_code, payload);
				break;
			}
			break;
		}
	}

	return 0;
}

static int report_has_usage(const struct parsed_report *r, uint32_t usage)
{
	int i;

	for (i = 0; i < r->usage_count; i++) {
		if (r->usages[i].usage == usage)
			return 1;
	}

	return 0;
}

/**
 * Check if any report matches the modesetting feature report criteria:
 *   - Type: Feature
 *   - Usage: USAGE_PAGE_VENDOR (0xFF00) | USAGE_SET_MODE (0xC8)
 *   - Size: 1 byte (8 bits)
 */
static int has_modesetting_report(const struct parser_state *state)
{
	uint32_t set_mode_usage =
		((uint32_t)USAGE_PAGE_VENDOR << 16) | USAGE_SET_MODE;
	int i;

	for (i = 0; i < state->report_count_total; i++) {
		const struct parsed_report *r = &state->reports[i];

		if (r->type != REPORT_TYPE_FEATURE)
			continue;

		/* Check size: total_bits / 8 == 1 byte */
		if (r->total_bits != 8)
			continue;

		if (report_has_usage(r, set_mode_usage))
			return 1;
	}

	return 0;
}

/**
 * Check if any report matches the touch data input report criteria:
 *   - Type: Input
 *   - Has Usage: USAGE_PAGE_DIGITIZER | USAGE_SCAN_TIME
 *   - Has Usage: USAGE_PAGE_DIGITIZER | USAGE_GESTURE_DATA
 */
static int has_touch_data_report(const struct parser_state *state)
{
	uint32_t scan_time_usage =
		((uint32_t)USAGE_PAGE_DIGITIZER << 16) | USAGE_SCAN_TIME;
	uint32_t gesture_data_usage =
		((uint32_t)USAGE_PAGE_DIGITIZER << 16) | USAGE_GESTURE_DATA;
	int i;

	for (i = 0; i < state->report_count_total; i++) {
		const struct parsed_report *r = &state->reports[i];

		if (r->type != REPORT_TYPE_INPUT)
			continue;

		if (report_has_usage(r, scan_time_usage) &&
		    report_has_usage(r, gesture_data_usage))
			return 1;
	}

	return 0;
}

int ipts_detect_device(const char *path, struct ipts_device_info *info)
{
	struct hidraw_devinfo devinfo;
	struct hidraw_report_descriptor rdesc;
	uint32_t desc_size = 0;
	struct parser_state state;
	int fd, ret;

	memset(info, 0, sizeof(*info));

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -errno;

	/* Get device info (vendor, product) */
	ret = ioctl(fd, HIDIOCGRAWINFO, &devinfo);
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}

	/* Get descriptor size */
	ret = ioctl(fd, HIDIOCGRDESCSIZE, &desc_size);
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}

	if (desc_size == 0 || desc_size > HID_MAX_DESCRIPTOR_SIZE) {
		ret = -EINVAL;
		goto out_close;
	}

	/* Get the full HID report descriptor */
	memset(&rdesc, 0, sizeof(rdesc));
	rdesc.size = desc_size;
	ret = ioctl(fd, HIDIOCGRDESC, &rdesc);
	if (ret < 0) {
		ret = -errno;
		goto out_close;
	}

	close(fd);
	fd = -1;

	/* Parse the HID descriptor */
	ret = parse_hid_descriptor(rdesc.value, desc_size, &state);
	if (ret < 0)
		return ret;

	/* Fill basic info regardless of IPTS status */
	info->vendor = (uint16_t)devinfo.vendor;
	info->product = (uint16_t)devinfo.product;

	/* Check IPTS criteria */
	if (!has_modesetting_report(&state) || !has_touch_data_report(&state)) {
		info->is_ipts = 0;
		return 0;
	}

	info->is_ipts = 1;

	/* Determine device type */
	if (state.has_touchpad_collection && !state.has_touchscreen_collection)
		info->type = IPTS_DEVICE_TYPE_TOUCHPAD;
	else if (state.has_touchscreen_collection && !state.has_touchpad_collection)
		info->type = IPTS_DEVICE_TYPE_TOUCHSCREEN;
	else
		info->type = IPTS_DEVICE_TYPE_UNKNOWN;

	return 0;

out_close:
	close(fd);
	return ret;
}
