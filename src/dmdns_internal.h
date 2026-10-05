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
 * @return Number of addresses of `qtype` written to `out` (0 if none)
 */
size_t dmdns_hosts_lookup(const char* name, uint16_t qtype, dmip_addr_t* out, size_t max);

/**
 * @brief Look a cached answer up
 *
 * @return > 0 number of cached addresses written to `out`, a negative
 *         errno for a cached negative answer (-ENOENT/-ENODATA), or 0 if
 *         nothing (still valid) is cached for this name and type
 */
int dmdns_cache_lookup(const char* name, uint16_t qtype, dmip_addr_t* out, size_t max);

/**
 * @brief Remember an answer for `ttl_sec` seconds
 *
 * @param result > 0 for `count` addresses in `addrs`, or a negative errno
 *               (-ENOENT/-ENODATA) for a negative answer
 */
void dmdns_cache_store(const char* name, uint16_t qtype, int result, const dmip_addr_t* addrs, uint32_t ttl_sec);

#endif // DMDNS_INTERNAL_H
