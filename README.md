# dmdns

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmdns/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmdns/actions/workflows/ci.yml)

DNS stub resolver for the DMOD network stack - turns `google.com` into an
IP address.

## Description

dmdns sits on top of [dmudp](https://github.com/choco-technologies/dmudp)
and answers the question every network client eventually asks: *what is the
address of this host?* It is a stub resolver (RFC 1034 5.3.1): it sends the
question to a recursive name server over UDP port 53 and trusts the answer.

```
          nslookup / any module that has a host name
                            |
                    dmdns_resolve()
                            |
   IP literal -> localhost -> hosts table -> cache -> name servers
                                                         |
                       static (dmdns_add_server())  +  every module
                                                       implementing the
                                                       dmdns_provide_servers
                                                       DIF (e.g. DHCP)
                            |
                 dmudp -> dmip -> dmnetbridge -> dmnetif
```

Highlights:

- **Layered and testable** - a pure codec (`dmdns_build_query()` /
  `_parse_response()`), address literal helpers, server list, hosts table,
  TTL cache, one-shot `dmdns_query()` and the `dmdns_resolve()` orchestrator.
- **Name servers through a DIF** - dmdns owns no network configuration.
  Any module that knows name servers (a DHCP client, a modem driver, static
  config) implements `dmdns_provide_servers` and is discovered at lookup
  time: no link-time dependency in either direction, never a stale list.
- **Safe answer handling** - random query IDs, one ephemeral port per
  query, replies checked against server address, port, ID and the question
  itself; compression pointers bounded; CNAME chains handled.
- **Cache** with positive (TTL) and negative (NXDOMAIN/NODATA) entries.
- **`nslookup`** CLI tool in [`tools/nslookup`](tools/nslookup/README.md).

See [docs/dmdns.md](docs/dmdns.md) for the full design.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

This builds `dmdns`, the `nslookup` tool, and their test modules.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/` and
`tools/nslookup/tests/`). Once built, run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmdns-local.dmd -y
dmod_loader build/dmf/test_dmdns.dmf
```

The codec is tested on hand-built messages and on real answers captured
from `8.8.8.8`; provider discovery through the `dmdns_test_provider`
fixture module (`tests/fixtures/`). No name server is reachable from
`dmod_loader`, so the network path is only tested for honest failures.

## Usage

```c
#include "dmdns.h"

/* Optional - normally a DHCP client provides servers through the DIF */
dmip_addr_t server;
dmdns_parse_address("8.8.8.8", &server);
dmdns_add_server(&server);

dmip_addr_t* addrs = NULL;     /* allocated by dmdns at exactly the right size */
size_t count = 0;
int result = dmdns_resolve("google.com", dmip_family_none, &addrs, &count, 0);
if (result == 0)
{
    char* text = dmdns_address_to_string(&addrs[0]);
    Dmod_Printf("google.com -> %s (%u addresses)\n", text, (unsigned)count);
    Dmod_Free(text);
    Dmod_Free(addrs);
}
else
{
    Dmod_Printf("lookup failed: %d\n", result);   /* -ENOENT, -ETIMEDOUT, ... */
}
```

Nothing is reserved up front: messages, address lists, server lists and
strings are allocated with `Dmod_Malloc()` at exactly the size they need,
and returned results are released by the caller with `Dmod_Free()`.

`dmdns_resolve()`/`dmdns_query()` block until an answer or a timeout -
never call them from a network receive callback.

From the shell (addresses are illustrative):

```
$ nslookup google.com 8.8.8.8
Server:  8.8.8.8

Name:    google.com
Address: 142.250.186.46
Address: 2a00:1450:401b:80e::200e
```

## API

| Function | Description |
|----------|-------------|
| `dmdns_resolve()` | Resolve a name (literal, localhost, hosts, cache, servers). |
| `dmdns_query()` | Ask one server one question. |
| `dmdns_add_server()` / `_remove_server()` / `_clear_servers()` | Static name servers. |
| `dmdns_get_servers()` | Static + DIF-provided servers, as a lookup would use them. |
| `dmdns_provide_servers` (DIF) | Implement to supply name servers (e.g. from DHCP). |
| `dmdns_add_host()` / `_remove_host()` | Static hosts table. |
| `dmdns_flush_cache()` | Drop cached answers. |
| `dmdns_build_query()` / `_parse_response()` / `_is_valid_name()` | Wire codec. |
| `dmdns_parse_address()` / `_address_to_string()` | IPv4/IPv6 literals. |

See [include/dmdns.h](include/dmdns.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[dmdns.md](docs/dmdns.md)** - Architecture and design rationale
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmdns`.

## Project Structure

```
dmdns/
├── docs/                        # Documentation (markdown format)
├── include/
│   └── dmdns.h                  # Public API
├── src/
│   ├── dmdns.c                  # dmod_init()/_deinit(), dmdns_resolve()
│   ├── dmdns_registrations.c    # Built-in API registration
│   ├── dmdns_codec.c            # name validation, query build, response parse
│   ├── dmdns_address.c          # IP literal parse/format
│   ├── dmdns_servers.c          # static servers + dmdns_provide_servers DIF
│   ├── dmdns_hosts.c            # hosts table, localhost
│   ├── dmdns_cache.c            # TTL cache
│   ├── dmdns_query.c            # one query over dmudp
│   └── dmdns_internal.h
├── tests/
│   ├── dmdns_test.c
│   └── fixtures/dmdns_test_provider/   # DIF provider fixture module
├── tools/
│   └── nslookup/                # CLI tool (Application module)
├── CMakeLists.txt
├── Makefile
├── dmdns.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
