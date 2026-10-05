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

static size_t lookup_localhost(uint16_t qtype, dmip_addr_t* out)
{
    memset(out, 0, sizeof(*out));
    if (qtype == DMDNS_TYPE_A)
    {
        out->family = dmip_family_v4;
        out->addr.v4[0] = 127;
        out->addr.v4[3] = 1;
    }
    else
    {
        out->family = dmip_family_v6;
        out->addr.v6[DMIP_IPV6_ADDR_LEN - 1u] = 1;
    }
    return 1;
}

size_t dmdns_hosts_lookup(const char* name, uint16_t qtype, dmip_addr_t* out, size_t max)
{
    if (max == 0u)
        return 0;
    if (dmdns_name_equal(name, LOCALHOST_NAME))
        return lookup_localhost(qtype, out);

    size_t count = 0;
    dmosi_mutex_lock(g_hosts_mutex);
    size_t size = dmlist_size(g_hosts);
    for (size_t i = 0; i < size && count < max; i++)
    {
        host_entry_t* entry = dmlist_get(g_hosts, i);
        if (dmdns_family_to_qtype(entry->addr.family) == qtype && dmdns_name_equal(entry->name, name))
            out[count++] = entry->addr;
    }
    dmosi_mutex_unlock(g_hosts_mutex);
    return count;
}
