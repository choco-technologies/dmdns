# Test-only Library-type module implementing dmdns's dmdns_provide_servers
# DIF - see include/dmdns_test_provider.h for why test_dmdns can't simply
# implement it itself.
set(DMOD_MODULE_NAME        dmdns_test_provider)

set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)

set(DMOD_MAL_IMPLS          )

set(DMOD_DIF_IMPLS          dmdns)

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/dmdns_test_provider.c
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${DMOD_MODULE_NAME}_if INTERFACE dmdns_if)

# dmip/dmroute/dmnetif: the header chain dmdns.h pulls in.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
)
