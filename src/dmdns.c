/**
 * @file dmdns.c
 * @brief DMOD lifecycle (dmod_init()/_deinit()) and dmdns_resolve()
 *
 * dmdns_resolve() only orchestrates - every step it takes lives in its own
 * file (see dmdns_internal.h's file map). dmod_init() binds no UDP port:
 * each dmdns_query() binds its own ephemeral one for as long as it runs,
 * so a loaded-but-idle dmdns costs nothing but its (empty) tables.
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include <errno.h>

/* Record types tried, in order, for dmip_family_none ("any address"). */
static const uint16_t g_any_qtypes[] = { DMDNS_TYPE_A, DMDNS_TYPE_AAAA };

static bool is_definitive(int result)
{
    return result == 0 || result == -ENOENT || result == -ENODATA;
}

/* An IP literal resolves to itself - no lookup. Returns 1 if `name` is not a literal. */
static int resolve_literal(const char* name, dmip_family_t family, dmip_addr_t** out_addrs, size_t* out_count)
{
    dmip_addr_t addr;
    if (dmdns_parse_address(name, &addr) != 0)
        return 1;
    if (family != dmip_family_none && family != addr.family)
        return -ENODATA;

    *out_addrs = dmdns_addr_copy(&addr, 1u);
    if (*out_addrs == NULL)
        return -ENOMEM;
    *out_count = 1;
    return 0;
}

/* Ask `servers` in turn, DMDNS_ATTEMPTS rounds, until one gives a definitive answer - which is cached. */
static int ask_each(const dmip_addr_t* servers, size_t server_count, const char* name, uint16_t qtype,
    dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms)
{
    int result = -ETIMEDOUT;
    for (size_t attempt = 0; attempt < DMDNS_ATTEMPTS; attempt++)
    {
        for (size_t i = 0; i < server_count; i++)
        {
            uint32_t ttl_sec = 0;
            result = dmdns_query(&servers[i], name, qtype, out_addrs, out_count, &ttl_sec, timeout_ms);
            if (is_definitive(result))
            {
                dmdns_cache_store(name, qtype, result, *out_addrs, *out_count, ttl_sec);
                return result;
            }
        }
    }
    return result;
}

static int ask_servers(const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms)
{
    dmip_addr_t* servers = NULL;
    size_t server_count = 0;
    int result = dmdns_get_servers(&servers, &server_count);
    if (result != 0)
        return result;
    if (server_count == 0u)
        return -EDESTADDRREQ;

    result = ask_each(servers, server_count, name, qtype, out_addrs, out_count, timeout_ms);
    Dmod_Free(servers);
    return result;
}

static int resolve_remote(const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms)
{
    int result = dmdns_cache_lookup(name, qtype, out_addrs, out_count);
    if (result > 0)
        return 0;
    if (result < 0)
        return result;
    return ask_servers(name, qtype, out_addrs, out_count, timeout_ms);
}

static int resolve_name(const char* name, const uint16_t* qtypes, size_t qtype_count,
    dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms)
{
    for (size_t i = 0; i < qtype_count; i++)
    {
        int result = dmdns_hosts_lookup(name, qtypes[i], out_addrs, out_count);
        if (result != -ENOENT)
            return result;
    }

    int result = -ENODATA;
    for (size_t i = 0; i < qtype_count && result == -ENODATA; i++)
        result = resolve_remote(name, qtypes[i], out_addrs, out_count, timeout_ms);
    return result;
}

dmod_dmdns_api_declaration(1.0, int, _resolve, ( const char* name, dmip_family_t family, dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms ))
{
    if (name == NULL || out_addrs == NULL || out_count == NULL)
        return -EINVAL;
    *out_addrs = NULL;
    *out_count = 0;
    if (family != dmip_family_none && dmdns_family_to_qtype(family) == 0u)
        return -EINVAL;

    int result = resolve_literal(name, family, out_addrs, out_count);
    if (result != 1)
        return result;
    if (!dmdns_is_valid_name(name))
        return -EINVAL;

    if (family == dmip_family_none)
        return resolve_name(name, g_any_qtypes, sizeof(g_any_qtypes) / sizeof(g_any_qtypes[0]), out_addrs, out_count, timeout_ms);

    uint16_t qtype = dmdns_family_to_qtype(family);
    return resolve_name(name, &qtype, 1u, out_addrs, out_count, timeout_ms);
}

static void deinit_all(void)
{
    dmdns_query_deinit();
    dmdns_cache_deinit();
    dmdns_hosts_deinit();
    dmdns_servers_deinit();
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    if (dmdns_servers_init() != 0 || dmdns_hosts_init() != 0 || dmdns_cache_init() != 0 || dmdns_query_init() != 0)
    {
        DMOD_LOG_ERROR("Failed to allocate dmdns state\n");
        deinit_all();
        return -1;
    }

    DMOD_LOG_INFO("DMDNS initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    deinit_all();
    DMOD_LOG_INFO("DMDNS deinitialized\n");
    return 0;
}
