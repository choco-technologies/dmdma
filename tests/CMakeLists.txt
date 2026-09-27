set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)

# Three independent on-target diagnostics (Applications, not
# dmod_add_test()/DMOD_TEST_STEP unit tests - all need argv and are meant
# to be run manually from the shell). dmdma_test_port/_dev allocate their
# transfer buffers from the existing dmheap context named "dma"
# (dmheap_get_context_by_name(), added in dmheap 0.6 - see
# dmdma_test_heap.h) - neither sets that context up itself, so both require
# a board whose linker script actually carves out a "dma" MEMORY region
# (see linker/common.ld in dmod-boot) - not every board does.

# dmdma_test_port talks to dmdma_port.h directly (the same Built-in API
# dmdma.c itself calls) - only dmdma_port needs to be bundled alongside it,
# no dmdevfs/dmdrvi/config involved.
dmod_add_executable(dmdma_test_port ${DMOD_MODULE_VERSION}
    dmdma_test_port.c
)

target_link_libraries(dmdma_test_port
    dmdma_port_if
)

dmod_link_modules(dmdma_test_port
    dmheap@>=0.6
)

# dmdma_test_dev instead opens the real device node dmdevfs exposes once
# dmdma is configured (e.g. /dev/dmdma1/0) and drives it purely through the
# VFS file interface (open/ioctl/close) - exercising the real dmdrvi DIF
# wiring end to end, not just the port.
dmod_add_executable(dmdma_test_dev ${DMOD_MODULE_VERSION}
    dmdma_test_dev.c
)

target_include_directories(dmdma_test_dev PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

dmod_link_modules(dmdma_test_dev
    dmheap@>=0.6
)

# dmdma_test_lease exercises the dmdma_lease.h Module API the way a
# peripheral driver (dmuart, dmspi, dmsdio, ...) would: dmdma_lease_*()
# called directly, patched by the loader against dmdma's own registration
# the same way dmdma_port_stream_start() is - no device node, no runtime
# lookup. It links against dmdma_if (the same interface target
# dmdma_test_port uses for dmdma_port_if) to get dmdma.h/dmdma_defs.h on its
# include path - the runtime dependency on dmdma itself is picked up
# automatically (todmd derives it straight from the "@dmdma:" signatures
# dmdma_lease.h's dmod_dmdma_api() calls embed into this binary's own
# .dmod.outputs, the same way it derives dmdma's own dependency on
# dmdma_port). Its transfer buffers are plain static arrays (not the "dma"
# dmheap context dmdma_test_port/_dev need) so it runs on any board
# regardless of whether one has been carved out.
dmod_add_executable(dmdma_test_lease ${DMOD_MODULE_VERSION}
    dmdma_test_lease.c
)

target_link_libraries(dmdma_test_lease
    dmdma_if
)
