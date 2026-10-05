/**
 * @file dmdns_test.c
 * @brief Test steps for dmdns
 *
 * The codec, address and table steps run on hand-built fixtures (and on
 * real answers captured from 8.8.8.8) and need no network at all. The
 * network steps can't get a real answer - there is no name server
 * reachable from dmod_loader - so they check the honest failure paths
 * instead: no server configured, and a server with no route to it
 * (dmdns_query() must bind, try to send, fail, and clean up).
 *
 * dmdns_get_servers()'s provider discovery is exercised for real through
 * dmdns_test_provider (tests/fixtures/), a Library-type module implementing
 * the dmdns_provide_servers DIF and standing in for a DHCP client - this
 * Application-type test module can't implement it itself, see
 * dmdns_test_provider.h.
 *
 * Fixture messages are built on the heap at their exact size, like dmdns
 * itself does - see make_response().
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdns.h"
#include "dmdns_test_provider.h"
#include <string.h>
#include <errno.h>

#define FIXTURE_ID   0xBEEFu
#define RR_TTL_SHORT 60u
#define RR_TTL_LONG  300u
#define RR_HEADER_LEN 12u   /* owner pointer + TYPE + CLASS + TTL + RDLENGTH */

static dmip_addr_t v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmip_addr_t addr = { 0 };
    addr.family = dmip_family_v4;
    addr.addr.v4[0] = a; addr.addr.v4[1] = b; addr.addr.v4[2] = c; addr.addr.v4[3] = d;
    return addr;
}

static void put_u16(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/*
 * Turn the query for `name` into a response header with `extra_flags`
 * (RCODE, TC) and `ancount` answers. The buffer is grown to leave exactly
 * `records_len` bytes for the records add_record() appends. Release with
 * Dmod_Free().
 */
static uint8_t* make_response(const char* name, uint16_t qtype, uint16_t extra_flags, uint16_t ancount, size_t records_len, size_t* out_len)
{
    uint8_t* query = NULL;
    size_t len = 0;
    if (dmdns_build_query(FIXTURE_ID, name, qtype, &query, &len) != 0)
        return NULL;

    uint8_t* msg = Dmod_Realloc(query, len + records_len);
    if (msg == NULL)
    {
        Dmod_Free(query);
        return NULL;
    }
    put_u16(&msg[2], (uint16_t)(0x8180u | extra_flags)); /* QR, RD, RA */
    put_u16(&msg[6], ancount);
    *out_len = len;
    return msg;
}

/* Append one resource record whose owner is a compression pointer to `owner_offset`. */
static size_t add_record(uint8_t* msg, size_t len, uint16_t owner_offset, uint16_t type, uint32_t ttl, const uint8_t* rdata, uint16_t rdlength)
{
    put_u16(&msg[len], (uint16_t)(0xC000u | owner_offset));
    put_u16(&msg[len + 2], type);
    put_u16(&msg[len + 4], DMDNS_CLASS_IN);
    put_u16(&msg[len + 6], (uint16_t)(ttl >> 16));
    put_u16(&msg[len + 8], (uint16_t)ttl);
    put_u16(&msg[len + 10], rdlength);
    memcpy(&msg[len + RR_HEADER_LEN], rdata, rdlength);
    return len + RR_HEADER_LEN + rdlength;
}

/* Parse and throw the result away - for steps that only care about the return code. */
static int parse_result(const uint8_t* msg, size_t len, uint16_t id, const char* name, uint16_t qtype)
{
    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    int result = dmdns_parse_response(msg, len, id, name, qtype, &addrs, &count, NULL);
    Dmod_Free(addrs);
    return result;
}

static bool same_addr(const dmip_addr_t* a, const dmip_addr_t* b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

void dmod_test_setup(void)
{
    dmdns_test_provider_set(NULL, 0);
    dmdns_clear_servers();
    dmdns_flush_cache();
}

void dmod_test_teardown(void)
{
    dmdns_test_provider_set(NULL, 0);
    dmdns_clear_servers();
    dmdns_remove_host("printer.lan");
}

/* ============================================================================
 *                              Name validation
 * ========================================================================== */

DMOD_TEST_STEP(valid_names_are_accepted)
{
    DMOD_TEST_EXPECT_TRUE(dmdns_is_valid_name("google.com"));
    DMOD_TEST_EXPECT_TRUE(dmdns_is_valid_name("localhost"));
    DMOD_TEST_EXPECT_TRUE(dmdns_is_valid_name("a-b.c_d.e1"));
    DMOD_TEST_EXPECT_TRUE(dmdns_is_valid_name("example.com."));
}

DMOD_TEST_STEP(invalid_names_are_rejected)
{
    char* long_label = Dmod_Malloc(DMDNS_MAX_LABEL_LEN + 2u);
    DMOD_TEST_EXPECT_NOT_NULL(long_label);
    memset(long_label, 'a', DMDNS_MAX_LABEL_LEN + 1u);
    long_label[DMDNS_MAX_LABEL_LEN + 1u] = '\0';

    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(NULL));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(""));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("."));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("a..b"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(".a"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("bad name"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(long_label));
    Dmod_Free(long_label);
}

/* ============================================================================
 *                                Query build
 * ========================================================================== */

DMOD_TEST_STEP(build_query_encodes_header_and_question)
{
    static const uint8_t expected[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 'a', 'b', 0x01, 'c', 0x00,
        0x00, 0x01, 0x00, 0x01,
    };
    uint8_t* msg = NULL;
    size_t len = 0;

    DMOD_TEST_EXPECT_EQ(dmdns_build_query(0x1234u, "ab.c", DMDNS_TYPE_A, &msg, &len), 0);
    DMOD_TEST_EXPECT_EQ(len, sizeof(expected));
    DMOD_TEST_EXPECT_EQ(memcmp(msg, expected, sizeof(expected)), 0);
    Dmod_Free(msg);
}

DMOD_TEST_STEP(build_query_trailing_dot_is_the_same_name)
{
    uint8_t* a = NULL;
    uint8_t* b = NULL;
    size_t len_a = 0, len_b = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(1u, "example.com", DMDNS_TYPE_AAAA, &a, &len_a), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(1u, "example.com.", DMDNS_TYPE_AAAA, &b, &len_b), 0);
    DMOD_TEST_EXPECT_EQ(len_a, len_b);
    DMOD_TEST_EXPECT_EQ(memcmp(a, b, len_a), 0);
    Dmod_Free(a);
    Dmod_Free(b);
}

