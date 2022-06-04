LOCAL_PATH:= $(call my-dir)


#build iptsd, with Surface Pro 4 fixes
include $(CLEAR_VARS)
INIH_INCLUDE := $(LOCAL_PATH)/../inih
LOCAL_CFLAGS := -Wundef \
	-Wuninitialized \
	-Wno-unused-result \
	-Wmissing-include-dirs \
	-Wold-style-definition \
	-Wpointer-arith \
	-Winit-self \
	-Wstrict-prototypes \
	-Wendif-labels \
	-Wstrict-aliasing=2 \
	-Woverflow \
	-Wmissing-prototypes \
	-Wno-missing-braces \
	-Wno-missing-field-initializers \
	-Wno-unused-parameter -std=gnu99
LOCAL_C_INCLUDES:= $(INIH_INCLUDE) \
				$(LOCAL_PATH)
LOCAL_SRC_FILES := 	 src/cone.c  \
	src/config.c  \
	src/contact.c  \
	src/control.c  \
	src/data.c  \
	src/devices.c  \
	src/finger.c  \
	src/heatmap.c  \
	src/hid.c  \
	src/main.c  \
	src/reader.c  \
	src/payload.c  \
	src/singletouch.c  \
	src/stylus.c  \
	src/touch.c  \
	src/touch-processing.c  \
	src/utils.c
LOCAL_MODULE := iptsd_sp4
LOCAL_SHARED_LIBRARIES := libinih
LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_EXECUTABLE)

#build iptsd-dbg-sp4
include $(CLEAR_VARS)
LOCAL_CFLAGS := -Wundef \
	-Wuninitialized \
	-Wno-unused-result \
	-Wmissing-include-dirs \
	-Wold-style-definition \
	-Wpointer-arith \
	-Winit-self \
	-Wstrict-prototypes \
	-Wendif-labels \
	-Wstrict-aliasing=2 \
	-Woverflow \
	-Wmissing-prototypes \
	-Wno-missing-braces \
	-Wno-missing-field-initializers \
	-Wno-unused-parameter -std=gnu99
LOCAL_C_INCLUDES:= $(LOCAL_PATH)
LOCAL_SRC_FILES := debug/debug.c \
	src/control.c  \
	src/utils.c
LOCAL_MODULE := ipts-dbg-sp4
LOCAL_PROPRIETARY_MODULE := true
include $(BUILD_EXECUTABLE)
