/**
 * @file dmdns_codec.c
 * @brief DNS message encoding/decoding (RFC 1035 4.1) - pure functions,
 *        no network, no state
 *
 * Messages are read and written as raw byte buffers indexed by hand rather
 * than through packed C structs - the same reasoning dmip.c/dmudp.c give:
 * dmod's minimal module runtime gives no struct-packing guarantee.
 */
#include "dmod.h"
#include "dmdns_internal.h"
#include <string.h>
#include <errno.h>

/** @brief Header flag bits (RFC 1035 4.1.1) */
#define FLAG_QR          0x8000u
#define FLAG_TC          0x0200u
#define FLAG_RD          0x0100u
#define OPCODE_SHIFT     11u
#define OPCODE_MASK      0x0Fu
#define RCODE_MASK       0x000Fu

/** @brief RCODE values (RFC 1035 4.1.1) */
#define RCODE_NOERROR    0u
#define RCODE_FORMERR    1u
#define RCODE_SERVFAIL   2u
#define RCODE_NXDOMAIN   3u
#define RCODE_REFUSED    5u

/** @brief Fixed part of a resource record after its owner name: TYPE, CLASS, TTL, RDLENGTH */
#define RR_FIXED_LEN     10u

/** @brief Upper bound on compression pointers followed in one name - stops pointer loops */
#define MAX_POINTER_JUMPS 16u

/** @brief Top two bits of a length octet: 00 = label, 11 = compression pointer */
#define LABEL_TYPE_MASK  0xC0u
#define LABEL_POINTER    0xC0u

typedef struct
{
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
} response_header_t;

typedef struct
{
    dmip_addr_t* out;
    size_t       max;
    size_t       count;
    uint32_t     min_ttl;
} answer_set_t;

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_label_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* Length of `name` without its (optional) single trailing dot. */
static size_t name_length(const char* name)
{
    size_t len = strlen(name);
    return (len > 0 && name[len - 1] == '.') ? len - 1 : len;
}

/* ============================================================================
 *                              Name validation
 * ========================================================================== */

dmod_dmdns_api_declaration(1.0, bool, _is_valid_name, ( const char* name ))
{
    if (name == NULL)
        return false;

    size_t len = name_length(name);
    if (len == 0 || len > DMDNS_MAX_NAME_LEN)
        return false;

    size_t label_len = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (name[i] == '.')
        {
            if (label_len == 0)
                return false;
            label_len = 0;
            continue;
        }
        if (!is_label_char(name[i]) || ++label_len > DMDNS_MAX_LABEL_LEN)
            return false;
    }
    return label_len > 0;
}

bool dmdns_name_equal(const char* a, const char* b)
{
    size_t len_a = name_length(a);
    if (len_a != name_length(b))
        return false;

    for (size_t i = 0; i < len_a; i++)
    {
        if (to_lower(a[i]) != to_lower(b[i]))
            return false;
    }
    return true;
}

/* ============================================================================
 *                                Query build
 * ========================================================================== */

/* Encode a (validated) name as a sequence of length-prefixed labels + root label. */
static int encode_name(uint8_t* buffer, size_t buffer_len, size_t pos, const char* name, size_t* out_pos)
{
    const char* label = name;
    while (*label != '\0')
    {
        const char* dot = strchr(label, '.');
        size_t label_len = (dot != NULL) ? (size_t)(dot - label) : strlen(label);
        if (pos + 1u + label_len > buffer_len)
            return -ENOBUFS;

        buffer[pos++] = (uint8_t)label_len;
        memcpy(&buffer[pos], label, label_len);
        pos += label_len;
        if (dot == NULL)
            break;
        label = dot + 1;
    }

    if (pos + 1u > buffer_len)
        return -ENOBUFS;
    buffer[pos++] = 0;
    *out_pos = pos;
    return 0;
}

