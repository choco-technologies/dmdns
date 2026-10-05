/**
 * @file nslookup_test.c
 * @brief Test steps for nslookup
 *
 * Drives the actual `nslookup` module through Dmod_RunModule("nslookup",
 * argc, argv) (loads it, runs its main(), unloads it again) rather than
 * depending on it as a linked module: an Application-type module can't be
 * loaded as another module's dependency - same reasoning dmicmp's
 * tools/ping/tests/ping_test.c documents for itself.
 *
 * No name server is reachable from dmod_loader, so successful lookups are
 * served by the hosts table (seeded here through dmdns_add_host()) and IP
 * literals; the network path is only checked to fail honestly.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmdns.h"

void dmod_test_setup(void)
{
    dmip_addr_t addr = { 0 };
    addr.family = dmip_family_v4;
    addr.addr.v4[0] = 192; addr.addr.v4[1] = 168; addr.addr.v4[2] = 1; addr.addr.v4[3] = 50;
    dmdns_add_host("printer.lan", &addr);
    dmdns_clear_servers();
}

void dmod_test_teardown(void)
{
    dmdns_remove_host("printer.lan");
}

DMOD_TEST_STEP(no_args_fails)
{
    char* argv[] = { "nslookup" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 1, argv), 0);
}

DMOD_TEST_STEP(help_flag_succeeds)
{
    char* argv[] = { "nslookup", "--help" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("nslookup", 2, argv), 0);
    DMOD_TEST_EXPECT_FALSE(Dmod_IsModuleLoaded("nslookup"));
}

DMOD_TEST_STEP(hosts_entry_is_found)
{
    char* argv[] = { "nslookup", "printer.lan" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("nslookup", 2, argv), 0);
}

DMOD_TEST_STEP(hosts_entry_has_no_ipv6_address)
{
    char* argv[] = { "nslookup", "-6", "printer.lan" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 3, argv), 0);
}

DMOD_TEST_STEP(localhost_and_literals_need_no_server)
{
    char* local[] = { "nslookup", "localhost" };
    char* literal[] = { "nslookup", "-4", "10.0.0.1" };
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("nslookup", 2, local), 0);
    DMOD_TEST_EXPECT_EQ(Dmod_RunModule("nslookup", 3, literal), 0);
}

DMOD_TEST_STEP(unknown_name_without_server_fails)
{
    char* argv[] = { "nslookup", "google.com" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 2, argv), 0);
}

DMOD_TEST_STEP(unreachable_explicit_server_fails)
{
    char* argv[] = { "nslookup", "-t", "50", "google.com", "198.51.100.53" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 5, argv), 0);
}

DMOD_TEST_STEP(invalid_arguments_fail)
{
    char* bad_server[] = { "nslookup", "google.com", "not-an-ip" };
    char* bad_timeout[] = { "nslookup", "-t", "0", "google.com" };
    char* both_families_off[] = { "nslookup", "-4", "-6", "google.com" };
    char* unknown_flag[] = { "nslookup", "--frobnicate", "google.com" };
    char* too_many[] = { "nslookup", "a.example", "1.1.1.1", "extra" };
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 3, bad_server), 0);
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 4, bad_timeout), 0);
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 4, both_families_off), 0);
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 3, unknown_flag), 0);
    DMOD_TEST_EXPECT_NE(Dmod_RunModule("nslookup", 4, too_many), 0);
}
