/*
 * Include redirect for nsgenbind-generated code.
 *
 * WHY: nsgenbind output does #include "javascript/duktape/duktape.h".
 * In enhanced mode, we redirect this to the Duktape API compatibility
 * shim that emulates Duktape's stack-based API using QuickJS-NG.
 */
#ifndef NETSURF_ENHANCED_DUKTAPE_REDIRECT_H
#define NETSURF_ENHANCED_DUKTAPE_REDIRECT_H

#include "content/handlers/javascript/enhanced/duk_compat.h"

#endif /* NETSURF_ENHANCED_DUKTAPE_REDIRECT_H */