dmod_dmdns_api_declaration(1.0, int, _build_query, ( uint8_t* buffer, size_t buffer_len, uint16_t id, const char* name, uint16_t qtype, size_t* out_len ))
{
    if (buffer == NULL || out_len == NULL || !dmdns_is_valid_name(name))
        return -EINVAL;
    if (buffer_len < DMDNS_HEADER_LEN)
        return -ENOBUFS;

    memset(buffer, 0, DMDNS_HEADER_LEN);
    write_u16_be(&buffer[0], id);
    write_u16_be(&buffer[2], FLAG_RD);
    write_u16_be(&buffer[4], 1u); /* QDCOUNT - ANCOUNT/NSCOUNT/ARCOUNT stay 0 */

    size_t pos = 0;
    int result = encode_name(buffer, buffer_len, DMDNS_HEADER_LEN, name, &pos);
    if (result != 0)
        return result;
    if (pos + 4u > buffer_len)
        return -ENOBUFS;

    write_u16_be(&buffer[pos], qtype);
    write_u16_be(&buffer[pos + 2u], DMDNS_CLASS_IN);
    *out_len = pos + 4u;
    return 0;
}

/* ============================================================================
 *                               Response parse
 * ========================================================================== */

/* Skip an encoded (possibly compressed) name, report the offset right after it. */
static int skip_name(const uint8_t* msg, size_t len, size_t pos, size_t* out_next)
{
    for (;;)
    {
        if (pos >= len)
            return -EPROTO;

        uint8_t octet = msg[pos];
        if ((octet & LABEL_TYPE_MASK) == LABEL_POINTER)
        {
            if (pos + 2u > len)
                return -EPROTO;
            *out_next = pos + 2u;
            return 0;
        }
        if ((octet & LABEL_TYPE_MASK) != 0u)
            return -EPROTO;
        if (octet == 0u)
        {
            *out_next = pos + 1u;
            return 0;
        }
        pos += 1u + octet;
    }
}

/* Compare one wire label against the next label of `*name`, advance `*name` past it on a match. */
static bool label_matches(const uint8_t* label, size_t label_len, const char** name)
{
    const char* p = *name;
    for (size_t i = 0; i < label_len; i++)
    {
        if (p[i] == '\0' || p[i] == '.' || to_lower((char)label[i]) != to_lower(p[i]))
            return false;
    }
    if (p[label_len] != '\0' && p[label_len] != '.')
        return false;

    *name = (p[label_len] == '.') ? &p[label_len + 1u] : &p[label_len];
    return true;
}

/* Check that the encoded name at `pos` spells `name` (case-insensitively), following compression pointers. */
static int match_name(const uint8_t* msg, size_t len, size_t pos, const char* name, size_t* out_next)
{
    const char* rest = name;
    size_t jumps = 0;
    bool jumped = false;

    for (;;)
    {
        if (pos >= len)
            return -EPROTO;

        uint8_t octet = msg[pos];
        if ((octet & LABEL_TYPE_MASK) == LABEL_POINTER)
        {
            if (pos + 2u > len || ++jumps > MAX_POINTER_JUMPS)
                return -EPROTO;
            if (!jumped)
                *out_next = pos + 2u;
            jumped = true;
            pos = ((size_t)(octet & ~LABEL_TYPE_MASK) << 8) | msg[pos + 1u];
            continue;
        }
        if ((octet & LABEL_TYPE_MASK) != 0u)
            return -EPROTO;
        if (octet == 0u)
            break;
        if (pos + 1u + octet > len || !label_matches(&msg[pos + 1u], octet, &rest))
            return -EPROTO;
        pos += 1u + octet;
    }

    if (!jumped)
        *out_next = pos + 1u;
    return (*rest == '\0') ? 0 : -EPROTO;
}

static int parse_header(const uint8_t* msg, size_t len, uint16_t id, response_header_t* header)
{
    if (len < DMDNS_HEADER_LEN || read_u16_be(&msg[0]) != id)
        return -EPROTO;

    header->flags   = read_u16_be(&msg[2]);
    header->qdcount = read_u16_be(&msg[4]);
    header->ancount = read_u16_be(&msg[6]);

    if ((header->flags & FLAG_QR) == 0u || ((header->flags >> OPCODE_SHIFT) & OPCODE_MASK) != 0u)
        return -EPROTO;
    return 0;
}

/* The response must repeat exactly our one question. */
static int check_question(const uint8_t* msg, size_t len, uint16_t qdcount, const char* name, uint16_t qtype, size_t* pos)
{
    if (qdcount != 1u)
        return -EPROTO;

    size_t next = 0;
    int result = match_name(msg, len, *pos, name, &next);
    if (result != 0)
        return result;
    if (next + 4u > len || read_u16_be(&msg[next]) != qtype || read_u16_be(&msg[next + 2u]) != DMDNS_CLASS_IN)
        return -EPROTO;

    *pos = next + 4u;
    return 0;
}

