#ifndef DMDNS_H
#define DMDNS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmip.h"
#include "dmdns_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmdns.h
 * @brief DMOD DNS stub resolver - Public API
 *
 * dmdns turns a host name ("google.com") into IP addresses. It is a *stub*
 * resolver (RFC 1034 5.3.1): it never recurses itself, it asks a recursive
 * name server (the one a DHCP lease offered, 8.8.8.8, a router...) over UDP
 * port 53 and trusts its answer.
 *
 * The module is split into layers, each usable on its own:
 *
 *  - Codec       dmdns_build_query()/_parse_response() - pure functions over
 *                byte buffers (RFC 1035 4.1), no network, no state.
 *  - Query       dmdns_query() - one question to one server: binds an
 *                ephemeral UDP port (dmudp_bind_any()), sends, waits for
 *                the matching answer.
 *  - Servers     dmdns_add_server()/... plus the dmdns_provide_servers DIF -
 *                where the name servers come from (see below).
 *  - Hosts       dmdns_add_host()/... - a static name table (/etc/hosts).
 *  - Resolver    dmdns_resolve() - the call most users want: IP literal ->
 *                "localhost" -> hosts table -> cache -> every name server.
 *
 * Where name servers come from
 * ----------------------------
 * dmdns owns no network configuration of its own. dmdns_get_servers()
 * merges two sources, every time it is called:
 *
 *  1. Static servers added with dmdns_add_server() (static config, a shell
 *     tool, a test).
 *  2. Every loaded module implementing the dmdns_provide_servers DIF
 *     (below) - e.g. a DHCP client reporting the option 6 servers of its
 *     current leases. The provider is discovered at query time, so neither
 *     side depends on the other at link time and a provider that is
 *     unloaded simply stops being asked.
 *
 * Threading
 * ---------
 * dmdns_query()/_resolve() block the calling thread until an answer or a
 * timeout. The answer is delivered by dmudp on the thread pumping the
 * network interface (see dmudp.h), so they must NOT be called from that
 * thread - i.e. never from inside a dmudp/dmip/dmnetbridge receive
 * callback - or they will always time out. All other functions are
 * non-blocking and thread-safe.
 *
 * Memory
 * ------
 * Nothing is reserved up front: every message, address list, server list
 * and string is allocated with Dmod_Malloc() at exactly the size it needs
 * once that size is known. Functions returning such a result say so - the
 * caller releases it with Dmod_Free().
 *
 * Limits
 * ------
 * Plain UDP DNS only (no TCP fallback, no EDNS0, no DNSSEC): a server
 * answers with at most 512 bytes (RFC 1035 4.2.1). A truncated answer (TC bit) is
 * still used if it carries at least one address. IPv6 (AAAA) records can
 * be looked up, but the question itself is sent to an IPv4 server only
 * until dmudp_send() gains an IPv6 path.
 */

/* ============================================================================
 *                                  Constants
 * ========================================================================== */

/** @brief UDP port name servers listen on */
#define DMDNS_PORT                53u

/**
 * @brief Longest valid host name, without the trailing dot (RFC 1035 2.3.4)
 *
 * A protocol limit checked by dmdns_is_valid_name() - no buffer is ever
 * sized from it.
 */
#define DMDNS_MAX_NAME_LEN        253u

/** @brief Longest valid label (RFC 1035 2.3.4) - checked by dmdns_is_valid_name() */
#define DMDNS_MAX_LABEL_LEN       63u

/** @brief Length of the fixed DNS message header (RFC 1035 4.1.1) */
#define DMDNS_HEADER_LEN          12u

/** @brief Resource record types (RFC 1035 3.2.2, RFC 3596) */
#define DMDNS_TYPE_A              1u
#define DMDNS_TYPE_CNAME          5u
#define DMDNS_TYPE_AAAA           28u

/** @brief The Internet class (RFC 1035 3.2.4) */
#define DMDNS_CLASS_IN            1u

