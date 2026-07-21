#ifndef DMDMA_H
#define DMDMA_H

#include "dmdma_defs.h"
#include "dmdma_types.h"

/**
 * @brief DMA driver configuration structure
 *
 * One dmdma context (one dmdrvi_create() call) manages exactly one physical
 * DMA controller. `controller` becomes the context's dmdrvi major number,
 * so e.g. controller=0 is exposed as /dev/dmdma0. It is read straight out
 * of the ini config section, the same way dmuart's `instance` key selects
 * which physical UART a context represents - see docs/README.md.
 *
 * Individual streams are not listed here: dmdma_dmdrvi_create() queries the
 * port for how many streams `controller` has and announces each one as a
 * minor device of this same context (dmdrvi_device_available()), so they
 * show up as /dev/dmdmaN/0, /dev/dmdmaN/1, ...
 */
typedef struct
{
    dmdma_controller_t controller; /**< Physical DMA controller this context manages (0-based) */
} dmdma_config_t;

#endif // DMDMA_H
