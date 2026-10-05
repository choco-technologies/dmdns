/**
 * @file dmdns_servers.c
 * @brief Where name servers come from: a static list + the
 *        dmdns_provide_servers DIF
 *
 * The static list is a dmlist of heap dmip_addr_t guarded by a mutex.
 * Providers are discovered on every call through Dmod_GetNextDifModule()
 * rather than registered - the same shape dmip uses for its protocol
 * handlers (see dispatch_packet() in dmip.c): a provider that crashes or is
 * unloaded simply stops being found, nothing stale is ever called, and
 * dmdns never needs to know who provides servers (a DHCP client, a PPP
 * link, a static config module...).
 *
 * ENABLE_DIF_REGISTRATIONS is set here, in the only file that uses
 * dmod_dmdns_provide_servers_sig, on purpose: it defines that signature as
 * a `const char* const` initialised with a string literal. The dmod loader
 * relocates a module's GOT but not pointers stored in its .data, so the
 * variable must be visible together with its initialiser where it is used
 * - the compiler then folds it into a PC-relative address of the literal
 * instead of loading the (unrelocated) pointer. Defining it in another
 * translation unit and reading it through an extern crashes at runtime.
 * dmip.c and dmnetbridge.c define their own DIF signatures in the file
 * that uses them for the same reason.
 */
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod.h"
#include "dmdns_internal.h"
#include <string.h>
#include <errno.h>

static dmlist_context_t* g_servers = NULL;
static dmosi_mutex_t     g_servers_mutex = NULL;

typedef struct
{
    dmip_addr_t* out;
    size_t       max;
    size_t       count;
} server_set_t;

static int compare_server(const void* entry, const void* key)
{
    return dmdns_addr_equal((const dmip_addr_t*)entry, (const dmip_addr_t*)key) ? 0 : 1;
}

static bool is_usable_server(const dmip_addr_t* server)
{
    return server != NULL && (server->family == dmip_family_v4 || server->family == dmip_family_v6);
}

int dmdns_servers_init(void)
{
    g_servers = dmlist_create();
    g_servers_mutex = dmosi_mutex_create(false);
    return (g_servers != NULL && g_servers_mutex != NULL) ? 0 : -ENOMEM;
}

void dmdns_servers_deinit(void)
{
    if (g_servers != NULL)
    {
        dmdns_clear_servers();
        dmlist_destroy(g_servers);
        g_servers = NULL;
    }
    if (g_servers_mutex != NULL)
    {
        dmosi_mutex_destroy(g_servers_mutex);
        g_servers_mutex = NULL;
    }
}

dmod_dmdns_api_declaration(1.0, int, _add_server, ( const dmip_addr_t* server ))
{
    if (!is_usable_server(server))
        return -EINVAL;

    dmip_addr_t* entry = Dmod_Malloc(sizeof(*entry));
    if (entry == NULL)
        return -ENOMEM;
    *entry = *server;

    int result = 0;
    dmosi_mutex_lock(g_servers_mutex);
    if (dmlist_find(g_servers, server, compare_server) != NULL)
        result = -EEXIST;
    else if (!dmlist_push_back(g_servers, entry))
        result = -ENOMEM;
    dmosi_mutex_unlock(g_servers_mutex);

    if (result != 0)
        Dmod_Free(entry);
    return result;
}

dmod_dmdns_api_declaration(1.0, int, _remove_server, ( const dmip_addr_t* server ))
{
    if (server == NULL)
        return -EINVAL;

    dmosi_mutex_lock(g_servers_mutex);
    dmip_addr_t* entry = dmlist_find(g_servers, server, compare_server);
    if (entry != NULL)
        dmlist_remove(g_servers, entry, compare_server);
    dmosi_mutex_unlock(g_servers_mutex);

    if (entry == NULL)
        return -ENOENT;
    Dmod_Free(entry);
    return 0;
}

dmod_dmdns_api_declaration(1.0, void, _clear_servers, ( void ))
{
    dmosi_mutex_lock(g_servers_mutex);
    dmip_addr_t* entry;
    while ((entry = dmlist_pop_front(g_servers)) != NULL)
        Dmod_Free(entry);
    dmosi_mutex_unlock(g_servers_mutex);
}

/* Append `server` unless it is unusable, already in the set, or the set is full. */
static void add_unique(server_set_t* set, const dmip_addr_t* server)
{
    if (set->count >= set->max || !is_usable_server(server))
        return;

    for (size_t i = 0; i < set->count; i++)
    {
        if (dmdns_addr_equal(&set->out[i], server))
            return;
    }
    set->out[set->count++] = *server;
}

static void collect_static(server_set_t* set)
{
    dmosi_mutex_lock(g_servers_mutex);
    size_t size = dmlist_size(g_servers);
    for (size_t i = 0; i < size; i++)
        add_unique(set, dmlist_get(g_servers, i));
    dmosi_mutex_unlock(g_servers_mutex);
}

static void collect_provided(server_set_t* set)
{
    dmip_addr_t provided[DMDNS_MAX_SERVERS];
    Dmod_Context_t* module = Dmod_GetNextDifModule(dmod_dmdns_provide_servers_sig, NULL);

    while (module != NULL && set->count < set->max)
    {
        dmod_dmdns_provide_servers_t provide =
            (dmod_dmdns_provide_servers_t)Dmod_GetDifFunction(module, dmod_dmdns_provide_servers_sig);
        if (provide != NULL)
        {
            size_t count = provide(provided, DMDNS_MAX_SERVERS);
            for (size_t i = 0; i < count && i < DMDNS_MAX_SERVERS; i++)
                add_unique(set, &provided[i]);
        }
        module = Dmod_GetNextDifModule(dmod_dmdns_provide_servers_sig, module);
    }
}

dmod_dmdns_api_declaration(1.0, size_t, _get_servers, ( dmip_addr_t* out, size_t max ))
{
    if (out == NULL || max == 0u)
        return 0;

    server_set_t set = { out, max, 0 };
    collect_static(&set);
    collect_provided(&set);
    return set.count;
}
