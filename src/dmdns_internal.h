#ifndef DMDNS_INTERNAL_H
#define DMDNS_INTERNAL_H

/**
 * @file dmdns_internal.h
 * @brief Shared declarations between dmdns's translation units
 *
 * File map:
 *  - dmdns_registrations.c  Built-in API registration (DMOD_ENABLE_REGISTRATION)
 *  - dmdns.c                dmod_init()/_deinit(), dmdns_resolve()
 *  - dmdns_codec.c          name validation, query build, response parse
 *  - dmdns_address.c        IP literal parse/format
 *  - dmdns_servers.c        static server list + dmdns_provide_servers DIF
 *  - dmdns_hosts.c          static hosts table, "localhost"
 *  - dmdns_cache.c          TTL answer cache
 *  - dmdns_query.c          one query over UDP (dmudp)
 */
#include "dmdns.h"
#include "dmosi.h"
#include "dmlist.h"

/** @brief Equality of two addresses (same family and same bytes) */
bool dmdns_addr_equal(const dmip_addr_t* a, const dmip_addr_t* b);

/**
 * @brief Grow a Dmod_Malloc'd address array by one entry (Dmod_Realloc)
 *
 * `*list` may be NULL with `*count` 0 to start a new array. On failure the
 * array is left untouched (still owned by the caller).
 *
 * @return 0, or -ENOMEM
 */
int dmdns_addr_append(dmip_addr_t** list, size_t* count, const dmip_addr_t* addr);

/** @brief Copy `count` addresses into a new array of exactly that size. Returns NULL on allocation failure. */
dmip_addr_t* dmdns_addr_copy(const dmip_addr_t* addrs, size_t count);

/** @brief Case-insensitive equality of two host names, ignoring one trailing dot on either */
bool dmdns_name_equal(const char* a, const char* b);

/** @brief Map an address family to the record type that holds it (A/AAAA), 0 for none */
uint16_t dmdns_family_to_qtype(dmip_family_t family);

/* Lifecycle of each stateful part - called from dmod_init()/_deinit() */
int  dmdns_servers_init(void);
void dmdns_servers_deinit(void);
int  dmdns_hosts_init(void);
void dmdns_hosts_deinit(void);
int  dmdns_cache_init(void);
void dmdns_cache_deinit(void);
int  dmdns_query_init(void);
void dmdns_query_deinit(void);

/**
 * @brief Look `name` up in "localhost" and the hosts table
 *
 * @return 0 with `*out_addrs` (Dmod_Malloc'd, `*out_count` entries) set,
 *         -ENOENT if there is no entry of `qtype`, or -ENOMEM
 */
int dmdns_hosts_lookup(const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count);

/**
 * @brief Look a cached answer up
 *
 * @return 1 with `*out_addrs` (Dmod_Malloc'd copy, `*out_count` entries)
 *         set for a cached positive answer, a negative errno for a cached
 *         negative answer (-ENOENT/-ENODATA) or an allocation failure
 *         (-ENOMEM), or 0 if nothing (still valid) is cached
 */
int dmdns_cache_lookup(const char* name, uint16_t qtype, dmip_addr_t** out_addrs, size_t* out_count);

/**
 * @brief Remember an answer for `ttl_sec` seconds
 *
 * @param result 0 for `count` addresses in `addrs` (copied), or a negative
 *               errno (-ENOENT/-ENODATA) for a negative answer
 */
void dmdns_cache_store(const char* name, uint16_t qtype, int result, const dmip_addr_t* addrs, size_t count, uint32_t ttl_sec);

#endif // DMDNS_INTERNAL_H
