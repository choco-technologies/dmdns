# Name of the module
set(DMOD_MODULE_NAME        nslookup)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer) - dmdns keeps every message,
# address list and string on the heap, so the usual 1024 is enough.
set(DMOD_STACK_SIZE         1024)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/nslookup.dmr)

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/nslookup.c
)

# dmdns is this same repository's own top-level module, already built as
# part of this CMake configure by the time tools/ is added (see the root
# CMakeLists.txt) - link its dmdns_if target directly, exactly like
# tests/CMakeLists.txt does, instead of fetching dmdns's own published
# headers back via dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} dmdns_if)

# dmip:    dmdns.h #includes dmip.h directly (dmip_addr_t, dmip_family_t).
# dmroute: dmip.h needs dmroute_addr_t (re-exported as dmip_addr_t) -
#          dmdns_if alone doesn't carry dmroute's own include dir.
# dmnetif: dmip.h needs dmnetif_iface_t - part of the header chain only,
#          nslookup.c never calls a dmnetif_* function.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
)

add_subdirectory(tests)
