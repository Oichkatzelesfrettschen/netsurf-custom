#
# Makefile for NetSurf
#
# Copyright 2007 Daniel Silverstone <dsilvers@netsurf-browser.org>
# Copyright 2008 Rob Kendrick <rjek@netsurf-browser.org>
# Copyright 2024 Vincent Sanders <vince@netsurf-browser.org>
#
# Trivially, invoke as:
#   make
# to build native, or:
#   make TARGET=riscos
# to cross-build for RO.
#
# Look at Makefile.config for configuration options.
#
# Best results obtained building on unix platforms cross compiling for others
#
# To clean, invoke as above, with the 'clean' target
#
# To build developer Doxygen generated documentation, invoke as above,
# with the 'docs' target:
#   make docs
#

.PHONY: all

all: all-program

# default values for base variables

# Resources executable target depends upon
RESOURCES=
# Messages executable target depends on
MESSAGES:=

# The filter applied to the fat (full) messages to generate split messages
MESSAGES_FILTER=any
# The languages in the fat messages to convert
MESSAGES_LANGUAGES=de en fr it nl zh_CN
# The target directory for the split messages
MESSAGES_TARGET=resources

# build verbosity
ifeq ($(V),1)
  Q:=
else
  Q=@
endif
VQ=@

# compute HOST, TARGET and SUBTARGET
include frontends/Makefile.hts

# tools used in builds
include Makefile.tools

# Target paths
OBJROOT = build/$(HOST)-$(TARGET)$(SUBTARGET)
DEPROOT := $(OBJROOT)/deps
TOOLROOT := $(OBJROOT)/tools

# keep C flags from environment
CFLAGS_ENV := $(CFLAGS)
CXXFLAGS_ENV := $(CXXFLAGS)

# library and feature building macros
include Makefile.macros

# ----------------------------------------------------------------------------
# General flag setup
# ----------------------------------------------------------------------------

# host compiler flags
BUILD_CFLAGS = -g -W -Wall -Wundef -Wpointer-arith -Wcast-align \
	-Wwrite-strings -Wmissing-declarations -Wuninitialized \
	-Wno-unused-parameter

# Set up the warning flags here so that they can be overridden in the
#   Makefile.config
COMMON_WARNFLAGS = -W -Wall -Wundef -Wpointer-arith -Wcast-align \
	-Wwrite-strings -Wmissing-declarations -Wuninitialized

ifneq ($(CC_MAJOR),2)
  COMMON_WARNFLAGS += -Wno-unused-parameter
endif

# deal with lots of unwanted warnings from javascript
ifeq ($(call cc_ver_ge,4,6),1)
  COMMON_WARNFLAGS += -Wno-unused-but-set-variable
endif

ifeq ($(TOOLCHAIN),gcc)
  # Implicit fallthrough warnings
  ifeq ($(call cc_ver_ge,7,1),1)
    COMMON_WARNFLAGS += -Wimplicit-fallthrough=5
  endif
else
  # non gcc has different warning syntax
  COMMON_WARNFLAGS += -Wimplicit-fallthrough
endif

# deal with changing warning flags for different platforms
ifeq ($(HOST),openbsd)
  # OpenBSD headers are not compatible with redundant declaration warning
  COMMON_WARNFLAGS += -Wno-redundant-decls
else
  COMMON_WARNFLAGS += -Wredundant-decls
endif

# c++ default warning flags
CXXWARNFLAGS :=

# C default warning flags
CWARNFLAGS := -Wstrict-prototypes -Wmissing-prototypes -Wnested-externs

# Pull in the default configuration
include Makefile.defaults

# Pull in the user configuration
-include Makefile.config

PROFILE ?= hostdev
KNOWN_PROFILES := hostdev core4m script16m
ifeq ($(filter $(PROFILE),$(KNOWN_PROFILES)),)
$(error Unknown PROFILE '$(PROFILE)'. Expected one of: $(KNOWN_PROFILES))
endif
PROFILE_MAKEFILE := profiles/$(PROFILE).mk
-include $(PROFILE_MAKEFILE)

# Keep the workspace pkg-config path visible to $(shell pkg-config ...)
# during parse-time feature detection, not just to recipe shells.
export PKG_CONFIG_PATH

MINIMAL_PARSE_GOALS := \
	doctor doctor-i386 bootstrap-tools bootstrap-libs bootstrap \
	build-native package-native build-gtk build-monkey build-monkey-enhanced build-framebuffer build-matrix-native \
	build-profile build-monkey-profile \
	test-unit sanitize-unit test-monkey-smoke test-monkey-division benchmark-monkey benchmark-monkey-enhanced \
	benchmark-profile profile-profile verify-profile \
	measure-monkey-engines measure-bootstrap-costs measure-i386-denominator \
	rebuild-i386-libs build-i386-denominator verify-i386-denominator \
	profile-valgrind-monkey profile-heaptrack-monkey profile-perf-monkey \
	static-analysis verify-native verify-matrix
SKIP_BUILD_CONFIG := $(filter $(MINIMAL_PARSE_GOALS),$(MAKECMDGOALS))

