# DMDNS - DMOD DNS stub resolver

## Overview

dmdns turns a host name (`google.com`) into IP addresses. It is a **stub
resolver** (RFC 1034 5.3.1): it never walks the DNS tree itself, it asks a
recursive name server - the one a DHCP lease offered, a public one like
`8.8.8.8`, the router - over UDP port 53 and trusts its answer. That is
the same split every desktop OS uses (`getaddrinfo()` -> `/etc/resolv.conf`
-> the ISP's resolver) and the only one that makes sense on a
microcontroller: a full recursive resolver needs a root hints file, TCP,
and far more RAM than any board in this tree has to spare.

```
                 nslookup, ping (future), any module needing a host name
                                   |
                         dmdns_resolve(name, family)
                                   |
┌──────────────────────────────────────────────────────────────────────┐
│                               DMDNS                                   │
│                                                                       │
│  1. IP literal ──> 2. "localhost" ──> 3. hosts table ──> 4. cache     │
│                                                              │ miss   │
│                                                              v        │
│  5. for every server (DMDNS_ATTEMPTS rounds):  dmdns_query()          │
│        │                                                              │
│        │  servers = static list (dmdns_add_server())                  │
│        │          + every dmdns_provide_servers DIF implementation    │
│        v                                                              │
│     codec: dmdns_build_query() / dmdns_parse_response()               │
└──────────────────────────────────────────────────────────────────────┘
          │ dmudp_bind_any() / dmudp_send()        ^ datagram handler
          v                                        │ (inline, RX thread)
┌──────────────────────────────────────────────────────────────────────┐
│              DMUDP  ->  DMIP  ->  DMNETBRIDGE  ->  DMNETIF            │
└──────────────────────────────────────────────────────────────────────┘
```

## Layers

The module is split so that every layer can be used - and tested - on its
own:

| Layer    | Functions | State | Network |
|----------|-----------|-------|---------|
| Codec    | `dmdns_is_valid_name()`, `_build_query()`, `_parse_response()` | none | no |
| Literals | `dmdns_parse_address()`, `_address_to_string()` | none | no |
| Servers  | `dmdns_add_server()`, `_remove_server()`, `_clear_servers()`, `_get_servers()`, DIF `dmdns_provide_servers` | static list | no |
| Hosts    | `dmdns_add_host()`, `_remove_host()` | table | no |
| Cache    | `dmdns_flush_cache()` (+ internal lookup/store) | table | no |
| Query    | `dmdns_query()` | pending-query table | yes |
| Resolver | `dmdns_resolve()` | - | via Query |

## Memory: allocate what is needed, when it is known

Nothing in dmdns is sized "just in case". Every message, address list,
server list and string is allocated with `Dmod_Malloc()` at exactly its
size once that size is known - no buffer is derived from a protocol
maximum (253-byte names, 512-byte UDP messages) and no thread's stack has
to grow to hold one:

- `dmdns_build_query()` computes the wire length of the name first and
  allocates exactly that;
- `dmdns_parse_response()` walks the answer section twice - count, then
  fill an array of exactly that many addresses;
- a received reply is copied at its real length;
- `dmdns_get_servers()`, the hosts table lookup and the server merge grow
  their result array one entry at a time (`Dmod_Realloc()`);
- `dmdns_address_to_string()` measures the text, then allocates it.

Returned results belong to the caller, who releases them with
`Dmod_Free()`. Bounds that remain - `DMDNS_MAX_NAME_LEN`,
`DMDNS_CACHE_MAX_ENTRIES` - are policy/validation limits, not buffers.

`dmdns_query()` is public on purpose: it asks *one* server *one* question,
bypassing hosts table and cache - exactly what `nslookup <name> <server>`
needs to check a server before configuring it.

## Where name servers come from

dmdns owns no network configuration. That is the one design decision
everything else follows from: on a DMOD system the name servers are known
by whoever configured the interface - today `dmdhcp` (option 6 of a lease,
already stored by `dmdhcp_get_dns_server()`), tomorrow maybe a PPP/cellular
modem driver or a static network config module. dmdns should not have to
know any of them.

So `dmdns_get_servers()` merges, on every call:

1. **Static servers** added with `dmdns_add_server()` - static config, a
   shell tool, a test. Asked first.
2. **Every loaded module implementing the `dmdns_provide_servers` DIF.**

```c
typedef int (*dmdns_server_sink_t)( void* sink_ctx, const dmip_addr_t* server );
dmod_dmdns_dif(1.0, void, _provide_servers, ( dmdns_server_sink_t add, void* sink_ctx ));
```

A provider calls `add` once per server it knows - there is no
caller-sized array to fill and no limit on how many it reports.

The DIF is the 1:N "many interchangeable backends discovered at runtime"
mechanism of DMOD, used exactly the way dmip uses it for protocol handlers:
providers are discovered with `Dmod_GetNextDifModule()` every time, never
registered or cached. Consequences:

- **No dependency in either direction at link time.** dmdns does not link
  dmdhcp, dmdhcp does not need to call into dmdns when a lease changes.
  A board without DHCP simply has no provider.
- **Always current.** A lease that expires stops being reported on the next
  lookup; a provider module that is unloaded or crashes stops being found.
  There is no stale copy of the server list anywhere.
- **No cycle.** The obvious alternative - dmdhcp calling
  `dmdns_add_server()` from its `on_bound` callback - would make dmdhcp
  depend on dmdns and leave dmdns holding servers of a lease that has since
  expired unless dmdhcp also remembers to remove them on every path.

A provider in dmdhcp is a few lines (sketch, not part of this module):

```c
dmod_dmdns_dif_api_declaration(1.0, dmdhcp, void, _provide_servers, ( dmdns_server_sink_t add, void* sink_ctx ))
{
    /* for every lease in BOUND/RENEWING/REBINDING:                   */
    /*     for i < dmdhcp_get_dns_server_count(lease):                */
    /*         dmdhcp_get_dns_server(lease, i, &server);              */
    /*         add(sink_ctx, &server);                                */
}
```

plus `set(DMOD_DIF_IMPLS dmdns)` and `dmod_link_modules(... dmdns)` for the
header. dmdns' own tests use exactly this shape through the
`dmdns_test_provider` fixture module (`tests/fixtures/`).

## One query, end to end

`dmdns_query()`:

1. allocates a pending query with a random 16-bit ID (xorshift re-seeded
   from the tick counter) and a semaphore,
2. binds an ephemeral UDP port for it (`dmudp_bind_any()`) and publishes it
   in the pending table keyed by that port,
3. builds the question (`dmdns_build_query()`, RD set, allocated at its
   exact length) and sends it (`dmudp_send()`),
4. waits on the semaphore for at most `timeout_ms`,
5. unpublishes the query, unbinds the port, and parses whatever arrived.

dmudp delivers datagrams by calling the bound handler **inline, on the
thread pumping the interface** (see dmudp.md). The handler therefore does
the minimum: find the query by destination port, check the reply comes
from the server we asked, from port 53, with our ID, copy it and post the
semaphore. Parsing happens on the asking thread. The table and the handler
share one mutex and a query is unpublished before it is freed, so a late
or duplicate reply can never touch freed memory.

One port per query (rather than one shared socket) keeps concurrent
lookups from different threads independent and gives a little source-port
randomisation on top of the ID (RFC 5452) - `dmudp_bind_any()` hands out
the first free ephemeral port, so it is not strong randomisation.

**Never call `dmdns_query()`/`dmdns_resolve()` from a receive callback**
(a dmudp handler, a dmip DIF): the answer would have to be delivered by the
very thread that is blocked waiting for it, and the lookup always times
out.

## Accepting an answer

`dmdns_parse_response()` accepts a message only if it really answers our
question:

- ID equals the one sent, QR set, OPCODE 0;
- exactly one question, whose name (case-insensitive, compression pointers
  followed), type and class repeat ours - a reply to some other question is
  rejected as `-EPROTO` even when it comes from the right address;
- every record is bounds-checked; compression pointers are limited to 16
  jumps so a pointer loop cannot hang the parser.

Every `IN` record of the requested type in the answer section is
collected, whatever its owner name. A recursive server answers
`www.github.com` with `www.github.com CNAME github.com` followed by
`github.com A 140.82.113.4`; collecting by type yields the final address
without re-implementing CNAME chasing.

RCODEs map to errno values so callers can tell "this name does not exist"
from "this server is broken":

| Answer | Result | Resolver |
|--------|--------|----------|
| addresses | 0 + address array | done, cached for min TTL |
| NXDOMAIN | `-ENOENT` | done, cached for `DMDNS_NEGATIVE_TTL_SEC` |
| NOERROR, no record of the type | `-ENODATA` | done, cached; `dmip_family_none` falls back to AAAA |
| SERVFAIL | `-EAGAIN` | next server |
| REFUSED | `-ECONNREFUSED` | next server |
| truncated, no address | `-EMSGSIZE` | next server |
| malformed / foreign / FORMERR | `-EPROTO` | next server |
| no reply | `-ETIMEDOUT` | next server |

## Cache

Keyed by (name, record type), case-insensitive. Positive answers live for
the smallest TTL of their records, capped at `DMDNS_CACHE_MAX_TTL_SEC`;
negative ones for `DMDNS_NEGATIVE_TTL_SEC` (a simplification of RFC 2308,
which would take it from the SOA record). At most
`DMDNS_CACHE_MAX_ENTRIES` names are kept and the oldest is evicted first.
Expiry is checked lazily on lookup - no timer. Transient failures
(timeouts, SERVFAIL) are never cached.

## Limits

- **UDP only.** No TCP fallback and no EDNS0, so servers answer with at
  most 512 bytes (RFC 1035) - that is the server's limit, dmdns reserves
  nothing for it. A
  truncated answer is still used when it carries an address, which is
  practically always the case for A/AAAA lookups.
- **IPv4 transport.** AAAA records are looked up and IPv6 addresses parse
  and format, but `dmudp_send()` has no IPv6 path yet, so an IPv6 *server*
  returns `-ENOSYS` and is skipped.
- **No DNSSEC, no search domains.** A name is always looked up exactly as
  given (an unqualified `printer` is not expanded to `printer.lan`).
- **Tick counter as clock.** Cache expiry uses `dmosi_get_tick_count()`
  (1 ms per tick on every dmosi port in this tree) and is wrap-around safe.

## Module runtime pitfall: DIF signature placement

`ENABLE_DIF_REGISTRATIONS` defines the DIF signature as a
`const char* const` initialised with a string literal. The dmod loader
relocates a module's GOT but not pointers stored in its `.data`, so that
variable must be defined in the translation unit that uses it
(`dmdns_servers.c`) - there the compiler folds it into a PC-relative
address of the literal. Defined elsewhere and read through an `extern`, it
holds an unrelocated address and `Dmod_GetNextDifModule()` crashes. dmip
and dmnetbridge follow the same rule. For the same reason the tests avoid
static tables of string pointers.

## Dependencies

- `dmudp` - `dmudp_bind_any()`/`_unbind()`/`_send()`
- `dmip` - `dmip_addr_t`/`dmip_family_t` (header)
- `dmroute`, `dmnetif` - the header chain of dmip.h/dmudp.h
- `dmlist` - server list, hosts table, cache, pending-query table
- `dmosi` - mutexes, the per-query semaphore, tick counter
