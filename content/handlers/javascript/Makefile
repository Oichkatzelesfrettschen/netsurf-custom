#
# NetSurf javascript source file inclusion
#
# Included by Makefile
#

S_JAVASCRIPT_BINDING:=

ifeq ($(NETSURF_JS_ENGINE),enhanced)
# Enhanced mode: QuickJS-NG based ES2023 engine
include content/handlers/javascript/enhanced/Makefile
else ifeq ($(NETSURF_JS_ENGINE),standard)
# Standard mode: Duktape ES5.1 engine
include content/handlers/javascript/duktape/Makefile
else ifeq ($(NETSURF_USE_DUKTAPE),YES)
# Legacy flag compatibility
include content/handlers/javascript/duktape/Makefile
else
# None
include content/handlers/javascript/none/Makefile
endif

# Fetcher for javascript scheme is always required
S_JAVASCRIPT += fetcher.c