ifeq ($(SKIP_BUILD_CONFIG),)
# libraries enabled by feature switch without pkgconfig file 
$(eval $(call feature_switch,JPEG,JPEG (libjpeg),-DWITH_JPEG,-ljpeg,-UWITH_JPEG,))
$(eval $(call feature_switch,HARU_PDF,PDF export (haru),-DWITH_PDF_EXPORT,-lhpdf -lpng,-UWITH_PDF_EXPORT,))
$(eval $(call feature_switch,LIBICONV_PLUG,glibc internal iconv,-DLIBICONV_PLUG,,-ULIBICONV_PLUG,-liconv))
$(eval $(call feature_switch,DUKTAPE,Javascript (Duktape),,,,,))

# Common libraries with pkgconfig
$(eval $(call pkg_config_find_and_add,libcss,CSS))
$(eval $(call pkg_config_find_and_add,libdom,DOM))
$(eval $(call pkg_config_find_and_add,libnsutils,nsutils))

# Common libraries without pkg-config support
LDFLAGS += -lz

# Optional libraries with pkgconfig

# define additional CFLAGS and LDFLAGS requirements for pkg-configed libs
# We only need to define the ones where the feature name doesn't exactly
# match the WITH_FEATURE flag
NETSURF_FEATURE_NSSVG_CFLAGS := -DWITH_NS_SVG
NETSURF_FEATURE_ROSPRITE_CFLAGS := -DWITH_NSSPRITE

# libcurl and openssl ordering matters as if libcurl requires ssl it
#  needs to come first in link order to ensure its symbols can be
#  resolved by the subsequent openssl

# freemint does not support pkg-config for libcurl
ifeq ($(HOST),mint)
    CFLAGS += $(shell curl-config --cflags)
    LDFLAGS += $(shell curl-config --libs)
else
    $(eval $(call pkg_config_find_and_add_enabled,CURL,libcurl,Curl))
endif
$(eval $(call pkg_config_find_and_add_enabled,OPENSSL,openssl,OpenSSL))

$(eval $(call pkg_config_find_and_add_enabled,UTF8PROC,libutf8proc,utf8))
$(eval $(call pkg_config_find_and_add_enabled,JPEGXL,libjxl,JPEGXL))
$(eval $(call pkg_config_find_and_add_enabled,WEBP,libwebp,WEBP))
$(eval $(call pkg_config_find_and_add_enabled,PNG,libpng,PNG))
$(eval $(call pkg_config_find_and_add_enabled,BMP,libnsbmp,BMP))
$(eval $(call pkg_config_find_and_add_enabled,GIF,libnsgif,GIF))
$(eval $(call pkg_config_find_and_add_enabled,NSSVG,libsvgtiny,SVG))
$(eval $(call pkg_config_find_and_add_enabled,ROSPRITE,librosprite,Sprite))
$(eval $(call pkg_config_find_and_add_enabled,NSPSL,libnspsl,PSL))
$(eval $(call pkg_config_find_and_add_enabled,NSLOG,libnslog,LOG))
endif

# List of directories in which headers are searched for
INCLUDE_DIRS :=. include $(OBJROOT)

# export the user agent format
CFLAGS += -DNETSURF_UA_FORMAT_STRING=\"$(NETSURF_UA_FORMAT_STRING)\"
CXXFLAGS += -DNETSURF_UA_FORMAT_STRING=\"$(NETSURF_UA_FORMAT_STRING)\"

# set the default homepage to use
CFLAGS += -DNETSURF_HOMEPAGE=\"$(NETSURF_HOMEPAGE)\"
CXXFLAGS += -DNETSURF_HOMEPAGE=\"$(NETSURF_HOMEPAGE)\"

# set the logging level
CFLAGS += -DNETSURF_LOG_LEVEL=$(NETSURF_LOG_LEVEL)
CXXFLAGS += -DNETSURF_LOG_LEVEL=$(NETSURF_LOG_LEVEL)

ifneq ($(PREFIX),)
CFLAGS += -I$(PREFIX)/include
CXXFLAGS += -I$(PREFIX)/include
LDFLAGS += -L$(PREFIX)/lib
endif

# If we're building the sanitize goal, override things
ifneq ($(filter-out sanitize,$(MAKECMDGOALS)),$(MAKECMDGOALS))
override NETSURF_USE_SANITIZER := YES
override NETSURF_RECOVER_SANITIZERS := NO
endif

# If we're going to use the sanitizer set it up
ifeq ($(NETSURF_USE_SANITIZER),YES)
SAN_FLAGS := -fsanitize=address -fsanitize=undefined
ifeq ($(NETSURF_RECOVER_SANITIZERS),NO)
SAN_FLAGS += -fno-sanitize-recover
endif
else
SAN_FLAGS :=
endif
CFLAGS += $(SAN_FLAGS)
CXXFLAGS += $(SAN_FLAGS)
LDFLAGS += $(SAN_FLAGS)

# and the logging filter
CFLAGS += -DNETSURF_BUILTIN_LOG_FILTER=\"$(NETSURF_BUILTIN_LOG_FILTER)\"
CXXFLAGS += -DNETSURF_BUILTIN_LOG_FILTER=\"$(NETSURF_BUILTIN_LOG_FILTER)\"
# and the verbose logging filter
CFLAGS += -DNETSURF_BUILTIN_VERBOSE_FILTER=\"$(NETSURF_BUILTIN_VERBOSE_FILTER)\"
CXXFLAGS += -DNETSURF_BUILTIN_VERBOSE_FILTER=\"$(NETSURF_BUILTIN_VERBOSE_FILTER)\"

