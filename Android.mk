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

LOCAL_MODULE := iptsd
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libinih
LOCAL_HEADER_LIBRARIES := inih_headers spdlog_headers cli11 microsoft-gsl libbase_headers hidrd_headers
LOCAL_C_INCLUDES:= $(LOCAL_PATH)/src
LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_EXECUTABLE)

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