/** @brief Per-server wait used when a caller passes timeout_ms = 0 */
#define DMDNS_DEFAULT_TIMEOUT_MS  2000u

/** @brief How many times dmdns_resolve() walks the whole server list before giving up */
#define DMDNS_ATTEMPTS            2u

/** @brief Most names kept in the answer cache - the oldest entry is evicted first */
#define DMDNS_CACHE_MAX_ENTRIES   16u

/** @brief Upper bound for a cached answer's lifetime, whatever TTL the server gave */
#define DMDNS_CACHE_MAX_TTL_SEC   3600u

/** @brief How long a "no such name" / "no such record" answer is cached (RFC 2308, simplified) */
#define DMDNS_NEGATIVE_TTL_SEC    30u

/* ============================================================================
 *                                  Codec
 * ========================================================================== */

/**
 * @brief Check whether `name` is a syntactically valid host name
 *
 * Labels of 1..DMDNS_MAX_LABEL_LEN letters, digits, '-' or '_' separated by
 * single dots, at most DMDNS_MAX_NAME_LEN characters in total. One trailing
 * dot (a fully qualified "example.com.") is accepted.
 */
dmod_dmdns_api(1.0, bool, _is_valid_name, ( const char* name ));

/**
 * @brief Build a standard recursive query (RD set) for one question
 *
 * @param id          Transaction ID to put in the header
 * @param name        Host name to ask about (see dmdns_is_valid_name())
 * @param qtype       DMDNS_TYPE_A or DMDNS_TYPE_AAAA (any type is encoded as given)
 * @param out_message Output: the message, allocated at exactly its size -
 *                     release with Dmod_Free()
 * @param out_len     Output: its length in bytes
 *
 * @return 0 on success, -EINVAL on a NULL argument or invalid name,
 *         -ENOMEM on allocation failure
 */
dmod_dmdns_api(1.0, int, _build_query, ( uint16_t id, const char* name, uint16_t qtype, uint8_t** out_message, size_t* out_len ));

/**
 * @brief Parse the answer to a query built by dmdns_build_query()
 *
 * The response is only accepted if it really answers *our* question: the
 * ID must be `id`, QR must be set, and the question section must repeat
 * `name`/`qtype`/IN (case-insensitively) - anything else is treated as
 * spoofed or stray traffic. Every answer record of type `qtype` in class IN
 * is collected, whatever its owner name, so a CNAME chain the server
 * already followed ("www.example.com CNAME example.com", "example.com A
 * ...") yields the final addresses.
 *
 * @param message     Received bytes
 * @param length      Number of bytes in `message`
 * @param id          Transaction ID the query was sent with
 * @param name        Name the query asked about
 * @param qtype       Type the query asked about (DMDNS_TYPE_A/_AAAA)
 * @param out_addrs   Output: every address found, allocated at exactly
 *                     `*out_count` entries - release with Dmod_Free().
 *                     Set to NULL on any error.
 * @param out_count   Output: number of addresses
 * @param out_ttl_sec Output (may be NULL): smallest TTL among them, or
 *                     DMDNS_NEGATIVE_TTL_SEC for a negative answer
 *
 * @return 0 if at least one address was found,
 *         -ENOENT       the name does not exist (NXDOMAIN),
 *         -ENODATA      the name exists but has no record of this type,
 *         -EAGAIN       the server failed (SERVFAIL) - try another one,
 *         -ECONNREFUSED the server refused to answer (REFUSED),
 *         -EMSGSIZE     truncated answer without a single usable address,
 *         -EPROTO       malformed message, wrong ID, question mismatch
 *                       or FORMERR,
 *         -EIO          any other server error (NOTIMP, ...),
 *         -ENOMEM       allocation failure,
 *         -EINVAL       a NULL argument
 */
