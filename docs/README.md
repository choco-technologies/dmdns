# dmdns Documentation

Welcome to the dmdns module documentation - the DNS stub resolver of the
DMOD network stack.

## Contents

- **[dmdns.md](dmdns.md)** - Architecture and design rationale: layers,
  where name servers come from (the `dmdns_provide_servers` DIF), how a
  query works, cache, limits
- **[api-reference.md](api-reference.md)** - Complete API documentation

## Quick Reference

```c
#include "dmdns.h"

dmip_addr_t addr;
int count = dmdns_resolve("google.com", dmip_family_v4, &addr, 1, 0);
if (count > 0)
{
    char text[DMDNS_ADDRESS_STRLEN];
    dmdns_format_address(&addr, text, sizeof(text));
    Dmod_Printf("google.com is %s\n", text);
}
```

View documentation using `dmf-man`:

```bash
dmf-man dmdns          # Main documentation
dmf-man dmdns dmdns    # Architecture
dmf-man dmdns api      # API reference
```
