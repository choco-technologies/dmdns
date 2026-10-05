/**
 * @file dmdns_hosts.c
 * @brief Static name -> address table (the /etc/hosts of dmdns) and the
 *        built-in "localhost"
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include <string.h>
#include <errno.h>

#define LOCALHOST_NAME "localhost"

typedef struct
{
    char*       name;
    dmip_addr_t addr;
} host_entry_t;

static dmlist_context_t* g_hosts = NULL;
static dmosi_mutex_t     g_hosts_mutex = NULL;

uint16_t dmdns_family_to_qtype(dmip_family_t family)
{
    if (family == dmip_family_v4)
        return DMDNS_TYPE_A;
    if (family == dmip_family_v6)
        return DMDNS_TYPE_AAAA;
    return 0;
}

static void free_entry(host_entry_t* entry)
{
    Dmod_Free(entry->name);
    Dmod_Free(entry);
}

int dmdns_hosts_init(void)
{
    g_hosts = dmlist_create();
    g_hosts_mutex = dmosi_mutex_create(false);
    return (g_hosts != NULL && g_hosts_mutex != NULL) ? 0 : -ENOMEM;
}

void dmdns_hosts_deinit(void)
{
    if (g_hosts != NULL)
    {
        host_entry_t* entry;
        while ((entry = dmlist_pop_front(g_hosts)) != NULL)
            free_entry(entry);
        dmlist_destroy(g_hosts);
        g_hosts = NULL;
    }
    if (g_hosts_mutex != NULL)
    {
        dmosi_mutex_destroy(g_hosts_mutex);
        g_hosts_mutex = NULL;
    }
}

/* Caller holds g_hosts_mutex. */
static bool contains_mapping(const char* name, const dmip_addr_t* addr)
{
    size_t size = dmlist_size(g_hosts);
    for (size_t i = 0; i < size; i++)
    {
        host_entry_t* entry = dmlist_get(g_hosts, i);
        if (dmdns_name_equal(entry->name, name) && dmdns_addr_equal(&entry->addr, addr))
            return true;
    }
    return false;
}

dmod_dmdns_api_declaration(1.0, int, _add_host, ( const char* name, const dmip_addr_t* addr ))
{
    if (!dmdns_is_valid_name(name) || addr == NULL || dmdns_family_to_qtype(addr->family) == 0u)
        return -EINVAL;

    host_entry_t* entry = Dmod_Malloc(sizeof(*entry));
    if (entry == NULL)
        return -ENOMEM;
    entry->name = Dmod_StrDup(name);
    entry->addr = *addr;
    if (entry->name == NULL)
    {
        Dmod_Free(entry);
        return -ENOMEM;
    }

    int result = 0;
    dmosi_mutex_lock(g_hosts_mutex);
    if (contains_mapping(name, addr))
        result = -EEXIST;
    else if (!dmlist_push_back(g_hosts, entry))
        result = -ENOMEM;
    dmosi_mutex_unlock(g_hosts_mutex);

    if (result != 0)
        free_entry(entry);
    return result;
}

dmod_dmdns_api_declaration(1.0, int, _remove_host, ( const char* name ))
{
    if (name == NULL)
        return -EINVAL;

    size_t removed = 0;
    dmosi_mutex_lock(g_hosts_mutex);
    for (size_t i = 0; i < dmlist_size(g_hosts); )
    {
        host_entry_t* entry = dmlist_get(g_hosts, i);
        if (!dmdns_name_equal(entry->name, name))
        {
            i++;
            continue;
        }
        dmlist_remove_at(g_hosts, i);
        free_entry(entry);
        removed++;
    }
    dmosi_mutex_unlock(g_hosts_mutex);

    return (removed > 0u) ? 0 : -ENOENT;
}

static int lookup_localhost(uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count)
{
    dmip_addr_t addr = { 0 };
    if (qtype == DMDNS_TYPE_A)
    {
        addr.family = dmip_family_v4;
        addr.addr.v4[0] = 127;
        addr.addr.v4[3] = 1;
    }
    else
    {
        addr.family = dmip_family_v6;
        addr.addr.v6[DMIP_IPV6_ADDR_LEN - 1u] = 1;
    }

    *out_addrs = dmdns_addr_copy(&addr, 1u);
    *out_count = (*out_addrs != NULL) ? 1u : 0u;
    return (*out_addrs != NULL) ? 0 : -ENOMEM;
}

int dmdns_hosts_lookup(const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count)
{
    *out_addrs = NULL;
    *out_count = 0;
    if (dmdns_name_equal(name, LOCALHOST_NAME))
        return lookup_localhost(qtype, out_addrs, out_count);

    int result = 0;
    dmosi_mutex_lock(g_hosts_mutex);
    size_t size = dmlist_size(g_hosts);
    for (size_t i = 0; i < size && result == 0; i++)
    {
        host_entry_t* entry = dmlist_get(g_hosts, i);
        if (dmdns_family_to_qtype(entry->addr.family) == qtype && dmdns_name_equal(entry->name, name))
            result = dmdns_addr_append(out_addrs, out_count, &entry->addr);
    }
    dmosi_mutex_unlock(g_hosts_mutex);

    if (result != 0)
    {
        Dmod_Free(*out_addrs);
        *out_addrs = NULL;
        *out_count = 0;
        return result;
    }
    return (*out_count > 0u) ? 0 : -ENOENT;
}
