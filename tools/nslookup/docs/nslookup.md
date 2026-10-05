# nslookup

## What it does

`nslookup <name> [server]` prints the IPv4 (A) and IPv6 (AAAA) addresses of
`<name>`:

1. `Server:` - the server given on the command line, or the first one
   `dmdns_get_servers()` reports, or `(none configured)`.
2. `Name:` - the name looked up.
3. One `Address:` line per address found, IPv4 first.

`-4`/`-6` restrict the lookup to one family. When the A lookup says the name
does not exist (NXDOMAIN), AAAA is not asked at all.

IP literals and `localhost` are answered locally, also when a server is
given.

## Errors

| Message | Cause |
|---------|-------|
| `no such name (NXDOMAIN)` | The name does not exist. |
| `no address of this type` | The name exists, but has no A/AAAA record (of the requested family). |
| `no answer - timed out` | No server answered in time. |
| `no name server configured` | No static server and no `dmdns_provide_servers` provider reported one. |
| `name server failure (SERVFAIL)` / `name server refused the query` | The server could not / would not answer. |
| `network unreachable` | No route to the server (interface down, no lease yet). |

The numeric errno follows the message in parentheses.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | At least one address was printed (or `--help`). |
| 1 | Invalid arguments, or no address found. |

## Testing

`tools/nslookup/tests/nslookup_test.c` runs the real module through
`Dmod_RunModule()`. No name server is reachable from `dmod_loader`, so
successful lookups are served from the hosts table (seeded with
`dmdns_add_host()`), `localhost` and IP literals; lookups that need the
network are checked to fail with a non-zero exit code rather than hang.