# Determine if the C compiler supports statement expressions
# This is needed to permit certain optimisations in our library headers
ifneq ($(shell $(CC) -dM -E - < /dev/null | grep __GNUC__),)
CFLAGS += -DSTMTEXPR=1
CXXFLAGS += -DSTMTEXPR=1
endif

# We trace during link so that we can determine if a libary changes under us in
# order to re-link.  This *may* be gcc specific, so may need tweaks in future.
LDFLAGS += -Wl,--trace

# ----------------------------------------------------------------------------
# General make rules
# ----------------------------------------------------------------------------

$(OBJROOT)/created:
	$(VQ)echo "   MKDIR: $(OBJROOT)"
	$(Q)$(MKDIR) -p $(OBJROOT)
	$(Q)$(TOUCH) $(OBJROOT)/created

$(DEPROOT)/created: $(OBJROOT)/created
	$(VQ)echo "   MKDIR: $(DEPROOT)"
	$(Q)$(MKDIR) -p $(DEPROOT)
	$(Q)$(TOUCH) $(DEPROOT)/created

$(TOOLROOT)/created: $(OBJROOT)/created
	$(VQ)echo "   MKDIR: $(TOOLROOT)"
	$(Q)$(MKDIR) -p $(TOOLROOT)
	$(Q)$(TOUCH) $(TOOLROOT)/created

CLEANS :=
POSTEXES :=

# ----------------------------------------------------------------------------
# Target specific setup
# ----------------------------------------------------------------------------

ifeq ($(SKIP_BUILD_CONFIG),)
include frontends/Makefile

# ----------------------------------------------------------------------------
# Build tools setup
# ----------------------------------------------------------------------------

include tools/Makefile

# ----------------------------------------------------------------------------
# General source file setup
# ----------------------------------------------------------------------------

# Content sources
include content/Makefile

# utility sources
include utils/Makefile

# http utility sources
include utils/http/Makefile

# nsurl utility sources
include utils/nsurl/Makefile

# Desktop sources
include desktop/Makefile

# S_COMMON are sources common to all builds
S_COMMON := \
	$(S_CONTENT) \
	$(S_FETCHERS) \
	$(S_UTILS) \
	$(S_HTTP) \
	$(S_NSURL) \
	$(S_DESKTOP) \
	$(S_JAVASCRIPT_BINDING)


# ----------------------------------------------------------------------------
# Message targets
# ----------------------------------------------------------------------------


# generate the message file rules
$(eval $(foreach LANG,$(MESSAGES_LANGUAGES), \
	$(call split_messages,$(LANG))))

clean-messages:
	$(VQ)echo "   CLEAN: $(CLEAN_MESSAGES)"
	$(Q)$(RM) $(CLEAN_MESSAGES)
CLEANS += clean-messages


# ----------------------------------------------------------------------------
# Source file setup
# ----------------------------------------------------------------------------

# Force exapnsion of source file list
SOURCES := $(SOURCES)

ifeq ($(SOURCES),)
$(error Unable to build NetSurf, could not determine set of sources to build)
endif

OBJECTS := $(sort $(addprefix $(OBJROOT)/,$(subst /,_,$(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(patsubst %.m,%.o,$(patsubst %.s,%.o,$(SOURCES))))))))

# Include directory flags
IFLAGS = $(addprefix -I,$(INCLUDE_DIRS))

$(EXETARGET): $(OBJECTS) $(RESOURCES) $(MESSAGES) tools/linktrace-to-depfile.pl
	$(VQ)echo "    LINK: $(EXETARGET)"
ifneq ($(TARGET),riscos)
	$(Q)$(CC) -o $(EXETARGET) $(OBJECTS) $(LDFLAGS) > $(DEPROOT)/link-raw.d
else
	@# RISC OS targets are a bit special: we need to convert ELF -> AIF
  ifeq ($(SUBTARGET),-aof)
	$(Q)$(CC) -o $(EXETARGET) $(OBJECTS) $(LDFLAGS) > $(DEPROOT)/link-raw.d
  else
	$(Q)$(CXX) -o $(EXETARGET:,ff8=,e1f) $(OBJECTS) $(LDFLAGS) > $(DEPROOT)/link-raw.d
	$(Q)$(ELF2AIF) $(EXETARGET:,ff8=,e1f) $(EXETARGET)
	$(Q)$(RM) $(EXETARGET:,ff8=,e1f)
  endif
endif
	$(VQ)echo "LINKDEPS: $(EXETARGET)"
	$(Q)echo -n "$(EXETARGET) $(DEPROOT)/link.d: " > $(DEPROOT)/link.d
	$(Q)$(PERL) tools/linktrace-to-depfile.pl < $(DEPROOT)/link-raw.d >> $(DEPROOT)/link.d
ifeq ($(NETSURF_STRIP_BINARY),YES)
	$(VQ)echo "   STRIP: $(EXETARGET)"
	$(Q)$(STRIP) $(EXETARGET)
endif
ifeq ($(TARGET),beos)
	$(VQ)echo "    XRES: $(EXETARGET)"
	$(Q)$(BEOS_XRES) -o $(EXETARGET) $(RSRC_BEOS)
	$(VQ)echo "  SETVER: $(EXETARGET)"
	$(Q)$(BEOS_SETVER) $(EXETARGET) \
                -app $(VERSION_MAJ) $(VERSION_MIN) 0 d 0 \
                -short "NetSurf $(VERSION_FULL)" \
                -long "NetSurf $(VERSION_FULL) © 2003 - 2021 The NetSurf Developers"
	$(VQ)echo " MIMESET: $(EXETARGET)"
	$(Q)$(BEOS_MIMESET) $(EXETARGET)
