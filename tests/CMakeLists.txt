set(DMOD_MODULE_NAME        dmdma_test)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)

# dmdma_test talks to dmdma_port.h directly (the same Built-in API dmdma.c
# itself calls) - it is an on-target diagnostic Application, not a
# dmod_add_test()/DMOD_TEST_STEP unit test, since it needs argv (the file
# path to copy) and is meant to be run manually from the shell.
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmdma_test.c
)

target_link_libraries(${DMOD_MODULE_NAME}
    dmdma_port_if
)
