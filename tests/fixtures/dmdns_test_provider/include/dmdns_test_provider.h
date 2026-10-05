#ifndef DMDNS_TEST_PROVIDER_H
#define DMDNS_TEST_PROVIDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmdns.h"
#include "dmdns_test_provider_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmdns_test_provider.h
 * @brief Test-only fixture module implementing the dmdns_provide_servers DIF
 *
 * dmdns_test.c needs a *loaded, enabled* module implementing
 * dmdns_provide_servers() to test dmdns_get_servers()'s provider
 * discovery - standing in for a DHCP client. The test binary itself can't
 * be that module: it is an Application-type module (dmod_add_test()) and
 * Dmod_GetNextDifModule() only returns *enabled* modules, a state only a
 * Library-type module reaches - the same reason dmip's own tests use
 * dmip_test_fixture (see dmip/tests/fixtures/). This fixture is loaded and
 * enabled as one of test_dmdns's required modules, and the test drives
 * what it reports through this Built-in API.
 */

/** @brief Most servers the fixture can be told to report */
#define DMDNS_TEST_PROVIDER_MAX_SERVERS 4u

/**
 * @brief Set what the fixture's dmdns_provide_servers() reports from now
 *        on - `count` 0 (or `servers` NULL) makes it report nothing
 */
dmod_dmdns_test_provider_api(1.0, void, _set, ( const dmip_addr_t* servers, size_t count ));

#ifdef __cplusplus
}
#endif

#endif // DMDNS_TEST_PROVIDER_H
