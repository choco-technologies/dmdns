/**
 * @file dmdns_query.c
 * @brief One question to one name server over UDP
 *
 * Every query binds its own ephemeral port (dmudp_bind_any()) for its
 * whole lifetime and is registered in a small "pending" table keyed by
 * that port. dmudp calls receive_handler() inline, on the thread pumping
 * the interface (see dmudp.h) - the handler only copies the payload out
 * and posts the query's semaphore; the asking thread, blocked in
 * dmosi_semaphore_wait(), does the actual parsing. That keeps the receive
 * path short and lets dmdns_query() be called from any number of threads
 * at once.
 *
 * A reply is only handed over if it comes from the server we asked, from
 * port 53, to the port we bound, with the ID we sent (RFC 5452 basics);
 * dmdns_parse_response() then also checks that it repeats our question.
 * The first matching reply wins - later duplicates are ignored.
 *
 * Lifetime: a query is unlinked from the table (under g_queries_mutex)
 * before it is freed, and the handler only touches it while holding the
 * same mutex, so a late reply can never reach a freed query.
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include "dmudp.h"
#include <string.h>
#include <errno.h>

#define DMDNS_QUERY_MAGIC 0x444E5351u /* "DNSQ" */

typedef struct
{
    uint32_t          magic;
    uint16_t          id;
    uint16_t          port;
    dmip_addr_t       server;
    dmosi_semaphore_t answered;
    uint8_t*          response;
    size_t            response_len;
} pending_query_t;

static dmlist_context_t* g_queries = NULL;
static dmosi_mutex_t     g_queries_mutex = NULL;
static uint32_t          g_id_state = 0;

/* xorshift32, re-seeded with the tick count on every call so IDs stay unpredictable across reboots. */
static uint16_t next_query_id(void)
{
    dmosi_mutex_lock(g_queries_mutex);
    g_id_state ^= dmosi_get_tick_count() * 2654435761u;
    if (g_id_state == 0u)
        g_id_state = 0x9E3779B9u;
    g_id_state ^= g_id_state << 13;
    g_id_state ^= g_id_state >> 17;
    g_id_state ^= g_id_state << 5;
    uint16_t id = (uint16_t)(g_id_state ^ (g_id_state >> 16));
    dmosi_mutex_unlock(g_queries_mutex);
    return id;
}

/* Caller holds g_queries_mutex. */
static pending_query_t* find_by_port(uint16_t port)
{
    size_t size = dmlist_size(g_queries);
    for (size_t i = 0; i < size; i++)
    {
        pending_query_t* query = dmlist_get(g_queries, i);
        if (query->magic == DMDNS_QUERY_MAGIC && query->port == port)
            return query;
    }
    return NULL;
}

static bool reply_matches(const pending_query_t* query, const dmip_addr_t* src, uint16_t src_port, const uint8_t* payload, size_t payload_len)
{
    return query->response == NULL
        && src_port == DMDNS_PORT
        && dmdns_addr_equal(src, &query->server)
        && payload_len >= DMDNS_HEADER_LEN
        && (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]) == query->id;
}

static void receive_handler(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface,
    const uint8_t* payload, size_t payload_len)
{
    (void)iface;

    dmosi_mutex_lock(g_queries_mutex);
    pending_query_t* query = find_by_port(dst_port);
    if (query != NULL && reply_matches(query, src, src_port, payload, payload_len))
    {
        query->response = Dmod_Malloc(payload_len);
        if (query->response != NULL)
        {
            memcpy(query->response, payload, payload_len);
            query->response_len = payload_len;
            dmosi_semaphore_post(query->answered, 1);
        }
    }
    dmosi_mutex_unlock(g_queries_mutex);
}

int dmdns_query_init(void)
{
    g_queries = dmlist_create();
    g_queries_mutex = dmosi_mutex_create(false);
    g_id_state = dmosi_get_tick_count();
    return (g_queries != NULL && g_queries_mutex != NULL) ? 0 : -ENOMEM;
}

