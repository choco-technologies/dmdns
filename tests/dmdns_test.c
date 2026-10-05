/**
 * @file dmdns_test.c
 * @brief Test steps for dmdns
 *
 * The codec, address and table steps run on hand-built fixtures and need
 * no network at all. The network steps can't get a real answer - there is
 * no name server reachable from dmod_loader - so they check the honest
 * failure paths instead: no server configured, and a server with no route
 * to it (dmdns_query() must bind, try to send, fail, and clean up).
 *
 * dmdns_get_servers()'s provider discovery is exercised for real through
 * dmdns_test_provider (tests/fixtures/), a Library-type module implementing
 * the dmdns_provide_servers DIF and standing in for a DHCP client - this
 * Application-type test module can't implement it itself, see
 * dmdns_test_provider.h.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdns.h"
#include "dmdns_test_provider.h"
#include <string.h>
#include <errno.h>

#define FIXTURE_ID 0xBEEFu
#define RR_TTL_SHORT 60u
#define RR_TTL_LONG  300u

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

/* Turn the query for `name` in `msg` into a response header with `rcode`/`flags`; returns its length. */
static size_t make_response(uint8_t* msg, const char* name, uint16_t qtype, uint16_t extra_flags, uint16_t ancount)
{
    size_t len = 0;
    dmdns_build_query(msg, DMDNS_MAX_MESSAGE_LEN, FIXTURE_ID, name, qtype, &len);
    put_u16(&msg[2], (uint16_t)(0x8180u | extra_flags)); /* QR, RD, RA */
    put_u16(&msg[6], ancount);
    return len;
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
    memcpy(&msg[len + 12], rdata, rdlength);
    return len + 12u + rdlength;
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
    char long_label[DMDNS_MAX_LABEL_LEN + 2];
    memset(long_label, 'a', DMDNS_MAX_LABEL_LEN + 1);
    long_label[DMDNS_MAX_LABEL_LEN + 1] = '\0';

    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(NULL));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(""));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("."));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("a..b"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(".a"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name("bad name"));
    DMOD_TEST_EXPECT_FALSE(dmdns_is_valid_name(long_label));
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
    uint8_t msg[DMDNS_MAX_QUERY_LEN];
    size_t len = 0;

    DMOD_TEST_EXPECT_EQ(dmdns_build_query(msg, sizeof(msg), 0x1234u, "ab.c", DMDNS_TYPE_A, &len), 0);
    DMOD_TEST_EXPECT_EQ(len, sizeof(expected));
    DMOD_TEST_EXPECT_EQ(memcmp(msg, expected, sizeof(expected)), 0);
}

DMOD_TEST_STEP(build_query_trailing_dot_is_the_same_name)
{
    uint8_t a[DMDNS_MAX_QUERY_LEN], b[DMDNS_MAX_QUERY_LEN];
    size_t len_a = 0, len_b = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(a, sizeof(a), 1u, "example.com", DMDNS_TYPE_AAAA, &len_a), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(b, sizeof(b), 1u, "example.com.", DMDNS_TYPE_AAAA, &len_b), 0);
    DMOD_TEST_EXPECT_EQ(len_a, len_b);
    DMOD_TEST_EXPECT_EQ(memcmp(a, b, len_a), 0);
}

DMOD_TEST_STEP(build_query_rejects_bad_input)
{
    uint8_t msg[16];
    size_t len = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(msg, sizeof(msg), 1u, "a..b", DMDNS_TYPE_A, &len), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_build_query(msg, sizeof(msg), 1u, "much-too-long-for-this.buffer", DMDNS_TYPE_A, &len), -ENOBUFS);
}

/* ============================================================================
 *                               Response parse
 * ========================================================================== */

DMOD_TEST_STEP(parse_collects_every_a_record_with_smallest_ttl)
{
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    static const uint8_t a1[] = { 142, 250, 1, 1 };
    static const uint8_t a2[] = { 142, 250, 1, 2 };
    size_t len = make_response(msg, "google.com", DMDNS_TYPE_A, 0, 2);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_LONG, a1, 4);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_SHORT, a2, 4);

    dmip_addr_t out[4];
    size_t count = 0;
    uint32_t ttl = 0;
    dmip_addr_t expected = v4(142, 250, 1, 2);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "google.com", DMDNS_TYPE_A, out, 4, &count, &ttl), 0);
    DMOD_TEST_EXPECT_EQ(count, 2u);
    DMOD_TEST_EXPECT_EQ(ttl, RR_TTL_SHORT);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[1], &expected, sizeof(expected)), 0);
}