endif

clean-target:
	$(VQ)echo "   CLEAN: $(EXETARGET)"
	$(Q)$(RM) $(EXETARGET)
CLEANS += clean-target


clean-builddir:
	$(VQ)echo "   CLEAN: $(OBJROOT)"
	$(Q)$(RM) -r $(OBJROOT)
CLEANS += clean-builddir


.PHONY: all-program

all-program: $(EXETARGET) $(POSTEXES)

.SUFFIXES:

DEPFILES :=

# Rules to construct dep lines for each object...
$(eval $(foreach SOURCE,$(filter %.c,$(SOURCES)), \
	$(call dependency_generate_c,$(SOURCE),$(subst /,_,$(SOURCE:.c=.d)),$(subst /,_,$(SOURCE:.c=.o)))))

$(eval $(foreach SOURCE,$(filter %.cpp,$(SOURCES)), \
	$(call dependency_generate_c,$(SOURCE),$(subst /,_,$(SOURCE:.cpp=.d)),$(subst /,_,$(SOURCE:.cpp=.o)))))

$(eval $(foreach SOURCE,$(filter %.m,$(SOURCES)), \
	$(call dependency_generate_c,$(SOURCE),$(subst /,_,$(SOURCE:.m=.d)),$(subst /,_,$(SOURCE:.m=.o)))))

# Cannot currently generate dep files for S files because they're objasm
# when we move to gas format, we will be able to.

#$(eval $(foreach SOURCE,$(filter %.s,$(SOURCES)), \
#	$(call dependency_generate_s,$(SOURCE),$(subst /,_,$(SOURCE:.s=.d)),$(subst /,_,$(SOURCE:.s=.o)))))

ifeq ($(filter $(MAKECMDGOALS),clean test coverage),)
-include $(sort $(addprefix $(DEPROOT)/,$(DEPFILES)))
-include $(DEPROOT)/link.d
endif

# And rules to build the objects themselves...

$(eval $(foreach SOURCE,$(filter %.c,$(SOURCES)), \
	$(call compile_target_c,$(SOURCE),$(subst /,_,$(SOURCE:.c=.o)),$(subst /,_,$(SOURCE:.c=.d)))))

$(eval $(foreach SOURCE,$(filter %.cpp,$(SOURCES)), \
	$(call compile_target_cpp,$(SOURCE),$(subst /,_,$(SOURCE:.cpp=.o)),$(subst /,_,$(SOURCE:.cpp=.d)))))

$(eval $(foreach SOURCE,$(filter %.m,$(SOURCES)), \
	$(call compile_target_c,$(SOURCE),$(subst /,_,$(SOURCE:.m=.o)),$(subst /,_,$(SOURCE:.m=.d)))))

$(eval $(foreach SOURCE,$(filter %.s,$(SOURCES)), \
	$(call compile_target_s,$(SOURCE),$(subst /,_,$(SOURCE:.s=.o)),$(subst /,_,$(SOURCE:.s=.d)))))

# ----------------------------------------------------------------------------
# Test setup
# ----------------------------------------------------------------------------

include test/Makefile


# ----------------------------------------------------------------------------
# Clean setup
# ----------------------------------------------------------------------------

.PHONY: clean

clean: $(CLEANS)


# ----------------------------------------------------------------------------
# build distribution package
# ----------------------------------------------------------------------------

.PHONY: package-$(TARGET) package

package: all-program package-$(TARGET)


# ----------------------------------------------------------------------------
# local install on the host system
# ----------------------------------------------------------------------------

.PHONY: install install-$(TARGET)

install: all-program install-$(TARGET)


# ----------------------------------------------------------------------------
# Documentation build
# ----------------------------------------------------------------------------

.PHONY: docs

docs: docs/Doxyfile
	doxygen $<


# ----------------------------------------------------------------------------
# Transifex message processing
# ----------------------------------------------------------------------------

.PHONY: messages-split-tfx messages-fetch-tfx messages-import-tfx

# split fat messages into properties files suitable for uploading to transifex
messages-split-tfx:
	for splitlang in $(FAT_LANGUAGES);do $(PERL) ./utils/split-messages.pl -l $${splitlang} -f transifex -p any -o Messages.any.$${splitlang}.tfx resources/FatMessages;done

# download property files from transifex
messages-fetch-tfx:
	for splitlang in $(FAT_LANGUAGES);do $(RM) Messages.any.$${splitlang}.tfx ; $(PERL) ./utils/fetch-transifex.pl -w insecure -l $${splitlang} -o Messages.any.$${splitlang}.tfx ;done

# merge property files into fat messages
messages-import-tfx: messages-fetch-tfx
	for tfxlang in $(FAT_LANGUAGES);do $(PERL) ./utils/import-messages.pl -l $${tfxlang} -p any -f transifex -o resources/FatMessages -i resources/FatMessages -I Messages.any.$${tfxlang}.tfx ; $(RM) Messages.any.$${tfxlang}.tfx; done

endif