DMOD_TEST_STEP(build_query_rejects_bad_input)
{
    uint8_t* msg = NULL;
    size_t len = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(1u, "a..b", DMDNS_TYPE_A, &msg, &len), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(1u, "a.b", DMDNS_TYPE_A, NULL, &len), -EINVAL);
    DMOD_TEST_EXPECT_NULL(msg);
}

/* ============================================================================
 *                               Response parse
 * ========================================================================== */

DMOD_TEST_STEP(parse_collects_every_a_record_with_smallest_ttl)
{
    static const uint8_t a1[] = { 142, 250, 1, 1 };
    static const uint8_t a2[] = { 142, 250, 1, 2 };
    size_t len = 0;
    uint8_t* msg = make_response("google.com", DMDNS_TYPE_A, 0, 2, 2u * (RR_HEADER_LEN + 4u), &len);
    DMOD_TEST_EXPECT_NOT_NULL(msg);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_LONG, a1, 4);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_SHORT, a2, 4);

    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    uint32_t ttl = 0;
    dmip_addr_t expected = v4(142, 250, 1, 2);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "google.com", DMDNS_TYPE_A, &addrs, &count, &ttl), 0);
    DMOD_TEST_EXPECT_EQ(count, 2u);
    DMOD_TEST_EXPECT_EQ(ttl, RR_TTL_SHORT);
    DMOD_TEST_EXPECT_TRUE(same_addr(&addrs[1], &expected));
    Dmod_Free(addrs);
    Dmod_Free(msg);
}