dmod_dmdns_api(1.0, int, _parse_response, ( const uint8_t* message, size_t length, uint16_t id, const char* name, uint16_t qtype,
    dmip_addr_t** out_addrs, size_t* out_count, uint32_t* out_ttl_sec ));

/* ============================================================================
 *                              Address literals
 * ========================================================================== */

/**
 * @brief Parse an IP address literal - dotted decimal IPv4 ("192.0.2.1")
 *        or IPv6 text form ("2001:db8::1", RFC 4291 2.2 forms 1 and 2)
 *
 * @return 0 on success, -EINVAL if `text` is not an address literal
 */
dmod_dmdns_api(1.0, int, _parse_address, ( const char* text, dmip_addr_t* out ));

/**
 * @brief Format an address as text - dotted decimal for IPv4, the RFC 5952
 *        canonical form ("2001:db8::1") for IPv6
 *
 * @return The text, allocated at exactly its length + 1 - release with
 *         Dmod_Free() - or NULL on a NULL/family-less address or
 *         allocation failure
 */
dmod_dmdns_api(1.0, char*, _address_to_string, ( const dmip_addr_t* addr ));

/* ============================================================================
 *                                Name servers
 * ========================================================================== */

/**
 * @brief Callback a dmdns_provide_servers implementation calls once per
 *        name server it knows
 *
 * @param sink_ctx As passed to dmdns_provide_servers()
 * @param server   One server address (copied - need not outlive the call)
 *
 * @return 0, or a negative errno (e.g. -ENOMEM) - a provider may stop
 *         reporting on an error but does not have to
 */
typedef int (*dmdns_server_sink_t)( void* sink_ctx, const dmip_addr_t* server );

/**
 * @brief DIF implemented by a module that knows name servers - e.g. a DHCP
 *        client reporting the servers its leases offered (option 6)
 *
 * Asked fresh on every dmdns_get_servers() call (so on every resolution
 * that goes to the network), never cached: report what is valid right now,
 * or nothing. Call `add` once per server - there is no limit on how many.
 * Called from the resolving thread, never from a receive callback, so the
 * implementation may take its own locks (but must not call back into
 * dmdns_get_servers()).
 *
 * @param add      Callback to report one server with
 * @param sink_ctx Opaque context to pass back to `add`
 */
dmod_dmdns_dif(1.0, void, _provide_servers, ( dmdns_server_sink_t add, void* sink_ctx ));

/**
 * @brief Add a static name server, asked before any provided by the DIF
 *
 * @return 0 on success, -EINVAL on a NULL/family-less address, -EEXIST if
 *         it is already on the list, -ENOMEM on allocation failure
 */
dmod_dmdns_api(1.0, int, _add_server, ( const dmip_addr_t* server ));

/** @brief Remove a static name server. Returns 0, or -ENOENT if it was not on the list. */
dmod_dmdns_api(1.0, int, _remove_server, ( const dmip_addr_t* server ));

/** @brief Remove every static name server (servers provided through the DIF are unaffected). */
dmod_dmdns_api(1.0, void, _clear_servers, ( void ));

/**
 * @brief The name servers dmdns_resolve() would ask right now, in order:
 *        static servers first, then every DIF provider's, duplicates removed
 *
 * @param out_servers Output: the servers, allocated at exactly `*out_count`
 *                     entries - release with Dmod_Free(); NULL if there are none
 * @param out_count   Output: number of servers
 *
 * @return 0 on success, -EINVAL on a NULL argument, -ENOMEM on allocation failure
 */
dmod_dmdns_api(1.0, int, _get_servers, ( dmip_addr_t** out_servers, size_t* out_count ));

/* ============================================================================
 *                                Hosts table
 * ========================================================================== */

/**
 * @brief Add a static name -> address mapping, consulted before any server
 *
 * A name may be given several addresses (one call each, IPv4 and/or IPv6).
 * "localhost" is built in (127.0.0.1 and ::1) and needs no entry.
 *
 * @return 0 on success, -EINVAL on an invalid name/address, -EEXIST if this
 *         exact mapping already exists, -ENOMEM on allocation failure
 */