# ----------------------------------------------------------------------------
# compile_commands.json (for clangd, clang-tidy, scan-build)
# WHY: A compilation database lets editors and static-analysis tools find
#      all include paths and flags without manual -I configuration.
#      bear wraps the build and records every compilation command.
# WHAT: Produces compile_commands.json in the repository root.
# HOW:  Requires bear >= 3.0 (installed as 'bear' on Arch/Ubuntu).
#       The repo-root wrapper will source docs/env.sh automatically.
# ----------------------------------------------------------------------------
.PHONY: compile-db
ENV_HELPER := tools/with-netsurf-env.sh
DOCTOR_TOOL := tools/build-doctor.py
PERF_BASELINE_TOOL := test/perf-baseline.py
PROFILE_SUITE_TOOL := test/run-profile-suite.py
MEASURE_WORKFLOW_TOOL := tools/measure-workflow.py
I386_GATE_TOOL := tools/i386_emulator_gate.py
I386_BUILD_TOOL := tools/i386_build_attempt.py
I386_REBUILD_TOOL := tools/rebuild_i386_workspace.py
PROFILE_DIR := build/profiles
PERF_BASELINE_DIR := test/perf-baselines
BOOTSTRAP_BRANCH_ARG := $(if $(NS_BRANCH),-b $(NS_BRANCH),)
WORKSPACE_MARKER := build/.workspace-path
JS_ENGINE_MARKER := build/.js-engine-mode
PROFILE_MARKER := build/.build-profile
BUILD_FLAVOUR_MARKER := build/.build-flavour
PROFILE_CHOICES := $(CURDIR)/profiles/Choices.$(PROFILE)
PROFILE_SUITE_CHOICES := $(CURDIR)/profiles/Choices.$(PROFILE).suite
PROFILE_SUITE := $(CURDIR)/test/profile-suites/$(PROFILE).txt
BUILD_FLAVOUR ?= native

define PROFILE_RUNTIME_ENV
profile_choices="$(PROFILE_CHOICES)"; \
if [ ! -f "$$profile_choices" ]; then \
	echo "Missing profile Choices file: $$profile_choices"; \
	exit 1; \
fi; \
export NETSURF_CHOICES="$$profile_choices"; \
export NETSURF_BUILD_PROFILE="$(PROFILE)"
endef

define PROFILE_SUITE_RUNTIME_ENV
profile_choices="$(PROFILE_CHOICES)"; \
suite_choices="$(PROFILE_SUITE_CHOICES)"; \
if [ -f "$$suite_choices" ]; then \
	profile_choices="$$suite_choices"; \
fi; \
if [ ! -f "$$profile_choices" ]; then \
	echo "Missing profile Choices file: $$profile_choices"; \
	exit 1; \
fi; \
export NETSURF_CHOICES="$$profile_choices"; \
export NETSURF_BUILD_PROFILE="$(PROFILE)"
endef

define PREPARE_WORKSPACE_BUILD
marker="$(WORKSPACE_MARKER)"; \
mode_marker="$(JS_ENGINE_MARKER)"; \
profile_marker="$(PROFILE_MARKER)"; \
flavour_marker="$(BUILD_FLAVOUR_MARKER)"; \
current="$$TARGET_WORKSPACE"; \
current_mode="$${NETSURF_JS_ENGINE:-standard}"; \
current_profile="$(PROFILE)"; \
current_flavour="$(BUILD_FLAVOUR)"; \
clean_args=""; \
if [ -n "$$build_target" ]; then \
	clean_args=" TARGET=$$build_target"; \
fi; \
if [ -d build ] && [ ! -f "$$marker" ]; then \
	echo "Cleaning stale build/ because no workspace marker is present"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -f "$$marker" ] && [ "$$(cat "$$marker")" != "$$current" ]; then \
	echo "Cleaning stale build/ for workspace $$current"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -d build ] && [ ! -f "$$mode_marker" ]; then \
	echo "Cleaning stale build/ because no JS engine marker is present"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -f "$$mode_marker" ] && [ "$$(cat "$$mode_marker")" != "$$current_mode" ]; then \
	echo "Cleaning stale build/ for JS engine $$current_mode"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -d build ] && [ ! -f "$$profile_marker" ]; then \
	echo "Cleaning stale build/ because no profile marker is present"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -f "$$profile_marker" ] && [ "$$(cat "$$profile_marker")" != "$$current_profile" ]; then \
	echo "Cleaning stale build/ for profile $$current_profile"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -d build ] && [ ! -f "$$flavour_marker" ]; then \
	echo "Cleaning stale build/ because no build flavour marker is present"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
if [ -f "$$flavour_marker" ] && [ "$$(cat "$$flavour_marker")" != "$$current_flavour" ]; then \
	echo "Cleaning stale build/ for build flavour $$current_flavour"; \
	eval "$$MAKE clean$$clean_args"; \
fi; \
mkdir -p build; \
printf "%s\n" "$$current" > "$$marker"; \
printf "%s\n" "$$current_mode" > "$$mode_marker"; \
printf "%s\n" "$$current_profile" > "$$profile_marker"; \
printf "%s\n" "$$current_flavour" > "$$flavour_marker"
endef

compile-db:
	$(ENV_HELPER) --shell 'bear -- $$MAKE $$USE_CPUS TARGET=gtk PROFILE=$(PROFILE)'