DMOD_TEST_STEP(parse_follows_cname_chain_and_matches_case_insensitively)
{
    /* www.example.com CNAME web.example.com ; web.example.com A 192.0.2.10 */
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    static const uint8_t cname[] = { 3, 'w', 'e', 'b', 0xC0, 16 }; /* "web" + pointer to "example.com" in the question */
    static const uint8_t addr[] = { 192, 0, 2, 10 };
    size_t len = make_response(msg, "www.example.com", DMDNS_TYPE_A, 0, 2);
    size_t cname_rdata = len + 12u;
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_CNAME, RR_TTL_LONG, cname, sizeof(cname));
    len = add_record(msg, len, (uint16_t)cname_rdata, DMDNS_TYPE_A, RR_TTL_LONG, addr, sizeof(addr));

    dmip_addr_t out[2];
    size_t count = 0;
    dmip_addr_t expected = v4(192, 0, 2, 10);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "WWW.Example.COM", DMDNS_TYPE_A, out, 2, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &expected, sizeof(expected)), 0);
}

DMOD_TEST_STEP(parse_reads_aaaa_records)
{
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    static const uint8_t addr[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    size_t len = make_response(msg, "v6.example", DMDNS_TYPE_AAAA, 0, 1);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_AAAA, RR_TTL_SHORT, addr, sizeof(addr));

    dmip_addr_t out[1];
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "v6.example", DMDNS_TYPE_AAAA, out, 1, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_EQ(out[0].family, dmip_family_v6);
    DMOD_TEST_EXPECT_EQ(memcmp(out[0].addr.v6, addr, sizeof(addr)), 0);
}

DMOD_TEST_STEP(parse_maps_negative_answers_to_errno)
{
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    dmip_addr_t out[1];
    size_t count = 0;

    size_t len = make_response(msg, "nope.example", DMDNS_TYPE_A, 3u /* NXDOMAIN */, 0);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A, out, 1, &count, NULL), -ENOENT);

    len = make_response(msg, "nope.example", DMDNS_TYPE_A, 2u /* SERVFAIL */, 0);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EAGAIN);

    len = make_response(msg, "nope.example", DMDNS_TYPE_A, 5u /* REFUSED */, 0);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A, out, 1, &count, NULL), -ECONNREFUSED);

    len = make_response(msg, "nope.example", DMDNS_TYPE_A, 0, 0);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A, out, 1, &count, NULL), -ENODATA);

    len = make_response(msg, "nope.example", DMDNS_TYPE_A, 0x0200u /* TC */, 0);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "nope.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EMSGSIZE);
}

DMOD_TEST_STEP(parse_rejects_foreign_or_broken_messages)
{
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    static const uint8_t addr[] = { 10, 0, 0, 1 };
    dmip_addr_t out[1];
    size_t count = 0;
    size_t len = make_response(msg, "host.example", DMDNS_TYPE_A, 0, 1);
    len = add_record(msg, len, DMDNS_HEADER_LEN, DMDNS_TYPE_A, RR_TTL_SHORT, addr, sizeof(addr));

    /* wrong ID, other question, other type, cut short */
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID + 1u, "host.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "evil.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "host.example", DMDNS_TYPE_AAAA, out, 1, &count, NULL), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len - 2u, FIXTURE_ID, "host.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, 5u, FIXTURE_ID, "host.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);

    /* a query (QR clear) is not an answer */
    put_u16(&msg[2], 0x0100u);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "host.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);
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
    dmip_addr_t out[DMDNS_MAX_ADDRESSES];
    size_t count = 0;
    uint32_t ttl = 0;
    dmip_addr_t expected = v4(140, 82, 113, 4);
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(g_www_github_com_a, sizeof(g_www_github_com_a), FIXTURE_ID, "www.github.com",
        DMDNS_TYPE_A, out, DMDNS_MAX_ADDRESSES, &count, &ttl), 0);
    DMOD_TEST_EXPECT_EQ(count, 1u);
    DMOD_TEST_EXPECT_EQ(ttl, 60u);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &expected, sizeof(expected)), 0);
}

