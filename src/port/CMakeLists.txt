# ======================================================================
#               dmdma_port Module Configuration
# ======================================================================

# Name of the module
set(DMOD_MODULE_NAME        dmdma_port)

# Version is already set above and used in project()

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the DMR file for this module
set(DMOD_DMR_PATH           ${CMAKE_SOURCE_DIR}/dmdma_port.dmr)

# stm32f4 and stm32f7 share the identical DMA controller IP block (confirmed
# by dnx-rtos shipping one combined "STM32F4F7" driver for both), so the bulk
# of the logic lives in stm32_common and per-family port.c stays thin
# (lifecycle + IRQ vector forwarding only) - see src/port/stm32_common/.
set(COMMON_SOURCES stm32_common/stm32_common.c)

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    ${DMOD_CPU_FAMILY}/port.c
    ${COMMON_SOURCES}
)

# dmdrvi: dmdma_types.h pulls in dmdrvi_ioctl.h (DMDRVI_IOCTL_CUSTOM_BASE)
dmod_link_modules(${DMOD_MODULE_NAME}
    dmosi
    dmdrvi
)

target_include_directories(${DMOD_MODULE_NAME} BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)
