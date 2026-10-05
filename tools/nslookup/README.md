# nslookup

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

`nslookup` DMOD application module - a CLI to look a host name up and print
its addresses, built on top of [dmdns](../../)'s API. It builds/parses no
DNS bytes itself.

## Usage

```
nslookup [options] <name> [server]   Look <name> up and print its addresses
nslookup --help | -h                 Show this help

Options:
  -4                Only IPv4 (A) addresses
  -6                Only IPv6 (AAAA) addresses
  -t <timeout_ms>   Wait per server attempt (default: 2000)
```

- Without `[server]` the lookup goes through `dmdns_resolve()` - hosts
  table, cache and every configured name server (static ones and every
  `dmdns_provide_servers` provider, e.g. a DHCP lease) - exactly the path
  any other module's lookup takes.
- With `[server]` (an IP literal) only that server is asked, directly, via
  `dmdns_query()` - useful to check a server before configuring it.

Example output (addresses are illustrative):

```
$ nslookup google.com 8.8.8.8
Server:  8.8.8.8

Name:    google.com
Address: 142.250.186.46
Address: 2a00:1450:401b:80e::200e

$ nslookup no-such-host.example 8.8.8.8
Server:  8.8.8.8

Name:    no-such-host.example
nslookup: no-such-host.example: no such name (NXDOMAIN) (-2)
```

Exit code: 0 if at least one address was found, 1 otherwise.

## Building

This module lives under `tools/nslookup` inside the `dmdns` repository and
is built as part of the parent's CMake configure (the top-level
`CMakeLists.txt` calls `add_subdirectory(tools)`) - it is not built
standalone.

## Documentation

- **[docs/nslookup.md](docs/nslookup.md)** - Usage, exit codes, and how this module is tested