DMOD_TEST_STEP(parse_real_answer_with_cname_and_aaaa)
{
    dmip_addr_t out[DMDNS_MAX_ADDRESSES];
    char text[DMDNS_ADDRESS_STRLEN];
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(g_ipv6_google_com_aaaa, sizeof(g_ipv6_google_com_aaaa), FIXTURE_ID, "ipv6.google.com",
        DMDNS_TYPE_AAAA, out, DMDNS_MAX_ADDRESSES, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 4u);
    DMOD_TEST_EXPECT_EQ(dmdns_format_address(&out[0], text, sizeof(text)), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(text, "2607:f8b0:4001:c19::65"), 0);

    /* `max` caps the output without failing the parse */
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(g_ipv6_google_com_aaaa, sizeof(g_ipv6_google_com_aaaa), FIXTURE_ID, "ipv6.google.com",
        DMDNS_TYPE_AAAA, out, 2, &count, NULL), 0);
    DMOD_TEST_EXPECT_EQ(count, 2u);
}

DMOD_TEST_STEP(parse_survives_a_compression_pointer_loop)
{
    uint8_t msg[DMDNS_MAX_MESSAGE_LEN];
    size_t len = make_response(msg, "x.example", DMDNS_TYPE_A, 0, 0);
    /* Point the question name at itself. */
    msg[DMDNS_HEADER_LEN] = 0xC0;
    msg[DMDNS_HEADER_LEN + 1] = DMDNS_HEADER_LEN;

    dmip_addr_t out[1];
    size_t count = 0;
    DMOD_TEST_EXPECT_EQ(dmdns_parse_response(msg, len, FIXTURE_ID, "x.example", DMDNS_TYPE_A, out, 1, &count, NULL), -EPROTO);
}

/* ============================================================================
 *                              Address literals
 * ========================================================================== */

static bool formats_as(const char* text, const char* expected)
{
    dmip_addr_t addr;
    char buffer[DMDNS_ADDRESS_STRLEN];
    return dmdns_parse_address(text, &addr) == 0
        && dmdns_format_address(&addr, buffer, sizeof(buffer)) == 0
        && strcmp(buffer, expected) == 0;
}

DMOD_TEST_STEP(address_literals_round_trip)
{
    DMOD_TEST_EXPECT_TRUE(formats_as("192.168.1.1", "192.168.1.1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("0.0.0.0", "0.0.0.0"));
    DMOD_TEST_EXPECT_TRUE(formats_as("::1", "::1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("::", "::"));
    DMOD_TEST_EXPECT_TRUE(formats_as("2001:DB8:0:0:0:0:0:1", "2001:db8::1"));
    DMOD_TEST_EXPECT_TRUE(formats_as("1:0:0:2:0:0:0:3", "1:0:0:2::3"));
    DMOD_TEST_EXPECT_TRUE(formats_as("fe80::", "fe80::"));
    DMOD_TEST_EXPECT_TRUE(formats_as("1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"));
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

DMOD_TEST_STEP(format_reports_a_short_buffer)
{
    dmip_addr_t addr = v4(192, 168, 100, 200);
    char buffer[8];
    DMOD_TEST_EXPECT_EQ(dmdns_format_address(&addr, buffer, sizeof(buffer)), -ENOBUFS);
}

/* ============================================================================
 *                                Name servers
 * ========================================================================== */

DMOD_TEST_STEP(static_servers_can_be_added_and_removed)
{
    dmip_addr_t server = v4(8, 8, 8, 8);
    dmip_addr_t out[DMDNS_MAX_SERVERS];

    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), -EEXIST);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(out, DMDNS_MAX_SERVERS), 1u);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &server, sizeof(server)), 0);

    DMOD_TEST_EXPECT_EQ(dmdns_remove_server(&server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_remove_server(&server), -ENOENT);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(out, DMDNS_MAX_SERVERS), 0u);
}

DMOD_TEST_STEP(provided_servers_follow_static_ones_without_duplicates)
{
    dmip_addr_t static_server = v4(1, 1, 1, 1);
    dmip_addr_t out[DMDNS_MAX_SERVERS];
    dmip_addr_t provided[2];
    provided[0] = v4(1, 1, 1, 1);   /* same as the static one */
    provided[1] = v4(192, 168, 100, 1);
    dmdns_test_provider_set(provided, 2);

    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&static_server), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(out, DMDNS_MAX_SERVERS), 2u);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &static_server, sizeof(static_server)), 0);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[1], &provided[1], sizeof(provided[1])), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_get_servers(out, 1), 1u);
}

