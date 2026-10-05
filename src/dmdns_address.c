/**
 * @file dmdns_address.c
 * @brief IP address literals: text <-> dmip_addr_t
 *
 * dmod's minimal module runtime has no inet_pton()/inet_ntop() (nor
 * strtol()) - see dmod/src/module/string.c - so both directions are done
 * by hand here. Exposed publicly because every tool that takes "a host"
 * on its command line (ping, nslookup, ...) needs exactly this before it
 * decides whether a lookup is needed at all.
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include <string.h>
#include <errno.h>

#define IPV6_GROUPS      8u
#define IPV6_GROUP_HEX   4u

typedef struct
{
    char*  buffer;
    size_t capacity;
    size_t length;
    bool   overflow;
} text_t;

bool dmdns_addr_equal(const dmip_addr_t* a, const dmip_addr_t* b)
{
    if (a->family != b->family)
        return false;
    if (a->family == dmip_family_v4)
        return memcmp(a->addr.v4, b->addr.v4, DMIP_IPV4_ADDR_LEN) == 0;
    if (a->family == dmip_family_v6)
        return memcmp(a->addr.v6, b->addr.v6, DMIP_IPV6_ADDR_LEN) == 0;
    return true;
}

/* ============================================================================
 *                                   Parse
 * ========================================================================== */

static int parse_ipv4(const char* s, dmip_addr_t* out)
{
    dmip_addr_t addr = { 0 };
    addr.family = dmip_family_v4;

    for (size_t i = 0; i < DMIP_IPV4_ADDR_LEN; i++)
    {
        uint32_t octet = 0;
        size_t digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            octet = octet * 10u + (uint32_t)(*s++ - '0');
            if (octet > 255u || ++digits > 3u)
                return -EINVAL;
        }
        if (digits == 0u)
            return -EINVAL;
        addr.addr.v4[i] = (uint8_t)octet;

        char expected = (i + 1u < DMIP_IPV4_ADDR_LEN) ? '.' : '\0';
        if (*s++ != expected)
            return -EINVAL;
    }

    *out = addr;
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* One group of 1-4 hex digits, `*s` advanced past it. */
static bool parse_hex_group(const char** s, uint16_t* out)
{
    uint32_t value = 0;
    size_t digits = 0;
    int digit;
    while ((digit = hex_value(**s)) >= 0)
    {
        if (++digits > IPV6_GROUP_HEX)
            return false;
        value = (value << 4) | (uint32_t)digit;
        (*s)++;
    }
    *out = (uint16_t)value;
    return digits > 0u;
}

/* Expand `count` parsed groups, with the "::" gap (if any) at index `gap`, into 16 bytes. */
static void store_ipv6(const uint16_t* groups, size_t count, int gap, dmip_addr_t* out)
{
    uint16_t full[IPV6_GROUPS] = { 0 };
    size_t head = (gap < 0) ? count : (size_t)gap;
    size_t tail = count - head;

    memcpy(full, groups, head * sizeof(uint16_t));
    memcpy(&full[IPV6_GROUPS - tail], &groups[head], tail * sizeof(uint16_t));

    memset(out, 0, sizeof(*out));
    out->family = dmip_family_v6;
    for (size_t i = 0; i < IPV6_GROUPS; i++)
    {
        out->addr.v6[2u * i]      = (uint8_t)(full[i] >> 8);
        out->addr.v6[2u * i + 1u] = (uint8_t)(full[i] & 0xFFu);
    }
}

static int parse_ipv6(const char* s, dmip_addr_t* out)
{
    uint16_t groups[IPV6_GROUPS];
    size_t count = 0;
    int gap = -1;

    if (s[0] == ':' && s[1] == ':')
    {
        gap = 0;
        s += 2;
    }
    while (*s != '\0')
    {
        if (count >= IPV6_GROUPS || !parse_hex_group(&s, &groups[count]))
            return -EINVAL;
        count++;
        if (*s == '\0')
            break;
        if (*s++ != ':' || *s == '\0')
            return -EINVAL;
        if (*s == ':')
        {
            if (gap >= 0)
                return -EINVAL;
            gap = (int)count;
            s++;
        }
    }

    if ((gap < 0) ? (count != IPV6_GROUPS) : (count > IPV6_GROUPS - 1u))
        return -EINVAL;
    store_ipv6(groups, count, gap, out);
    return 0;
}

dmod_dmdns_api_declaration(1.0, int, _parse_address, ( const char* text, dmip_addr_t* out ))
{
    if (text == NULL || out == NULL)
        return -EINVAL;

    return (strchr(text, ':') != NULL) ? parse_ipv6(text, out) : parse_ipv4(text, out);
}

/* ============================================================================
 *                                   Format
 * ========================================================================== */

static void put_char(text_t* text, char c)
{
    if (text->length + 1u < text->capacity)
        text->buffer[text->length++] = c;
    else
        text->overflow = true;
}

static void put_number(text_t* text, uint32_t value, uint32_t base)
{
    static const char digits[] = "0123456789abcdef";
    char reversed[10];
    size_t n = 0;
    do
    {
        reversed[n++] = digits[value % base];
        value /= base;
    } while (value != 0u);

    while (n > 0u)
        put_char(text, reversed[--n]);
}

static void format_ipv4(text_t* text, const uint8_t* v4)
{
    for (size_t i = 0; i < DMIP_IPV4_ADDR_LEN; i++)
    {
        if (i > 0u)
            put_char(text, '.');
        put_number(text, v4[i], 10u);
    }
}

/* RFC 5952 4.2: the longest run (>= 2) of zero groups, the first one on a tie. */
static void find_zero_run(const uint16_t* groups, size_t* out_start, size_t* out_len)
{
    *out_start = 0;
    *out_len = 0;
    for (size_t i = 0; i < IPV6_GROUPS; )
    {
        size_t run = 0;
        while (i + run < IPV6_GROUPS && groups[i + run] == 0u)
            run++;
        if (run >= 2u && run > *out_len)
        {
            *out_start = i;
            *out_len = run;
        }
        i += (run > 0u) ? run : 1u;
    }
}

static void format_ipv6(text_t* text, const uint8_t* v6)
{
    uint16_t groups[IPV6_GROUPS];
    for (size_t i = 0; i < IPV6_GROUPS; i++)
        groups[i] = (uint16_t)(((uint16_t)v6[2u * i] << 8) | v6[2u * i + 1u]);

    size_t run_start, run_len;
    find_zero_run(groups, &run_start, &run_len);

    for (size_t i = 0; i < IPV6_GROUPS; i++)
    {
        if (run_len > 0u && i == run_start)
        {
            put_char(text, ':');
            put_char(text, ':');
            i += run_len - 1u;
            continue;
        }
        if (i > 0u && !(run_len > 0u && i == run_start + run_len))
            put_char(text, ':');
        put_number(text, groups[i], 16u);
    }
}

dmod_dmdns_api_declaration(1.0, int, _format_address, ( const dmip_addr_t* addr, char* buffer, size_t buffer_len ))
{
    if (addr == NULL || buffer == NULL || buffer_len == 0u)
        return -EINVAL;

    text_t text = { buffer, buffer_len, 0, false };
    if (addr->family == dmip_family_v4)
        format_ipv4(&text, addr->addr.v4);
    else if (addr->family == dmip_family_v6)
        format_ipv6(&text, addr->addr.v6);
    else
        return -EINVAL;

    buffer[text.length] = '\0';
    return text.overflow ? -ENOBUFS : 0;
}