void dmdns_query_deinit(void)
{
    if (g_queries != NULL)
    {
        dmlist_destroy(g_queries);
        g_queries = NULL;
    }
    if (g_queries_mutex != NULL)
    {
        dmosi_mutex_destroy(g_queries_mutex);
        g_queries_mutex = NULL;
    }
}

static void destroy_query(pending_query_t* query)
{
    query->magic = 0;
    if (query->answered != NULL)
        dmosi_semaphore_destroy(query->answered);
    Dmod_Free(query->response);
    Dmod_Free(query);
}

/* Allocate a query, bind its port and publish it in the pending table. */
static int open_query(const dmip_addr_t* server, pending_query_t** out_query)
{
    pending_query_t* query = Dmod_Malloc(sizeof(*query));
    if (query == NULL)
        return -ENOMEM;
    memset(query, 0, sizeof(*query));
    query->magic    = DMDNS_QUERY_MAGIC;
    query->id       = next_query_id();
    query->server   = *server;
    query->answered = dmosi_semaphore_create(0, 1);
    if (query->answered == NULL)
    {
        destroy_query(query);
        return -ENOMEM;
    }

    dmosi_mutex_lock(g_queries_mutex);
    int result = dmudp_bind_any(receive_handler, &query->port);
    if (result == 0 && !dmlist_push_back(g_queries, query))
    {
        dmudp_unbind(query->port);
        result = -ENOMEM;
    }
    dmosi_mutex_unlock(g_queries_mutex);

    if (result != 0)
    {
        destroy_query(query);
        return result;
    }
    *out_query = query;
    return 0;
}

static int compare_pointer(const void* entry, const void* key)
{
    return (entry == key) ? 0 : 1;
}

static void close_query(pending_query_t* query)
{
    dmosi_mutex_lock(g_queries_mutex);
    dmlist_remove(g_queries, query, compare_pointer);
    dmudp_unbind(query->port);
    dmosi_mutex_unlock(g_queries_mutex);
    destroy_query(query);
}

/* Send the question and wait for the answer; parse it into a freshly allocated address array. */
static int exchange(pending_query_t* query, const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count,
    uint32_t* out_ttl_sec, uint32_t timeout_ms)
{
    uint8_t* message = NULL;
    size_t message_len = 0;
    int result = dmdns_build_query(query->id, name, qtype, &message, &message_len);
    if (result != 0)
        return result;

    result = dmudp_send(&query->server, query->port, DMDNS_PORT, message, message_len, timeout_ms);
    Dmod_Free(message);
    if (result != 0)
        return result;
    if (dmosi_semaphore_wait(query->answered, 1, (int32_t)timeout_ms) != 0)
        return -ETIMEDOUT;

    return dmdns_parse_response(query->response, query->response_len, query->id, name, qtype, out_addrs, out_count, out_ttl_sec);
}

dmod_dmdns_api_declaration(1.0, int, _query, ( const dmip_addr_t* server, const char* name, uint16_t qtype,
    dmip_addr_t** out_addrs, size_t* out_count, uint32_t* out_ttl_sec, uint32_t timeout_ms ))
{
    if (server == NULL || out_addrs == NULL || out_count == NULL || !dmdns_is_valid_name(name))
        return -EINVAL;
    *out_addrs = NULL;
    *out_count = 0;
    if (server->family != dmip_family_v4 && server->family != dmip_family_v6)
        return -EINVAL;
    if (timeout_ms == 0u)
        timeout_ms = DMDNS_DEFAULT_TIMEOUT_MS;

    pending_query_t* query = NULL;
    int result = open_query(server, &query);
    if (result != 0)
        return result;

    result = exchange(query, name, qtype, out_addrs, out_count, out_ttl_sec, timeout_ms);
    close_query(query);
    return result;
}
