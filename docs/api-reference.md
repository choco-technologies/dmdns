# dmdns API Reference

All functions are declared in [`include/dmdns.h`](../include/dmdns.h) and
return a negative errno on failure unless stated otherwise. Addresses are
`dmip_addr_t` (= `dmroute_addr_t`, discriminated by `family`).

## Constants

| Name | Value | Meaning |
|------|-------|---------|
| `DMDNS_PORT` | 53 | UDP port of name servers |
| `DMDNS_MAX_NAME_LEN` | 253 | Longest host name (without trailing dot) |
| `DMDNS_MAX_LABEL_LEN` | 63 | Longest label |
| `DMDNS_MAX_MESSAGE_LEN` | 512 | Largest UDP DNS message |
| `DMDNS_MAX_QUERY_LEN` | 271 | Buffer size always enough for `dmdns_build_query()` |
| `DMDNS_TYPE_A` / `_AAAA` / `_CNAME` | 1 / 28 / 5 | Record types |
| `DMDNS_CLASS_IN` | 1 | Internet class |
| `DMDNS_DEFAULT_TIMEOUT_MS` | 2000 | Per-server wait for `timeout_ms = 0` |
| `DMDNS_ATTEMPTS` | 2 | Rounds over the server list |
| `DMDNS_MAX_SERVERS` | 8 | Most servers `dmdns_get_servers()` reports |
| `DMDNS_MAX_ADDRESSES` | 8 | Most addresses cached per name |
| `DMDNS_CACHE_MAX_ENTRIES` | 16 | Cache capacity (oldest evicted first) |
| `DMDNS_CACHE_MAX_TTL_SEC` | 3600 | Upper bound of a cached answer's lifetime |
| `DMDNS_NEGATIVE_TTL_SEC` | 30 | Lifetime of a cached NXDOMAIN/NODATA |
| `DMDNS_ADDRESS_STRLEN` | 46 | Buffer always enough for `dmdns_format_address()` |

## Resolving

| Function | Description |
|----------|-------------|
| `int dmdns_resolve(const char* name, dmip_family_t family, dmip_addr_t* out, size_t max, uint32_t timeout_ms)` | Resolve a host name or IP literal. `family`: `dmip_family_v4`, `_v6`, or `_none` (A, then AAAA). Order: literal, `localhost`, hosts table, cache, servers. Returns the number of addresses, or `-EINVAL`, `-ENOENT` (no such name), `-ENODATA` (no address of that family), `-EDESTADDRREQ` (no server configured), or the last `dmdns_query()` error. **Blocks.** |
| `int dmdns_query(const dmip_addr_t* server, const char* name, uint16_t qtype, dmip_addr_t* out, size_t max, uint32_t* out_ttl_sec, uint32_t timeout_ms)` | One question to one server - no hosts table, no cache. Returns the number of addresses, `-ETIMEDOUT`, any `dmdns_parse_response()` error, or the `dmudp_bind_any()`/`dmudp_send()` error. **Blocks.** |

Blocking functions must not be called from a network receive callback
(dmudp handler, dmip/dmnetbridge DIF) - see [dmdns.md](dmdns.md#one-query-end-to-end).

## Name servers

| Function | Description |
|----------|-------------|
| `int dmdns_add_server(const dmip_addr_t* server)` | Add a static server (asked before provided ones). `-EINVAL`, `-EEXIST`, `-ENOMEM`. |
| `int dmdns_remove_server(const dmip_addr_t* server)` | Remove a static server. `-ENOENT` if absent. |
| `void dmdns_clear_servers(void)` | Remove every static server. |
| `size_t dmdns_get_servers(dmip_addr_t* out, size_t max)` | Servers a lookup would use now: static, then every provider's, without duplicates. |

### DIF `dmdns_provide_servers`

```c
dmod_dmdns_dif(1.0, size_t, _provide_servers, ( dmip_addr_t* out_servers, size_t max_servers ));
```

Implemented by any module that knows name servers (e.g. a DHCP client).
Called on every `dmdns_get_servers()`, from the resolving thread. Return the
number of addresses written. To implement it:

```c
#include "dmdns.h"

dmod_dmdns_dif_api_declaration(1.0, my_module, size_t, _provide_servers, ( dmip_addr_t* out, size_t max ))
{
    /* write up to `max` currently valid servers to `out` */
    return count;
}
```

with `set(DMOD_DIF_IMPLS dmdns)` and `dmod_link_modules(${DMOD_MODULE_NAME} dmdns)`
in the implementing module's `CMakeLists.txt`. Only Library-type (enabled)
modules are discovered.

## Hosts table

| Function | Description |
|----------|-------------|
| `int dmdns_add_host(const char* name, const dmip_addr_t* addr)` | Add a static mapping; a name may have several addresses. `-EINVAL`, `-EEXIST`, `-ENOMEM`. |
| `int dmdns_remove_host(const char* name)` | Remove every mapping of `name`. `-ENOENT` if none. |

`localhost` is built in (`127.0.0.1`, `::1`).

## Cache

| Function | Description |
|----------|-------------|
| `void dmdns_flush_cache(void)` | Forget all cached answers, positive and negative. |

## Codec

| Function | Description |
|----------|-------------|
| `bool dmdns_is_valid_name(const char* name)` | Labels of 1-63 `[A-Za-z0-9_-]`, total <= 253, one trailing dot allowed. |
| `int dmdns_build_query(uint8_t* buffer, size_t buffer_len, uint16_t id, const char* name, uint16_t qtype, size_t* out_len)` | Build a recursive (RD) query. `-EINVAL`, `-ENOBUFS`. |
| `int dmdns_parse_response(const uint8_t* message, size_t length, uint16_t id, const char* name, uint16_t qtype, dmip_addr_t* out, size_t max, size_t* out_count, uint32_t* out_ttl_sec)` | Validate an answer against the question and collect every `IN` record of `qtype`. Returns 0, `-ENOENT` (NXDOMAIN), `-ENODATA`, `-EAGAIN` (SERVFAIL), `-ECONNREFUSED` (REFUSED), `-EMSGSIZE` (truncated, empty), `-EPROTO` (malformed/foreign/FORMERR), `-EIO` (other RCODE), `-EINVAL`. |

## Address literals

| Function | Description |
|----------|-------------|
| `int dmdns_parse_address(const char* text, dmip_addr_t* out)` | Dotted-decimal IPv4 or IPv6 text form (with `::`). `-EINVAL` if not a literal. |
| `int dmdns_format_address(const dmip_addr_t* addr, char* buffer, size_t buffer_len)` | IPv4 dotted decimal or RFC 5952 IPv6. `-EINVAL`, `-ENOBUFS`. |
