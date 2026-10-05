/*
 * Registers dmdns's Built-in API in the .dmod.inputs section - the same
 * shape dmdhcp_registrations.c/dmtcp_registrations.c use for their own
 * multi-file modules.
 *
 * This must live in its own translation unit, separate from every other
 * dmdns_*.c file: the registration struct array dmdns_defs.h generates
 * when DMOD_ENABLE_REGISTRATION is set covers every function declared in
 * dmdns.h, not just the ones defined in whichever file set the macro - so
 * defining it in more than one translation unit produces one duplicate
 * "multiple definition" linker error per public function. Only dmdns.h is
 * included here (not the full dmod.h) so this can't accidentally
 * re-register dmod's own kernel Built-in APIs too.
 *
 * The dmdns_provide_servers DIF signature is NOT defined here (no
 * ENABLE_DIF_REGISTRATIONS) - see the top comment of dmdns_servers.c.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmdns.h"
