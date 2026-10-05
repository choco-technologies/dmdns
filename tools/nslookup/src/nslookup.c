/**
 * @file nslookup.c
 * @brief nslookup - look a host name up and print its addresses
 *
 * A thin CLI over dmdns: without a server argument it goes through
 * dmdns_resolve() (hosts table, cache, every configured name server - the
 * same path any other module's lookup takes); with one it asks only that
 * server, directly, through dmdns_query() - handy to check a name server
 * before configuring it. IPv4 (A) and IPv6 (AAAA) addresses are both
 * looked up unless -4/-6 restricts it to one family.
 */
#include "dmod.h"
#include "dmdns.h"
#include <string.h>
#include <errno.h>

typedef struct
{
    const char*  prog;
    const char*  name;
    const char*  server_text;
    dmip_addr_t  server;
    bool         want_v4;
    bool         want_v6;
    uint32_t     timeout_ms;
} options_t;

/* Parses a plain decimal uint32_t (dmod's minimal module runtime has no
 * strtol()/atoi() - see dmod/src/module/string.c's replacement set). */
static bool parse_uint32(const char* s, uint32_t* out)
{
    if (s == NULL || *s == '\0')
        return false;

    uint64_t value = 0;
    for (const char* p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10u + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }
    *out = (uint32_t)value;
    return true;
}

static const char* error_text(int error)
{
    switch (error)
    {
        case -ENOENT:       return "no such name (NXDOMAIN)";
        case -ENODATA:      return "no address of this type";
        case -ETIMEDOUT:    return "no answer - timed out";
        case -EDESTADDRREQ: return "no name server configured";
        case -EAGAIN:       return "name server failure (SERVFAIL)";
        case -ECONNREFUSED: return "name server refused the query";
        case -EMSGSIZE:     return "answer truncated";
        case -EPROTO:       return "malformed or mismatched answer";
        case -ENETUNREACH:  return "network unreachable";
        case -EINVAL:       return "invalid name";
        default:            return "lookup failed";
    }
}

static void print_usage(const char* prog)
{
    Dmod_Printf("Usage:\n");
    Dmod_Printf("  %s [options] <name> [server]   Look <name> up and print its addresses\n", prog);
    Dmod_Printf("  %s --help | -h                 Show this help\n", prog);
    Dmod_Printf("\n");
    Dmod_Printf("Options:\n");
    Dmod_Printf("  -4                Only IPv4 (A) addresses\n");
    Dmod_Printf("  -6                Only IPv6 (AAAA) addresses\n");
    Dmod_Printf("  -t <timeout_ms>   Wait per server attempt (default: %u)\n", (unsigned)DMDNS_DEFAULT_TIMEOUT_MS);
    Dmod_Printf("\n");
    Dmod_Printf("Without [server] the configured name servers are used (dmdns_get_servers():\n");
    Dmod_Printf("static ones and every dmdns_provide_servers provider, e.g. a DHCP lease).\n");
    Dmod_Printf("[server] must be an IP address literal.\n");
}

static void print_server(const options_t* options)
{
    char text[DMDNS_ADDRESS_STRLEN];
    dmip_addr_t servers[DMDNS_MAX_SERVERS];

    if (options->server_text != NULL)
    {
        Dmod_Printf("Server:  %s\n", options->server_text);
    }
    else if (dmdns_get_servers(servers, DMDNS_MAX_SERVERS) > 0u && dmdns_format_address(&servers[0], text, sizeof(text)) == 0)
    {
        Dmod_Printf("Server:  %s\n", text);
    }
    else
    {
        Dmod_Printf("Server:  (none configured)\n");
    }
    Dmod_Printf("\n");
}

static void print_addresses(const dmip_addr_t* addrs, int count)
{
    char text[DMDNS_ADDRESS_STRLEN];
    for (int i = 0; i < count; i++)
    {
        if (dmdns_format_address(&addrs[i], text, sizeof(text)) == 0)
            Dmod_Printf("Address: %s\n", text);
    }
}

/* One family: through the resolver, or straight to the given server. */
static int lookup_family(const options_t* options, dmip_family_t family, dmip_addr_t* out, size_t max)
{
    if (options->server_text == NULL)
        return dmdns_resolve(options->name, family, out, max, options->timeout_ms);

    dmip_addr_t literal;
    if (dmdns_parse_address(options->name, &literal) == 0)
        return dmdns_resolve(options->name, family, out, max, options->timeout_ms);

    uint16_t qtype = (family == dmip_family_v4) ? DMDNS_TYPE_A : DMDNS_TYPE_AAAA;
    return dmdns_query(&options->server, options->name, qtype, out, max, NULL, options->timeout_ms);
}

static int lookup_and_print(const options_t* options, dmip_family_t family, size_t* found)
{
    dmip_addr_t addrs[DMDNS_MAX_ADDRESSES];
    int result = lookup_family(options, family, addrs, DMDNS_MAX_ADDRESSES);
    if (result > 0)
    {
        print_addresses(addrs, result);
        *found += (size_t)result;
    }
    return result;
}

static int run_lookup(const options_t* options)
{
    size_t found = 0;
    int v4_result = -ENODATA;
    int v6_result = -ENODATA;

    print_server(options);
    Dmod_Printf("Name:    %s\n", options->name);

    if (options->want_v4)
    {
        v4_result = lookup_and_print(options, dmip_family_v4, &found);
        if (v4_result == -ENOENT)
            return v4_result; /* the name does not exist - no point asking for AAAA */
    }
    if (options->want_v6)
        v6_result = lookup_and_print(options, dmip_family_v6, &found);

    if (found > 0u)
        return 0;
    /* Report the more telling of the two failures: "no address of this type" only if both say so. */
    return (v4_result != -ENODATA) ? v4_result : v6_result;
}

static int parse_option(options_t* options, int argc, char* argv[], int* i)
{
    const char* arg = argv[*i];
    if (strcmp(arg, "-4") == 0)
    {
        options->want_v6 = false;
        return 0;
    }
    if (strcmp(arg, "-6") == 0)
    {
        options->want_v4 = false;
        return 0;
    }
    if (strcmp(arg, "-t") == 0 && *i + 1 < argc)
    {
        if (!parse_uint32(argv[++(*i)], &options->timeout_ms) || options->timeout_ms == 0u)
            return -EINVAL;
        return 0;
    }
    return -EINVAL;
}

static int parse_arguments(options_t* options, int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
        {
            if (parse_option(options, argc, argv, &i) != 0)
                return -EINVAL;
        }
        else if (options->name == NULL)
            options->name = argv[i];
        else if (options->server_text == NULL)
            options->server_text = argv[i];
        else
            return -EINVAL;
    }
    if (options->name == NULL || (!options->want_v4 && !options->want_v6))
        return -EINVAL;
    if (options->server_text != NULL && dmdns_parse_address(options->server_text, &options->server) != 0)
    {
        Dmod_Printf("%s: invalid server address '%s'\n", options->prog, options->server_text);
        return -EINVAL;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    options_t options = { 0 };
    options.prog       = (argc > 0 && argv[0] != NULL) ? argv[0] : "nslookup";
    options.want_v4    = true;
    options.want_v6    = true;
    options.timeout_ms = DMDNS_DEFAULT_TIMEOUT_MS;

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(options.prog);
        return 0;
    }
    if (parse_arguments(&options, argc, argv) != 0)
    {
        print_usage(options.prog);
        return 1;
    }

    int result = run_lookup(&options);
    if (result != 0)
    {
        Dmod_Printf("%s: %s: %s (%d)\n", options.prog, options.name, error_text(result), result);
        return 1;
    }
    return 0;
}