# ----------------------------------------------------------------------------
# Repo-root bootstrap, build, profiling, and verification
# WHY: Centralize the native build and profiling workflow behind reproducible
#      GNU Make targets so developers and CI do not have to hand-assemble
#      env.sh calls, workspace paths, or profiler arguments.
# ----------------------------------------------------------------------------
.PHONY: \
	doctor doctor-i386 bootstrap-tools bootstrap-libs bootstrap \
	build-native package-native build-gtk build-monkey build-monkey-enhanced build-framebuffer build-matrix-native \
	build-profile build-monkey-profile \
	test-unit sanitize-unit test-monkey-smoke test-monkey-division benchmark-monkey benchmark-monkey-enhanced \
	benchmark-profile profile-profile verify-profile \
	measure-monkey-engines measure-bootstrap-costs measure-i386-denominator \
	rebuild-i386-libs build-i386-denominator verify-i386-denominator \
	profile-valgrind-monkey profile-heaptrack-monkey profile-perf-monkey \
	static-analysis verify-native verify-matrix

doctor:
	python3 $(DOCTOR_TOOL)

doctor-i386:
	python3 $(I386_GATE_TOOL)

bootstrap-tools:
	$(ENV_HELPER) --shell 'ns-clone -d $(BOOTSTRAP_BRANCH_ARG) && ns-make-tools install'

bootstrap-libs:
	$(ENV_HELPER) --shell 'ns-clone -d $(BOOTSTRAP_BRANCH_ARG) && ns-make-libs install'

bootstrap:
	$(ENV_HELPER) --shell 'ns-clone -d $(BOOTSTRAP_BRANCH_ARG) && ns-make-tools install && ns-make-libs install'

build-native:
	$(ENV_HELPER) --shell 'build_target="$(TARGET)"; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS $(if $(TARGET),TARGET=$(TARGET),) PROFILE=$(PROFILE)'

package-native:
	$(ENV_HELPER) --shell '$$MAKE package $(if $(TARGET),TARGET=$(TARGET),) PROFILE=$(PROFILE)'

build-gtk:
	$(ENV_HELPER) --shell 'build_target=gtk; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=gtk PROFILE=$(PROFILE)'

build-monkey:
	$(ENV_HELPER) --shell 'build_target=monkey; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=monkey PROFILE=$(PROFILE)'

build-monkey-enhanced:
	$(ENV_HELPER) --shell 'export NETSURF_JS_ENGINE=enhanced; build_target=monkey; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=monkey PROFILE=$(PROFILE) NETSURF_JS_ENGINE=enhanced'

build-framebuffer:
	$(ENV_HELPER) --shell 'build_target=framebuffer; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=framebuffer PROFILE=$(PROFILE)'

build-profile:
ifeq ($(PROFILE),hostdev)
	$(MAKE) build-native $(if $(TARGET),TARGET=$(TARGET),) PROFILE=$(PROFILE)
else
	$(ENV_HELPER) --shell 'build_target=framebuffer; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=framebuffer PROFILE=$(PROFILE)'
endif

build-monkey-profile:
	$(ENV_HELPER) --shell 'build_target=monkey; $(PREPARE_WORKSPACE_BUILD) && $$MAKE $$USE_CPUS TARGET=monkey PROFILE=$(PROFILE)'

build-matrix-native:
	$(MAKE) build-gtk
	$(MAKE) build-monkey
	$(MAKE) build-framebuffer
	$(MAKE) test-unit

test-unit:
	$(ENV_HELPER) --shell '$$MAKE test PROFILE=$(PROFILE) $(if $(SKIP_MALLOC_LIMIT_TESTS),SKIP_MALLOC_LIMIT_TESTS=$(SKIP_MALLOC_LIMIT_TESTS),)'

sanitize-unit:
	$(ENV_HELPER) --shell '$$MAKE sanitize PROFILE=$(PROFILE)'

test-monkey-smoke: build-monkey
	$(ENV_HELPER) --shell 'LC_ALL=C.UTF-8; for plan in test/monkey-tests/history-api.yaml test/monkey-tests/polyfill-es6.yaml test/monkey-tests/innerHTML-correctness.yaml; do echo "=== $$plan ==="; python3 test/monkey-see-monkey-do ./nsmonkey $$plan || exit 1; done'

MONKEY_DIVISION ?= short-internet
test-monkey-division: build-monkey
	$(ENV_HELPER) --shell 'LC_ALL=C.UTF-8 test/monkey-see-monkey-do -v -d $(MONKEY_DIVISION)'

benchmark-monkey: build-monkey
	$(ENV_HELPER) --shell 'mkdir -p $(PROFILE_DIR) && python3 test/run-benchmark.py -m ./nsmonkey --json $(PROFILE_DIR)/monkey-benchmark.raw.json && python3 $(PERF_BASELINE_TOOL) summarize-benchmark --input $(PROFILE_DIR)/monkey-benchmark.raw.json --output $(PROFILE_DIR)/monkey-benchmark.json --scenario monkey-web-standards --target monkey --command "python3 test/run-benchmark.py -m ./nsmonkey" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST" && python3 $(PERF_BASELINE_TOOL) check --actual $(PROFILE_DIR)/monkey-benchmark.json --baseline $(PERF_BASELINE_DIR)/monkey-benchmark-baseline.json'

