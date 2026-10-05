# dmdns Documentation

Welcome to the dmdns module documentation - the DNS stub resolver of the
DMOD network stack.

## Contents

- **[dmdns.md](dmdns.md)** - Architecture and design rationale: layers,
  where name servers come from (the `dmdns_provide_servers` DIF), how a
  query works, cache, limits
- **[api-reference.md](api-reference.md)** - Complete API documentation
- **[service.md](service.md)** - Running the resolver as a `dmsystem` service (`configs/dns.ini`)

## Quick Reference

```c
#include "dmdns.h"

dmip_addr_t* addrs = NULL;
size_t count = 0;
if (dmdns_resolve("google.com", dmip_family_v4, &addrs, &count, 0) == 0)
{
    char* text = dmdns_address_to_string(&addrs[0]);
    Dmod_Printf("google.com is %s\n", text);
    Dmod_Free(text);
    Dmod_Free(addrs);
}
```

View documentation using `dmf-man`:

```bash
dmf-man dmdns          # Main documentation
dmf-man dmdns dmdns    # Architecture
dmf-man dmdns api      # API reference
```
