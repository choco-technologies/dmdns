# nslookup Documentation

Welcome to the nslookup module documentation.

## Contents

- **[nslookup.md](nslookup.md)** - Usage, exit codes, and how this module is tested

## Quick Reference

```
nslookup [options] <name> [server]   Look <name> up and print its addresses
nslookup --help | -h                 Show this help

Options:
  -4                Only IPv4 (A) addresses
  -6                Only IPv6 (AAAA) addresses
  -t <timeout_ms>   Wait per server attempt (default: 2000)
```

View documentation using `dmf-man`:

```bash
dmf-man nslookup           # Main documentation
dmf-man nslookup nslookup  # nslookup.md
```