DMOD_TEST_STEP(parse_follows_cname_chain_and_matches_case_insensitively)
{
    /* www.example.com CNAME web.example.com ; web.example.com A 192.0.2.10 */
    static const uint8_t cname[] = { 3, 'w', 'e', 'b', 0xC0, 16 }; /* "web" + pointer to "example.com" in the question */
    static const uint8_t addr[] = { 192, 0, 2, 10 };
    size_t len = 0;
    uint8_t* msg = make_response("www.example.com", DMDNS_TYPE_A, 0, 2, 2u * RR_HEADER_LEN + sizeof(cname) + sizeof(addr), &len);
    DMOD_TEST_EXPECT_NOT_NULL(msg);
    size_t cname_rdata = len + RR_HEADER_LEN;
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_CNAME, RR_TTL_LONG, cname, sizeof(cname));
    len = add_record(msg, len, (uint16_t)cname_rdata, DMDNS_TYPE_A, RR_TTL_LONG, addr, sizeof(addr));

    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    dmip_addr_t expected = v4(192, 0, 2, 10);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "WWW.Example.COM", DMDNS_TYPE_A, &addrs, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&addrs[0], &expected));
    Dmod_Free(addrs);
    Dmod_Free(msg);
}

DMOD_TEST_STEP(parse_reads_aaaa_records)
{
    static const uint8_t addr[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    size_t len = 0;
    uint8_t* msg = make_response("v6.example", DMDNS_TYPE_AAAA, 0, 1, RR_HEADER_LEN + sizeof(addr), &len);
    DMOD_TEST_EXPECT_NOT_NULL(msg);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_AAAA, RR_TTL_SHORT, addr, sizeof(addr));

    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "v6.example", DMDNS_TYPE_AAAA, &addrs, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_EQ(addrs[0].family, dmip_family_v6);
    DMOD_TEST_EXPECT_EQ(memcmp(addrs[0].addr.v6, addr, sizeof(addr)), 0);
    Dmod_Free(addrs);
    Dmod_Free(msg);
}

/* Header-only response (no answers) with the given RCODE/flags -> parse result. */
static int negative_answer(uint16_t extra_flags)
{
    size_t len = 0;
    uint8_t* msg = make_response("nope.example", DMDNS_TYPE_A, extra_flags, 0, 0, &len);
    if (msg == NULL)
        return -ENOMEM;
    int result = parse_result(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A);
    Dmod_Free(msg);
    return result;
}

DMOD_TEST_STEP(parse_maps_negative_answers_to_errno)
{
    DMOD_TEST_EXPECT_EQ(negative_answer(3u /* NXDOMAIN */), -ENOENT);
    DMOD_TEST_EXPECT_EQ(negative_answer(2u /* SERVFAIL */), -EAGAIN);
    DMOD_TEST_EXPECT_EQ(negative_answer(5u /* REFUSED */), -ECONNREFUSED);
    DMOD_TEST_EXPECT_EQ(negative_answer(4u /* NOTIMP */), -EIO);
    DMOD_TEST_EXPECT_EQ(negative_answer(0u), -ENODATA);
    DMOD_TEST_EXPECT_EQ(negative_answer(0x0200u /* TC */), -EMSGSIZE);
}

DMOD_TEST_STEP(parse_rejects_foreign_or_broken_messages)
{
    static const uint8_t addr[] = { 10, 0, 0, 1 };
    size_t len = 0;
    uint8_t* msg = make_response("host.example", DMDNS_TYPE_A, 0, 1, RR_HEADER_LEN + sizeof(addr), &len);
    DMOD_TEST_EXPECT_NOT_NULL(msg);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_SHORT, addr, sizeof(addr));

    /* wrong ID, other question, other type, cut short */
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len, FIXTURE_ID + 1u, "host.example", DMDNS_TYPE_A), -EPROTO);
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len, FIXTURE_ID, "evil.example", DMDNS_TYPE_A), -EPROTO);
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len, FIXTURE_ID, "host.example", DMDNS_TYPE_AAAA), -EPROTO);
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len - 2u, FIXTURE_ID, "host.example", DMDNS_TYPE_A), -EPROTO);
    DMOD_TEST_EXPECT_EQ(parse_result(msg, 5u, FIXTURE_ID, "host.example", DMDNS_TYPE_A), -EPROTO);

    /* a query (QR clear) is not an answer */
    put_u16(&msg[2], 0x0100u);
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len, FIXTURE_ID, "host.example", DMDNS_TYPE_A), -EPROTO);
    Dmod_Free(msg);
}

/* Real answers from 8.8.8.8 (captured with ID 0xBEEF): a CNAME chain
 * ending in an A record, and a CNAME chain ending in four AAAA records -
 * both with compressed owner names pointing into the CNAME's own rdata. */
