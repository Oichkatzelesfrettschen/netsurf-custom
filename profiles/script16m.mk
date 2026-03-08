override COMMON_WARNFLAGS += -Werror
override NETSURF_STRIP_BINARY := YES
override NETSURF_LOG_LEVEL := WARNING

override NETSURF_USE_JPEG := YES
override NETSURF_USE_PNG := YES
override NETSURF_USE_BMP := YES
override NETSURF_USE_GIF := YES
override NETSURF_USE_NSSVG := YES

override NETSURF_USE_WEBP := NO
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_RSVG := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_NSPSL := NO
override NETSURF_USE_NSLOG := NO
override NETSURF_USE_UTF8PROC := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_DUKTAPE := YES
override NETSURF_JS_ENGINE := standard

CFLAGS += -Os -fdata-sections -ffunction-sections
CXXFLAGS += -Os -fdata-sections -ffunction-sections
LDFLAGS += -Wl,--gc-sections