dmod_dmdns_api(1.0, int, _add_host, ( const char* name, const dmip_addr_t* addr ));

/** @brief Remove every mapping of `name`. Returns 0, or -ENOENT if there was none. */
dmod_dmdns_api(1.0, int, _remove_host, ( const char* name ));

/* ============================================================================
 *                                  Cache
 * ========================================================================== */

/** @brief Forget every cached answer, positive and negative */
dmod_dmdns_api(1.0, void, _flush_cache, ( void ));

/* ============================================================================
 *                                 Resolving
 * ========================================================================== */

/**
 * @brief Ask one name server one question - no hosts table, no cache
 *
 * Binds an ephemeral UDP port, sends one query with a random ID and waits
 * up to `timeout_ms` for the matching answer. Blocks: see "Threading" in
 * this file's top comment.
 *
 * @param server      Name server to ask
 * @param name        Host name to look up
 * @param qtype       DMDNS_TYPE_A or DMDNS_TYPE_AAAA
 * @param out_addrs   Output: every address in the answer, allocated at
 *                     exactly `*out_count` entries - release with Dmod_Free()
 * @param out_count   Output: number of addresses
 * @param out_ttl_sec Output (may be NULL): TTL of the answer
 * @param timeout_ms  How long to wait for the answer, 0 = DMDNS_DEFAULT_TIMEOUT_MS
 *
 * @return 0 on success, -ETIMEDOUT if no answer arrived in time, any error
 *         of dmdns_parse_response() for a negative/bad answer, or the error
 *         of dmudp_bind_any()/dmudp_send() (e.g. -ENETUNREACH) if the query
 *         could not be sent at all
 */
dmod_dmdns_api(1.0, int, _query, ( const dmip_addr_t* server, const char* name, uint16_t qtype,
    dmip_addr_t** out_addrs, size_t* out_count, uint32_t* out_ttl_sec, uint32_t timeout_ms ));

/**
 * @brief Resolve a host name to addresses - the gethostbyname() of dmod
 *
 * In order, the first step that knows the answer wins:
 *  1. `name` is an IP literal -> that address (no lookup at all)
 *  2. "localhost"             -> 127.0.0.1 / ::1
 *  3. the hosts table         (dmdns_add_host())
 *  4. the answer cache        (positive and negative answers, by TTL)
 *  5. every name server       (dmdns_get_servers()), DMDNS_ATTEMPTS rounds;
 *                              a definitive answer (addresses, NXDOMAIN,
 *                              NODATA) stops the walk and is cached
 *
 * @param name       Host name or IP literal
 * @param family     dmip_family_v4 (A), dmip_family_v6 (AAAA) or
 *                    dmip_family_none for "any": A first, AAAA if the name
 *                    has no A record
 * @param out_addrs  Output: the addresses, allocated at exactly `*out_count`
 *                    entries - release with Dmod_Free(); NULL on error
 * @param out_count  Output: number of addresses (> 0 on success)
 * @param timeout_ms Wait per server attempt, 0 = DMDNS_DEFAULT_TIMEOUT_MS
 *
 * @return 0 on success, or
 *         -EINVAL        invalid name/argument,
 *         -ENOENT        no such name,
 *         -ENODATA       the name has no address of the requested family,
 *         -EDESTADDRREQ  no name server is configured,
 *         -ENOMEM        allocation failure,
 *         otherwise the last error dmdns_query() returned (-ETIMEDOUT, ...)
 */
dmod_dmdns_api(1.0, int, _resolve, ( const char* name, dmip_family_t family, dmip_addr_t** out_addrs, size_t* out_count, uint32_t timeout_ms ));

#ifdef __cplusplus
}
#endif

#endif // DMDNS_H