static const uint8_t g_www_github_com_a[] = {
    0xbe, 0xef, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77, 0x77, 0x77,
    0x06, 0x67, 0x69, 0x74, 0x68, 0x75, 0x62, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x01, 0x00, 0x01,
    0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x0b, 0xf6, 0x00, 0x02, 0xc0, 0x10, 0xc0, 0x10,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 0x8c, 0x52, 0x71, 0x04,
};

static const uint8_t g_ipv6_google_com_aaaa[] = {
    0xbe, 0xef, 0x81, 0x80, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x04, 0x69, 0x70, 0x76,
    0x36, 0x06, 0x67, 0x6f, 0x6f, 0x67, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x1c, 0x00,
    0x01, 0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x09, 0x04, 0x69, 0x70,
    0x76, 0x36, 0x01, 0x6c, 0xc0, 0x11, 0xc0, 0x2d, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c,
    0x00, 0x10, 0x26, 0x07, 0xf8, 0xb0, 0x40, 0x01, 0x0c, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x65, 0xc0, 0x2d, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x10, 0x26, 0x07,
    0xf8, 0xb0, 0x40, 0x01, 0x0c, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0xc0, 0x2d,
    0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x10, 0x26, 0x07, 0xf8, 0xb0, 0x40, 0x01,
    0x0c, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8b, 0xc0, 0x2d, 0x00, 0x1c, 0x00, 0x01,
    0x00, 0x00, 0x01, 0x2c, 0x00, 0x10, 0x26, 0x07, 0xf8, 0xb0, 0x40, 0x01, 0x0c, 0x19, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x8a,
};

DMOD_TEST_STEP(parse_real_answer_with_cname_and_a)
{
    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    uint32_t ttl = 0;
    dmip_addr_t expected = v4(140, 82, 113, 4);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(g_www_github_com_a, sizeof(g_www_github_com_a), FIXTURE_ID, "www.github.com",
        DMDNS_TYPE_A, &addrs, &count, &ttl), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_EQ(ttl, 60u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&addrs[0], &expected));
    Dmod_Free(addrs);
}

DMOD_TEST_STEP(parse_real_answer_with_cname_and_aaaa)
{
    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(g_ipv6_google_com_aaaa, sizeof(g_ipv6_google_com_aaaa), FIXTURE_ID, "ipv6.google.com",
        DMDNS_TYPE_AAAA, &addrs, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 4u);

    char* text = dmdns_address_to_string(&addrs[0]);
    DMOD_TEST_EXPECT_NOT_NULL(text);
    DMOD_TEST_EXPECT_EQ(strcmp(text, "2607:f8b0:4001:c19::65"), 0);
    Dmod_Free(text);
    Dmod_Free(addrs);
}

DMOD_TEST_STEP(parse_survives_a_compression_pointer_loop)
{
    size_t len = 0;
    uint8_t* msg = make_response("x.example", DMDNS_TYPE_A, 0, 0, 0, &len);
    DMOD_TEST_EXPECT_NOT_NULL(msg);
    /* Point the question name at itself. */
    msg[DMDNS_HEADER_LEN] = 0xC0;
    msg[DMDNS_HEADER_LEN + 1] = DMDNS_HEADER_LEN;
    DMOD_TEST_EXPECT_EQ(parse_result(msg, len, FIXTURE_ID, "x.example", DMDNS_TYPE_A), -EPROTO);
    Dmod_Free(msg);
}

/* ============================================================================
 *                              Address literals
 * ========================================================================== */

static bool formats_as(const char* text, const char* expected)
{
    dmip_addr_t addr;
    if (dmdns_parse_address(text, &addr) != 0)
        return false;

    char* formatted = dmdns_address_to_string(&addr);
    bool same = formatted != NULL && strcmp(formatted, expected) == 0;
    Dmod_Free(formatted);
    return same;
}

