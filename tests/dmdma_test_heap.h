#ifndef DMDMA_TEST_HEAP_H
#define DMDMA_TEST_HEAP_H

#include "dmheap.h"

/**
 * @brief Name of the dmheap context these test tools allocate their
 *        transfer buffers from. Must already exist - these tools only
 *        fetch it, they don't set one up.
 */
#define DMDMA_TEST_HEAP_NAME "dma"

#endif // DMDMA_TEST_HEAP_H