benchmark-monkey-enhanced: build-monkey-enhanced
	$(ENV_HELPER) --shell 'export NETSURF_JS_ENGINE=enhanced; mkdir -p $(PROFILE_DIR) && python3 test/run-benchmark.py -m ./nsmonkey --json $(PROFILE_DIR)/monkey-benchmark-enhanced.raw.json && python3 $(PERF_BASELINE_TOOL) summarize-benchmark --input $(PROFILE_DIR)/monkey-benchmark-enhanced.raw.json --output $(PROFILE_DIR)/monkey-benchmark-enhanced.json --scenario monkey-web-standards-enhanced --target monkey-enhanced --command "NETSURF_JS_ENGINE=enhanced python3 test/run-benchmark.py -m ./nsmonkey" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST NETSURF_JS_ENGINE=$$NETSURF_JS_ENGINE" && python3 $(PERF_BASELINE_TOOL) check --actual $(PROFILE_DIR)/monkey-benchmark-enhanced.json --baseline $(PERF_BASELINE_DIR)/monkey-benchmark-enhanced-baseline.json'

benchmark-profile: build-monkey-profile
	$(ENV_HELPER) --shell '$(PROFILE_SUITE_RUNTIME_ENV); suite="$(PROFILE_SUITE)"; if [ ! -f "$$suite" ]; then echo "Missing profile suite $$suite"; exit 1; fi; mkdir -p $(PROFILE_DIR) && python3 $(PROFILE_SUITE_TOOL) --monkey ./nsmonkey --suite "$$suite" --json $(PROFILE_DIR)/$(PROFILE)-suite.raw.json && python3 $(PERF_BASELINE_TOOL) summarize-suite --input $(PROFILE_DIR)/$(PROFILE)-suite.raw.json --output $(PROFILE_DIR)/$(PROFILE)-suite.json --scenario $(PROFILE)-profile-suite --target $(PROFILE)-monkey --command "python3 $(PROFILE_SUITE_TOOL) --monkey ./nsmonkey --suite $$suite" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST PROFILE=$(PROFILE) NETSURF_CHOICES=$$NETSURF_CHOICES" && python3 $(PERF_BASELINE_TOOL) check --actual $(PROFILE_DIR)/$(PROFILE)-suite.json --baseline $(PERF_BASELINE_DIR)/$(PROFILE)-suite-baseline.json'

profile-profile: build-monkey-profile
	$(ENV_HELPER) --shell '$(PROFILE_SUITE_RUNTIME_ENV); suite="$(PROFILE_SUITE)"; if [ ! -f "$$suite" ]; then echo "Missing profile suite $$suite"; exit 1; fi; mkdir -p $(PROFILE_DIR) && LC_ALL=C /usr/bin/time -v -o $(PROFILE_DIR)/$(PROFILE)-suite.time python3 $(PROFILE_SUITE_TOOL) --monkey ./nsmonkey --suite "$$suite" --json $(PROFILE_DIR)/$(PROFILE)-suite-telemetry.raw.json >/dev/null && python3 $(PERF_BASELINE_TOOL) summarize-time --input $(PROFILE_DIR)/$(PROFILE)-suite.time --output $(PROFILE_DIR)/$(PROFILE)-telemetry.json --scenario $(PROFILE)-profile-telemetry --target $(PROFILE)-monkey --command "/usr/bin/time -v python3 $(PROFILE_SUITE_TOOL) --monkey ./nsmonkey --suite $$suite" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST PROFILE=$(PROFILE) NETSURF_CHOICES=$$NETSURF_CHOICES" && python3 $(PERF_BASELINE_TOOL) check --actual $(PROFILE_DIR)/$(PROFILE)-telemetry.json --baseline $(PERF_BASELINE_DIR)/$(PROFILE)-telemetry-baseline.json'

measure-monkey-engines:
	python3 $(MEASURE_WORKFLOW_TOOL) compare-monkey-engines --restore-standard

measure-bootstrap-costs:
	python3 $(MEASURE_WORKFLOW_TOOL) measure-bootstrap-costs

measure-i386-denominator:
	python3 $(I386_GATE_TOOL)

rebuild-i386-libs:
	python3 $(I386_REBUILD_TOOL)

build-i386-denominator:
	python3 $(I386_BUILD_TOOL)

verify-i386-denominator:
	python3 $(I386_GATE_TOOL) --strict && python3 $(I386_BUILD_TOOL) --strict

profile-valgrind-monkey: build-monkey
	$(ENV_HELPER) --shell 'mkdir -p $(PROFILE_DIR) && python3 test/monkey_driver.py -m ./nsmonkey -w "valgrind --quiet --tool=memcheck --xml=yes --xml-file=$(PROFILE_DIR)/monkey-valgrind.xml --leak-check=full --track-origins=yes --suppressions=tools/valgrind.supp" -t test/monkey-tests/start-stop.yaml && python3 $(PERF_BASELINE_TOOL) summarize-valgrind --input $(PROFILE_DIR)/monkey-valgrind.xml --output $(PROFILE_DIR)/monkey-valgrind.json --scenario monkey-start-stop --target monkey --command "valgrind memcheck nsmonkey start-stop" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST" && python3 $(PERF_BASELINE_TOOL) check --actual $(PROFILE_DIR)/monkey-valgrind.json --baseline $(PERF_BASELINE_DIR)/monkey-valgrind-baseline.json'

