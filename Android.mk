LOCAL_PATH:= $(call my-dir)

#build iptsd
include $(CLEAR_VARS)
LOCAL_CPPFLAGS := \
	-Wall -Winvalid-pch \
	-Wnon-virtual-dtor -Wextra \
	-Wpedantic -Werror -std=c++17 \
	-O2 -g -Wuninitialized -Wno-unused-result \
	-Wmissing-include-dirs -Wpointer-arith \
	-Winit-self -Wimplicit-fallthrough -Wendif-labels \
	-Wstrict-aliasing=2 -Woverflow -Wno-missing-braces \
	-Wno-missing-field-initializers -Wno-unused-parameter -fexceptions

LOCAL_SRC_FILES := $(call all-cpp-files-under, src/config)
LOCAL_SRC_FILES += $(call all-cpp-files-under, src/contacts)
LOCAL_SRC_FILES += $(call all-cpp-files-under, src/daemon)
LOCAL_SRC_FILES += $(call all-cpp-files-under, src/hid)
LOCAL_SRC_FILES += $(call all-c-files-under, src/hid/shim)
LOCAL_SRC_FILES += $(call all-cpp-files-under, src/ipts)

LOCAL_MODULE := iptsd
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libinih libc++fs
LOCAL_STATIC_LIBRARIES := libhidrd_usage libhidrd_item
LOCAL_HEADER_LIBRARIES := inih_headers spdlog_headers cli11 microsoft-gsl libbase_headers hidrd_headers
LOCAL_C_INCLUDES:= $(LOCAL_PATH)/src
LOCAL_POST_INSTALL_CMD := (hide) mkdir -p $(TARGET_OUT_VENDOR)/etc/ipts; \
						  rsync -av -l $(LOCAL_PATH)/etc/config/ipts.conf $(TARGET_OUT_VENDOR)/etc/ipts; \
						  rsync -av -l $(LOCAL_PATH)/etc/config/* $(TARGET_OUT_VENDOR)/etc/ipts
LOCAL_POST_INSTALL_CMD := ln -sf /vendor/bin/iptsd $(TARGET_OUT)/bin/iptsd
LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_EXECUTABLE)

#copy iptsd-find-hidraw
include $(CLEAR_VARS)
LOCAL_MODULE := iptsd-find-hidraw
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_PROPRIETARY_MODULE := true
LOCAL_SRC_FILES := etc/iptsd-find-hidraw
LOCAL_POST_INSTALL_CMD := ln -sf /vendor/bin/iptsd-find-hidraw $(TARGET_OUT)/bin/iptsd-find-hidraw

include $(BUILD_PREBUILT)

#build iptsd-dbg
#include $(CLEAR_VARS)
#LOCAL_CFLAGS := -Wundef \
#	-Wuninitialized \
#	-Wno-unused-result \
#	-Wmissing-include-dirs \
#	-Wold-style-definition \
#	-Wpointer-arith \
#	-Winit-self \
#	-Wstrict-prototypes \
#	-Wendif-labels \
#	-Wstrict-aliasing=2 \
#	-Woverflow \
#	-Wmissing-prototypes \
#	-Wno-missing-braces \
#	-Wno-missing-field-initializers \
#	-Wno-unused-parameter -std=gnu99
#LOCAL_C_INCLUDES:= $(LOCAL_PATH)
#LOCAL_SRC_FILES := debug/debug.c \
#	src/control.c  \
#	src/utils.c
#LOCAL_MODULE := ipts-dbg
#LOCAL_PROPRIETARY_MODULE := true
#include $(BUILD_EXECUTABLE)