DMOD_TEST_STEP(address_literals_round_trip)
{
    DMOD_TEST_EXPECT_TRUE(formats_as("192.168.1.1", "192.168.1.1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("0.0.0.0", "0.0.0.0"));
    DMOD_TEST_EXPECT_TRUE(formats_as("255.255.255.255", "255.255.255.255"));
    DMOD_TEST_EXPECT_TRUE(formats_as("::1", "::1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("::", "::"));
    DMOD_TEST_EXPECT_TRUE(formats_as("2001:DB8:0:0:0:0:0:1", "2001:db8::1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("1:0:0:2:0:0:0:3", "1:0:0:2::3"));
    DMOD_TEST_EXPECT_TRUE(formats_as("fe80::", "fe80::"));
    DMOD_TEST_EXPECT_TRUE(formats_as("1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"));
    DMOD_TEST_EXPECT_TRUE(formats_as("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
}

static bool is_rejected(const char* text)
{
    dmip_addr_t addr;
    return dmdns_parse_address(text, &addr) == -EINVAL;
}

/* One call per literal rather than a table of `const char*`: dmod only
 * relocates a module's GOT, not pointers stored in its (ro)data, so a
 * static array of string pointers would hold unrelocated addresses. */
DMOD_TEST_STEP(invalid_address_literals_are_rejected)
{
    DMOD_TEST_EXPECT_TRUE(is_rejected(""));
    DMOD_TEST_EXPECT_TRUE(is_rejected("google.com"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("256.1.1.1"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1.2.3"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1.2.3.4.5"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1..2.3"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1.2.3.4 "));
    DMOD_TEST_EXPECT_TRUE(is_rejected(":1"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1:::2"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1::2::3"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("12345::"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1:2:3:4:5:6:7:8:9"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1:2:3:4:5:6:7"));
    DMOD_TEST_EXPECT_TRUE(is_rejected("1:"));
}

DMOD_TEST_STEP(address_without_family_has_no_text)
{
    dmip_addr_t addr = { 0 };
    DMOD_TEST_EXPECT_NULL(dmdns_address_to_string(&addr));
    DMOD_TEST_EXPECT_NULL(dmdns_address_to_string(NULL));
}

/* ============================================================================
 *                                Name servers
 * ========================================================================== */

DMOD_TEST_STEP(static_servers_can_be_added_and_removed)
{
    dmip_addr_t server = v4(8, 8, 8, 8);
    dmip_addr_t* servers = NULL;
    size_t count = 0;

    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), -EEXIST);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(&servers, &count), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&servers[0], &server));
    Dmod_Free(servers);

    DMOD_TEST_EXPECT_EQ(dmdns_remove_server(&server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_remove_server(&server), -ENOENT);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(&servers, &count), 0);
    DMOD_TEST_EXPECT_EQ(count, 0u);
    DMOD_TEST_EXPECT_NULL(servers);
}

DMOD_TEST_STEP(provided_servers_follow_static_ones_without_duplicates)
{
    dmip_addr_t static_server = v4(1, 1, 1, 1);
    dmip_addr_t provided[2];
    provided[0] = v4(1, 1, 1, 1);   /* same as the static one */
    provided[1] = v4(192, 168, 100, 1);
    dmdns_test_provider_set(provided, 2);

    dmip_addr_t* servers = NULL;
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&static_server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(&servers, &count), 0);
    DMOD_TEST_EXPECT_EQ(count, 2u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&servers[0], &static_server));
    DMOD_TEST_EXPECT_TRUE(same_addr(&servers[1], &provided[1]));
    Dmod_Free(servers);
}

DMOD_TEST_STEP(server_without_family_is_rejected)
{
    dmip_addr_t server = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(NULL, NULL), -EINVAL);
}

/* ============================================================================
 *                           Resolving without network
 * ========================================================================== */

/* Resolve and keep only the first address (if any) - most steps need just that. */
static int resolve_first(const char* name, dmip_family_t family, dmip_addr_t* first, size_t* count, uint32_t timeout_ms)
{
    dmip_addr_t* addrs = NULL;
    size_t n = 0;
    int result = dmdns_resolve(name, family, &addrs, &n, timeout_ms);
    if (result == 0 && first != NULL)
        *first = addrs[0];
    if (count != NULL)
        *count = n;
    Dmod_Free(addrs);
    return result;
}

DMOD_TEST_STEP(ip_literal_resolves_to_itself)
{
    dmip_addr_t first;
    size_t count = 0;
    dmip_addr_t expected = v4(10, 1, 2, 3);
    DMOD_TEST_EXPECT_EQ(resolve_first("10.1.2.3", dmip_family_none, &first, &count, 0), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&first, &expected));
    DMOD_TEST_EXPECT_EQ(resolve_first("10.1.2.3", dmip_family_v6, NULL, NULL, 0), -ENODATA);
}

DMOD_TEST_STEP(localhost_is_built_in)
{
    dmip_addr_t first;
    dmip_addr_t expected = v4(127, 0, 0, 1);
    DMOD_TEST_EXPECT_EQ(resolve_first("localhost", dmip_family_v4, &first, NULL, 0), 0);
    DMOD_TEST_EXPECT_TRUE(same_addr(&first, &expected));
    DMOD_TEST_EXPECT_EQ(resolve_first("LocalHost.", dmip_family_v6, &first, NULL, 0), 0);
    DMOD_TEST_EXPECT_EQ(first.family, dmip_family_v6);
    DMOD_TEST_EXPECT_EQ(first.addr.v6[15], 1);
}

DMOD_TEST_STEP(hosts_table_answers_before_any_server)
{
    dmip_addr_t printer = v4(192, 168, 1, 50);
    dmip_addr_t printer2 = v4(192, 168, 1, 51);
    dmip_addr_t first;
    size_t count = 0;

    DMOD_TEST_EXPECT_EQ(dmdns_add_host("printer.lan", &printer), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_add_host("printer.lan", &printer), -EEXIST);
    DMOD_TEST_EXPECT_EQ(dmdns_add_host("printer.lan", &printer2), 0);
    DMOD_TEST_EXPECT_EQ(resolve_first("PRINTER.lan", dmip_family_none, &first, &count, 0), 0);
    DMOD_TEST_EXPECT_EQ(count, 2u);
    DMOD_TEST_EXPECT_TRUE(same_addr(&first, &printer));

    DMOD_TEST_EXPECT_EQ(dmdns_remove_host("printer.lan"), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_remove_host("printer.lan"), -ENOENT);
    DMOD_TEST_EXPECT_EQ(resolve_first("printer.lan", dmip_family_v4, NULL, NULL, 0), -EDESTADDRREQ);
}

DMOD_TEST_STEP(resolve_rejects_bad_arguments)
{
    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_resolve(NULL, dmip_family_v4, &addrs, &count, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("ok.example", dmip_family_v4, NULL, &count, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("ok.example", dmip_family_v4, &addrs, NULL, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("not a name", dmip_family_v4, &addrs, &count, 0), -EINVAL);
    DMOD_TEST_EXPECT_NULL(addrs);
}

DMOD_TEST_STEP(resolve_without_servers_says_so)
{
    DMOD_TEST_EXPECT_EQ(resolve_first("google.com", dmip_family_none, NULL, NULL, 0), -EDESTADDRREQ);
}

DMOD_TEST_STEP(unreachable_server_fails_without_hanging)
{
    /* No interface or route exists in dmod_loader, so the send itself
     * fails - dmdns_query() must report that error (not -ETIMEDOUT after
     * waiting) and release its port. */
    dmip_addr_t server = v4(198, 51, 100, 53);
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), 0);

    int result = resolve_first("google.com", dmip_family_v4, NULL, NULL, 50);
    DMOD_TEST_EXPECT_TRUE(result < 0);
    DMOD_TEST_EXPECT_NE(result, -ENOENT);
    DMOD_TEST_EXPECT_NE(result, -ENODATA);

    /* A failed lookup is never cached - the same error comes back from the network again. */
    DMOD_TEST_EXPECT_EQ(resolve_first("google.com", dmip_family_v4, NULL, NULL, 50), result);
}

DMOD_TEST_STEP(query_rejects_bad_arguments)
{
    dmip_addr_t server = v4(198, 51, 100, 53);
    dmip_addr_t no_family = { 0 };
    dmip_addr_t* addrs = NULL;
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_query(NULL, "a.example", DMDNS_TYPE_A, &addrs, &count, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&no_family, "a.example", DMDNS_TYPE_A, &addrs, &count, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&server, "a..example", DMDNS_TYPE_A, &addrs, &count, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&server, "a.example", DMDNS_TYPE_A, NULL, &count, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_NULL(addrs);
}