profile-heaptrack-monkey: build-monkey
	$(ENV_HELPER) --shell 'mkdir -p $(PROFILE_DIR) && python3 test/monkey_driver.py -m ./nsmonkey -w "heaptrack --record-only -o $(PROFILE_DIR)/monkey-heaptrack" -t test/monkey-tests/start-stop.yaml > $(PROFILE_DIR)/monkey-heaptrack.log 2>&1 && heaptrack_print -f $(PROFILE_DIR)/monkey-heaptrack.zst --print-peaks --print-allocators --print-temporary --peak-limit 10 > $(PROFILE_DIR)/monkey-heaptrack-report.txt 2>> $(PROFILE_DIR)/monkey-heaptrack.log && python3 $(PERF_BASELINE_TOOL) summarize-heaptrack --input $(PROFILE_DIR)/monkey-heaptrack.log --profile-file $(PROFILE_DIR)/monkey-heaptrack.zst --output $(PROFILE_DIR)/monkey-heaptrack.json --scenario monkey-start-stop --target monkey --command "heaptrack --record-only nsmonkey start-stop" --environment "TARGET_WORKSPACE=$$TARGET_WORKSPACE HOST=$$HOST"'

profile-perf-monkey: build-monkey
	$(ENV_HELPER) --shell 'mkdir -p $(PROFILE_DIR); if perf stat true >/dev/null 2>$(PROFILE_DIR)/monkey-perf.stderr; then python3 test/monkey_driver.py -m ./nsmonkey -w "perf stat -x, -o $(PROFILE_DIR)/monkey-perf.csv" -t test/monkey-tests/start-stop.yaml && printf "{\n  \"tool\": \"perf\",\n  \"status\": \"supported\",\n  \"csv\": \"$(PROFILE_DIR)/monkey-perf.csv\"\n}\n" > $(PROFILE_DIR)/monkey-perf.json; else reason=$$(awk '\''NF { line=$$0 } END { print line }'\'' $(PROFILE_DIR)/monkey-perf.stderr 2>/dev/null); printf "{\n  \"tool\": \"perf\",\n  \"status\": \"unsupported\",\n  \"reason\": \"%s\"\n}\n" "$$reason" > $(PROFILE_DIR)/monkey-perf.json; cat $(PROFILE_DIR)/monkey-perf.json; fi'

static-analysis:
	cppcheck \
	  --error-exitcode=1 \
	  --force \
	  --suppress=*:content/handlers/javascript/duktape/duktape.c \
	  --suppress=*:content/handlers/javascript/enhanced/engine.c \
	  --suppress=syntaxError:content/handlers/javascript/duktape/duk_config.h \
	  --suppress=internalAstError:content/handlers/html/box_special.c \
	  --suppress=syntaxError:content/handlers/html/redraw.c \
	  --suppress=syntaxError:content/handlers/image/rsvg.c \
	  --suppress=memleak:content/handlers/css/css.c \
	  --suppress=memleak:content/handlers/image/bmp.c \
	  --suppress=memleak:content/handlers/image/gif.c \
	  --suppress=memleak:content/handlers/image/ico.c \
	  --suppress=memleak:content/handlers/image/nssprite.c \
	  --suppress=memleak:content/handlers/image/png.c \
	  --suppress=memleak:content/handlers/image/rsvg246.c \
	  --suppress=memleak:content/handlers/image/svg.c \
	  --suppress=memleak:content/handlers/javascript/content.c \
	  --suppress=memleak:content/handlers/text/textplain.c \
	  --suppress=doubleFree:desktop/cookie_manager.c \
	  --suppress=doubleFree:desktop/global_history.c \
	  --suppress=doubleFree:desktop/hotlist.c \
	  --suppress=unknownMacro:utils/talloc.c \
	  --suppress=va_list_usedBeforeStarted:utils/talloc.c \
	  --suppress=memleak:utils/talloc.c \
	  --inline-suppr \
	  -j "$(shell nproc)" \
	  --std=c99 \
	  -I include \
	  -I . \
	  -i content/handlers/javascript/duktape/duktape.c \
	  -i content/handlers/javascript/enhanced/engine.c \
	  -i utils/talloc.c \
	  content/ \
	  desktop/ \
	  utils/

verify-native:
	$(MAKE) doctor
	$(MAKE) build-matrix-native
	$(MAKE) test-monkey-smoke
	$(MAKE) benchmark-monkey
	$(MAKE) static-analysis

verify-profile:
	$(MAKE) doctor PROFILE=$(PROFILE)
	$(MAKE) build-profile PROFILE=$(PROFILE) $(if $(TARGET),TARGET=$(TARGET),)
	$(MAKE) test-unit PROFILE=$(PROFILE)
	$(MAKE) benchmark-profile PROFILE=$(PROFILE)
	$(MAKE) profile-profile PROFILE=$(PROFILE)

verify-matrix:
	$(MAKE) verify-native


# ----------------------------------------------------------------------------
# lacunae -- JS binding gap analysis tool (zero-dep single script)
# WHY: Data-driven visibility into which JS API gaps to close next.
# HOW: Run lacunae-scan first; then lacunae-gaps or lacunae-diff.
# ----------------------------------------------------------------------------
.PHONY: lacunae-scan lacunae-gaps lacunae-diff lacunae-baseline

lacunae-scan:
	python3 tools/lacunae.py scan

lacunae-gaps: lacunae-scan
	python3 tools/lacunae.py gaps --top 20

lacunae-diff: lacunae-scan
	python3 tools/lacunae.py diff

lacunae-baseline: lacunae-scan
	cp lacunae-gaps.json test/lacunae-baseline.json
	@echo "Baseline updated: test/lacunae-baseline.json"