static int rcode_to_errno(uint16_t rcode)
{
    switch (rcode)
    {
        case RCODE_FORMERR:  return -EPROTO;
        case RCODE_SERVFAIL: return -EAGAIN;
        case RCODE_NXDOMAIN: return -ENOENT;
        case RCODE_REFUSED:  return -ECONNREFUSED;
        default:             return -EIO;
    }
}

static size_t qtype_address_len(uint16_t qtype)
{
    if (qtype == DMDNS_TYPE_A)
        return DMIP_IPV4_ADDR_LEN;
    if (qtype == DMDNS_TYPE_AAAA)
        return DMIP_IPV6_ADDR_LEN;
    return 0;
}

static void add_answer(answer_set_t* set, uint16_t qtype, const uint8_t* rdata, uint32_t ttl)
{
    if (set->count >= set->max)
        return;

    dmip_addr_t* addr = &set->out[set->count++];
    memset(addr, 0, sizeof(*addr));
    if (qtype == DMDNS_TYPE_A)
    {
        addr->family = dmip_family_v4;
        memcpy(addr->addr.v4, rdata, DMIP_IPV4_ADDR_LEN);
    }
    else
    {
        addr->family = dmip_family_v6;
        memcpy(addr->addr.v6, rdata, DMIP_IPV6_ADDR_LEN);
    }

    /* RFC 2181 8: a TTL with the top bit set is treated as zero */
    ttl = (ttl & 0x80000000u) ? 0u : ttl;
    if (ttl < set->min_ttl)
        set->min_ttl = ttl;
}

/* Walk the answer section, keep every IN record of `qtype` (CNAMEs and others are skipped). */
static int collect_answers(const uint8_t* msg, size_t len, size_t pos, uint16_t ancount, uint16_t qtype, answer_set_t* set)
{
    size_t address_len = qtype_address_len(qtype);

    for (uint16_t i = 0; i < ancount; i++)
    {
        int result = skip_name(msg, len, pos, &pos);
        if (result != 0)
            return result;
        if (pos + RR_FIXED_LEN > len)
            return -EPROTO;

        uint16_t type     = read_u16_be(&msg[pos]);
        uint16_t rr_class = read_u16_be(&msg[pos + 2u]);
        uint32_t ttl      = read_u32_be(&msg[pos + 4u]);
        uint16_t rdlength = read_u16_be(&msg[pos + 8u]);
        pos += RR_FIXED_LEN;
        if (pos + rdlength > len)
            return -EPROTO;

        if (type == qtype && rr_class == DMDNS_CLASS_IN && address_len != 0u && rdlength == address_len)
            add_answer(set, qtype, &msg[pos], ttl);
        pos += rdlength;
    }
    return 0;
}

dmod_dmdns_api_declaration(1.0, int, _parse_response, ( const uint8_t* message, size_t length, uint16_t id, const char* name, uint16_t qtype,
    dmip_addr_t* out, size_t max, size_t* out_count, uint32_t* out_ttl_sec ))
{
    if (message == NULL || name == NULL || out_count == NULL || (out == NULL && max > 0u))
        return -EINVAL;

    *out_count = 0;
    if (out_ttl_sec != NULL)
        *out_ttl_sec = DMDNS_NEGATIVE_TTL_SEC;

    response_header_t header;
    size_t pos = DMDNS_HEADER_LEN;
    int result = parse_header(message, length, id, &header);
    if (result == 0)
        result = check_question(message, length, header.qdcount, name, qtype, &pos);
    if (result != 0)
        return result;
    if ((header.flags & RCODE_MASK) != RCODE_NOERROR)
        return rcode_to_errno(header.flags & RCODE_MASK);

    answer_set_t set = { out, max, 0, UINT32_MAX };
    result = collect_answers(message, length, pos, header.ancount, qtype, &set);
    if (result != 0)
        return result;
    if (set.count == 0u)
        return (header.flags & FLAG_TC) ? -EMSGSIZE : -ENODATA;

    *out_count = set.count;
    if (out_ttl_sec != NULL)
        *out_ttl_sec = set.min_ttl;
    return 0;
}
