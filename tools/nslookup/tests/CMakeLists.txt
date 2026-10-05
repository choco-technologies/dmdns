set(DMOD_MODULE_NAME        test_nslookup)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    nslookup_test.c
)

# dmdns: dmdns_add_host() to seed a name nslookup can find without a
#        network - and, as a loaded dependency, the library nslookup's own
#        calls resolve against while Dmod_RunModule() runs it.
target_link_libraries(${DMOD_MODULE_NAME} dmdns_if)

# dmip/dmroute/dmnetif: the header chain dmdns.h pulls in.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
)

find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()
