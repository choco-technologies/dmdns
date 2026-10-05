/**
 * @file dmdns_test_provider.c
 * @brief Test-only dmdns_provide_servers DIF implementation - see
 *        include/dmdns_test_provider.h
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmdns_test_provider.h"
#include <string.h>

static dmip_addr_t* g_servers = NULL;
static size_t       g_server_count = 0;

static void clear_servers(void)
{
    Dmod_Free(g_servers);
    g_servers = NULL;
    g_server_count = 0;
}

dmod_dmdns_test_provider_api_declaration(1.0, void, _set, ( const dmip_addr_t* servers, size_t count ))
{
    clear_servers();
    if (servers == NULL || count == 0)
        return;

    g_servers = Dmod_Malloc(count * sizeof(dmip_addr_t));
    if (g_servers == NULL)
        return;
    memcpy(g_servers, servers, count * sizeof(dmip_addr_t));
    g_server_count = count;
}

dmod_dmdns_dif_api_declaration(1.0, dmdns_test_provider, void, _provide_servers, ( dmdns_server_sink_t add, void* sink_ctx ))
{
    for (size_t i = 0; i < g_server_count; i++)
        add(sink_ctx, &g_servers[i]);
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    clear_servers();
    return 0;
}

int dmod_deinit(void)
{
    clear_servers();
    return 0;
}
