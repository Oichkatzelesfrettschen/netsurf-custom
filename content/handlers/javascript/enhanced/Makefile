#
# NetSurf enhanced JavaScript engine (QuickJS-NG based) source file inclusion
#
# Included by javascript/Makefile when NETSURF_JS_ENGINE=enhanced
#
# WHY: Provides ES2023 JavaScript support via QuickJS-NG, adapted into
# NetSurf's native architecture. Uses the same js.h API contract and
# nsgenbind binding infrastructure as the standard Duktape engine.
#

# Bridge implements js.h using QuickJS-NG native API
S_JAVASCRIPT += enhanced/bridge.c

# QuickJS-NG engine (vendored amalgamation, compiled with warnings suppressed)
S_JAVASCRIPT += enhanced/engine.c

# Duktape API compatibility shim (makes nsgenbind output compile)
S_JAVASCRIPT += enhanced/duk_compat.c

# Real implementations of dukky_* helpers (ported from duktape/dukky.c)
S_JAVASCRIPT += enhanced/dukky_helpers.c

# Content handler for text/javascript MIME type (defines javascript_init)
S_JAVASCRIPT += content.c

# Suppress warnings in vendored QuickJS-NG code (not our code to fix).
# The build system flattens paths (/ -> _) for object filenames.
# WHY: COMMON_WARNFLAGS and CWARNFLAGS appear before CFLAGS in the
# compile command, so -w in CFLAGS overrides them all.
$(OBJROOT)/content_handlers_javascript_enhanced_engine.o: \
	CFLAGS += -w
$(OBJROOT)/content_handlers_javascript_enhanced_engine.o: \
	COMMON_WARNFLAGS := -w

# The compat include path must come BEFORE content/handlers in the
# include search order so that #include "javascript/duktape/duktape.h"
# in nsgenbind output resolves to our duk_compat.h redirect instead of
# the real duktape.h.
# WHY: IFLAGS is built from INCLUDE_DIRS and appears before CFLAGS in
# compile commands. Adding to INCLUDE_DIRS here (before content/handlers
# Makefile:27 appends content/handlers) ensures correct ordering.
INCLUDE_DIRS += content/handlers/javascript/enhanced/compat

# Include nsgenbind-generated bindings (same .bnd files as Duktape mode)
content/handlers/javascript/enhanced/bridge.c: \
	$(OBJROOT)/duktape/binding.h \
	$(OBJROOT)/duktape/generics.js.inc \
	$(OBJROOT)/duktape/polyfill.js.inc

BINDINGS := $(wildcard content/handlers/javascript/duktape/*.bnd)

# Generator for the C include representing the generics.js
$(OBJROOT)/duktape/generics.js.inc: content/handlers/javascript/duktape/generics.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/duktape
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_duktape_generics_js/generics_js/' $@.tmp > $@

# Generator for the C include representing the polyfill.js
$(OBJROOT)/duktape/polyfill.js.inc: content/handlers/javascript/duktape/polyfill.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/duktape
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_duktape_polyfill_js/polyfill_js/' $@.tmp > $@

# ensure genbind generates debugging files
GBFLAGS+=-D

$(OBJROOT)/duktape/binding.h $(OBJROOT)/duktape/Makefile: content/handlers/javascript/duktape/netsurf.bnd $(BINDINGS)
	$(Q)$(MKDIR) -p $(OBJROOT)/duktape
	$(VQ)echo " GENBIND: $<"
	$(Q)nsgenbind $(GBFLAGS) -I content/handlers/javascript/WebIDL $< $(OBJROOT)/duktape

ifeq ($(filter $(MAKECMDGOALS),clean test coverage),)
-include $(OBJROOT)/duktape/Makefile
endif

S_JAVASCRIPT_BINDING:=$(addprefix $(OBJROOT)/duktape/,$(NSGENBIND_SOURCES))

$(S_JAVASCRIPT_BINDING): $(BINDINGS)

# Mark as enhanced mode for conditional compilation
CFLAGS += -DNETSURF_ENHANCED_JS
