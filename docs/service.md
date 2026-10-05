# The DNS resolver as a dmsystem service

## Why this exists

dmdns is a **Library**-type DMOD module: it has no `main()` and does its
work only after something loads and enables it. `dmod_init()` sets up the
static server list, the hosts table and the answer cache. From then on any
module can call `dmdns_resolve()`.

Without a service, the only thing that loads dmdns is a module that links
it, e.g. `nslookup`. In that case the resolver appears the first time
someone happens to run such a tool. Anything a module set up earlier, such
as `dmdns_add_server()` or `dmdns_add_host()`, has nowhere to live until
then. `configs/dns.ini` gives
[dmsystem](https://github.com/choco-technologies/dmsystem)'s `libsystemd` a
way to bring the resolver up at boot, once, for the whole system.

## `exec=dmdns`, `type=library`

`type=library` is libsystemd's unit type for Library modules:
- **start**: `Dmod_LoadModuleByName` + `Dmod_EnableModule`. This runs
  `dmod_init()` and also pulls in and enables the modules dmdns requires
  (dmudp, dmip, ...).
- **stop**: `Dmod_DisableModule` + `Dmod_UnloadModule`. Static servers, hosts
  entries and the cache are released.

There is no process, so `args`/`stdin`/`stdout`/`restart` have no effect on
this unit. See dmsystem's
[configuration docs](https://github.com/choco-technologies/dmsystem/blob/main/app/libsystemd/docs/configuration.md#typelibrary-services-backed-by-a-library-module-not-a-process).

```ini
# dns.ini
description=DNS resolver (dmdns)
exec=dmdns
type=library
```

## One unit, no device rules

Per-interface services such as `networkd@eth0` (the RX pump) and
`dhcp@eth0` (the DHCP lease) use a template plus a `[class=netif]` rule,
because each interface needs its own instance. The resolver is not shaped
like that:

- **Its state is global.** There is one server list, one hosts table and one
  cache, and lookups are not tied to an interface. Which interface a query
  leaves through is decided by routing (dmroute/dmnetbridge), not by dmdns.
- **Per-interface servers are pulled, not pushed.** The name servers a lease
  on `eth0` offered are reported by the DHCP client through the
  `dmdns_provide_servers` DIF on every lookup (see [dmdns.md](dmdns.md)).
  An interface or lease that appears later is picked up automatically, and
  one that goes away simply stops being reported. Nothing has to tell dmdns
  about it.
- **A library module exists once.** `dns@eth0` and `dns@wlan0` would both
  load and enable the same single `dmdns` module. Stopping either one,
  e.g. when `eth0` is removed, would unload the resolver for every
  interface.

So a single `dns.ini` with no rules file is enough.

## `service start`/`service stop`

```bash
service start dns
service status dns     # running while dmdns is enabled
service stop dns
```

`service stop dns` is refused by the dmod core while another loaded module
still requires dmdns, the same as for any library module.

## Enabling at boot

With dmod-boot, add the module with its unit to a `.dmd` file. The
`service=` tag routes the file into the units directory
(`/configs/services`):

```
dmdns service=dns.ini
nslookup
```

Elsewhere, install dmdns with its `.dmr` and copy the unit by hand:

```bash
cp /opt/dmdns/configs/dns.ini /etc/dmsystem/units/dns.ini
```

## Files

| File | Does |
|------|------|
| [`configs/dns.ini`](../configs/dns.ini) | The dmsystem unit: `exec=dmdns`, `type=library`. |
