/**
 * @file dmdns_test_provider.c
 * @brief Test-only dmdns_provide_servers DIF implementation - see
 *        include/dmdns_test_provider.h
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmdns_test_provider.h"
#include <string.h>

static dmip_addr_t g_servers[DMDNS_TEST_PROVIDER_MAX_SERVERS];
static size_t      g_server_count = 0;

dmod_dmdns_test_provider_api_declaration(1.0, void, _set, ( const dmip_addr_t* servers, size_t count ))
{
    if (servers == NULL)
        count = 0;
    if (count > DMDNS_TEST_PROVIDER_MAX_SERVERS)
        count = DMDNS_TEST_PROVIDER_MAX_SERVERS;
    if (count > 0)
        memcpy(g_servers, servers, count * sizeof(dmip_addr_t));
    g_server_count = count;
}

dmod_dmdns_dif_api_declaration(1.0, dmdns_test_provider, size_t, _provide_servers, ( dmip_addr_t* out_servers, size_t max_servers ))
{
    size_t count = (g_server_count < max_servers) ? g_server_count : max_servers;
    memcpy(out_servers, g_servers, count * sizeof(dmip_addr_t));
    return count;
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    g_server_count = 0;
    return 0;
}

int dmod_deinit(void)
{
    g_server_count = 0;
    return 0;
}