DMOD_TEST_STEP(server_without_family_is_rejected)
{
    dmip_addr_t server = { 0 };
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(NULL), -EINVAL);
}

/* ============================================================================
 *                           Resolving without network
 * ========================================================================== */

DMOD_TEST_STEP(ip_literal_resolves_to_itself)
{
    dmip_addr_t out[2];
    dmip_addr_t expected = v4(10, 1, 2, 3);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("10.1.2.3", dmip_family_none, out, 2, 0), 1);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &expected, sizeof(expected)), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("10.1.2.3", dmip_family_v6, out, 2, 0), -ENODATA);
}

DMOD_TEST_STEP(localhost_is_built_in)
{
    dmip_addr_t out[1];
    dmip_addr_t expected = v4(127, 0, 0, 1);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("localhost", dmip_family_v4, out, 1, 0), 1);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &expected, sizeof(expected)), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("LocalHost.", dmip_family_v6, out, 1, 0), 1);
    DMOD_TEST_EXPECT_EQ(out[0].family, dmip_family_v6);
    DMOD_TEST_EXPECT_EQ(out[0].addr.v6[15], 1);
}

DMOD_TEST_STEP(hosts_table_answers_before_any_server)
{
    dmip_addr_t printer = v4(192, 168, 1, 50);
    dmip_addr_t out[2];

    DMOD_TEST_EXPECT_EQ(dmdns_add_host("printer.lan", &printer), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_add_host("printer.lan", &printer), -EEXIST);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("PRINTER.lan", dmip_family_none, out, 2, 0), 1);
    DMOD_TEST_EXPECT_EQ(memcmp(&out[0], &printer, sizeof(printer)), 0);

    DMOD_TEST_EXPECT_EQ(dmdns_remove_host("printer.lan"), 0);
    DMOD_TEST_EXPECT_EQ(dmdns_remove_host("printer.lan"), -ENOENT);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("printer.lan", dmip_family_v4, out, 2, 0), -EDESTADDRREQ);
}

DMOD_TEST_STEP(resolve_rejects_bad_arguments)
{
    dmip_addr_t out[1];
    DMOD_TEST_EXPECT_EQ(dmdns_resolve(NULL, dmip_family_v4, out, 1, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("ok.example", dmip_family_v4, NULL, 1, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("ok.example", dmip_family_v4, out, 0, 0), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("not a name", dmip_family_v4, out, 1, 0), -EINVAL);
}

DMOD_TEST_STEP(resolve_without_servers_says_so)
{
    dmip_addr_t out[1];
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("google.com", dmip_family_none, out, 1, 0), -EDESTADDRREQ);
}

DMOD_TEST_STEP(unreachable_server_fails_without_hanging)
{
    /* No interface or route exists in dmod_loader, so the send itself
     * fails - dmdns_query() must report that error (not -ETIMEDOUT after
     * waiting) and release its port. */
    dmip_addr_t server = v4(198, 51, 100, 53);
    dmip_addr_t out[1];
    DMOD_TEST_EXPECT_EQ(dmdns_add_server(&server), 0);

    int result = dmdns_resolve("google.com", dmip_family_v4, out, 1, 50);
    DMOD_TEST_EXPECT_TRUE(result < 0);
    DMOD_TEST_EXPECT_NE(result, -ENOENT);
    DMOD_TEST_EXPECT_NE(result, -ENODATA);

    /* A failed lookup is never cached - the same error comes back from the network again. */
    DMOD_TEST_EXPECT_EQ(dmdns_resolve("google.com", dmip_family_v4, out, 1, 50), result);
}

DMOD_TEST_STEP(query_rejects_bad_arguments)
{
    dmip_addr_t server = v4(198, 51, 100, 53);
    dmip_addr_t no_family = { 0 };
    dmip_addr_t out[1];
    DMOD_TEST_EXPECT_EQ(dmdns_query(NULL, "a.example", DMDNS_TYPE_A, out, 1, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&no_family, "a.example", DMDNS_TYPE_A, out, 1, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&server, "a..example", DMDNS_TYPE_A, out, 1, NULL, 10), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmdns_query(&server, "a.example", DMDNS_TYPE_A, out, 0, NULL, 10), -EINVAL);
}
