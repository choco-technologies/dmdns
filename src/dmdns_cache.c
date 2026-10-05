/**
 * @file dmdns_cache.c
 * @brief Answer cache keyed by (name, record type), expired by TTL
 *
 * Positive answers live for the smallest TTL of their records (capped at
 * DMDNS_CACHE_MAX_TTL_SEC), negative ones (NXDOMAIN/NODATA) for
 * DMDNS_NEGATIVE_TTL_SEC. At most DMDNS_CACHE_MAX_ENTRIES names are kept;
 * when full, the oldest entry is evicted - entries are always appended, so
 * that is simply the front of the list. Expiry is checked lazily, on
 * lookup, so the cache needs no timer of its own.
 *
 * Time comes from dmosi_get_tick_count() (1 ms per tick on every dmosi
 * port in this tree); the wrap-around safe "(int32_t)(a - b)" comparison
 * keeps working across the 49-day tick counter overflow.
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include <string.h>
#include <errno.h>

#define MS_PER_SEC 1000u

typedef struct
{
    char*        name;
    uint16_t     qtype;
    int          result;      /* > 0: number of addrs, < 0: cached negative errno */
    dmip_addr_t* addrs;
    uint32_t     expires_at;  /* dmosi tick */
} cache_entry_t;

static dmlist_context_t* g_cache = NULL;
static dmosi_mutex_t     g_cache_mutex = NULL;

static void free_entry(cache_entry_t* entry)
{
    Dmod_Free(entry->addrs);
    Dmod_Free(entry->name);
    Dmod_Free(entry);
}

static bool is_expired(const cache_entry_t* entry, uint32_t now)
{
    return (int32_t)(entry->expires_at - now) <= 0;
}

/* Caller holds g_cache_mutex. Returns the entry's index, or -1. */
static int find_entry(const char* name, uint16_t qtype)
{
    size_t size = dmlist_size(g_cache);
    for (size_t i = 0; i < size; i++)
    {
        cache_entry_t* entry = dmlist_get(g_cache, i);
        if (entry->qtype == qtype && dmdns_name_equal(entry->name, name))
            return (int)i;
    }
    return -1;
}

/* Caller holds g_cache_mutex. */
static void remove_at(size_t index)
{
    cache_entry_t* entry = dmlist_remove_at(g_cache, index);
    if (entry != NULL)
        free_entry(entry);
}

int dmdns_cache_init(void)
{
    g_cache = dmlist_create();
    g_cache_mutex = dmosi_mutex_create(false);
    return (g_cache != NULL && g_cache_mutex != NULL) ? 0 : -ENOMEM;
}

void dmdns_cache_deinit(void)
{
    if (g_cache != NULL)
    {
        dmdns_flush_cache();
        dmlist_destroy(g_cache);
        g_cache = NULL;
    }
    if (g_cache_mutex != NULL)
    {
        dmosi_mutex_destroy(g_cache_mutex);
        g_cache_mutex = NULL;
    }
}

dmod_dmdns_api_declaration(1.0, void, _flush_cache, ( void ))
{
    dmosi_mutex_lock(g_cache_mutex);
    while (dmlist_size(g_cache) > 0u)
        remove_at(0);
    dmosi_mutex_unlock(g_cache_mutex);
}

int dmdns_cache_lookup(const char* name, uint16_t qtype, dmip_addr_t* out, size_t max)
{
    int result = 0;
    dmosi_mutex_lock(g_cache_mutex);
    int index = find_entry(name, qtype);
    if (index >= 0)
    {
        cache_entry_t* entry = dmlist_get(g_cache, (size_t)index);
        if (is_expired(entry, dmosi_get_tick_count()))
        {
            remove_at((size_t)index);
        }
        else if (entry->result < 0)
        {
            result = entry->result;
        }
        else
        {
            size_t count = ((size_t)entry->result < max) ? (size_t)entry->result : max;
            memcpy(out, entry->addrs, count * sizeof(dmip_addr_t));
            result = (int)count;
        }
    }
    dmosi_mutex_unlock(g_cache_mutex);
    return result;
}

static cache_entry_t* create_entry(const char* name, uint16_t qtype, int result, const dmip_addr_t* addrs, uint32_t ttl_sec)
{
    cache_entry_t* entry = Dmod_Malloc(sizeof(*entry));
    if (entry == NULL)
        return NULL;

    size_t count = (result > 0) ? (size_t)result : 0u;
    entry->name       = Dmod_StrDup(name);
    entry->qtype      = qtype;
    entry->result     = result;
    entry->addrs      = (count > 0u) ? Dmod_Malloc(count * sizeof(dmip_addr_t)) : NULL;
    entry->expires_at = dmosi_get_tick_count() + ttl_sec * MS_PER_SEC;

    if (entry->name == NULL || (count > 0u && entry->addrs == NULL))
    {
        free_entry(entry);
        return NULL;
    }
    if (count > 0u)
        memcpy(entry->addrs, addrs, count * sizeof(dmip_addr_t));
    return entry;
}

void dmdns_cache_store(const char* name, uint16_t qtype, int result, const dmip_addr_t* addrs, uint32_t ttl_sec)
{
    if (ttl_sec > DMDNS_CACHE_MAX_TTL_SEC)
        ttl_sec = DMDNS_CACHE_MAX_TTL_SEC;
    if (ttl_sec == 0u || result == 0)
        return;

    cache_entry_t* entry = create_entry(name, qtype, result, addrs, ttl_sec);
    if (entry == NULL)
        return;

    dmosi_mutex_lock(g_cache_mutex);
    int index = find_entry(name, qtype);
    if (index >= 0)
        remove_at((size_t)index);
    if (dmlist_size(g_cache) >= DMDNS_CACHE_MAX_ENTRIES)
        remove_at(0);
    if (!dmlist_push_back(g_cache, entry))
        free_entry(entry);
    dmosi_mutex_unlock(g_cache_mutex);
}
