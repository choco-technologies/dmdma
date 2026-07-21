#include "dmod.h"
#include "dmdma_types.h"
#include "dmdma_test_heap.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief dmdma_test_dev - on-target DMA memory-to-memory smoke test,
 *        going through the device node dmdevfs exposes for dmdma.
 *
 * Usage:
 *   dmdma_test_dev <path-to-dmdma-stream-device>
 *   e.g. dmdma_test_dev /dev/dmdma1/0   (DMA2, the mem-to-mem capable
 *                                        controller, stream 0)
 *
 * Unlike dmdma_test_port.c, this does not touch dmdma_port.h at all - it
 * opens the path with the ordinary VFS file interface (open/ioctl/close)
 * and drives it purely through the dmdrvi_ioctl() commands dmdma.c's DIF
 * implementation exposes (dmdma_ioctl_cmd_t in dmdma_types.h), exercising
 * the real production path: dmdevfs must already have mounted and
 * configured dmdma (see configs/) for this path to exist at all.
 */

#define DMDMA_TEST_BYTES                   64U
#define DMDMA_TEST_BUSY_POLL_ITERATIONS    1000000UL

static int run_transfer_test(void *handle, const uint8_t *src, uint8_t *dst)
{
    dmdma_transfer_config_t transfer;
    transfer.direction             = dmdma_direction_memory_to_memory;
    transfer.request               = DMDMA_REQUEST_NONE;
    transfer.source_address        = src;
    transfer.destination_address   = dst;
    transfer.source_width          = dmdma_data_width_byte;
    transfer.destination_width     = dmdma_data_width_byte;
    transfer.source_increment      = true;
    transfer.destination_increment = true;
    transfer.circular              = false;
    transfer.priority              = dmdma_priority_medium;
    transfer.element_count         = DMDMA_TEST_BYTES;

    Dmod_Printf("Starting memory-to-memory transfer of %u byte(s) via ioctl()...\n", DMDMA_TEST_BYTES);
    if (Dmod_Ioctl(handle, dmdma_ioctl_cmd_start_transfer, &transfer) != 0)
    {
        Dmod_Printf("ERROR: dmdma_ioctl_cmd_start_transfer failed\n");
        return -1;
    }

    bool busy = true;
    for (uint32_t i = 0; i < DMDMA_TEST_BUSY_POLL_ITERATIONS; i++)
    {
        if (Dmod_Ioctl(handle, dmdma_ioctl_cmd_is_busy, &busy) != 0)
        {
            Dmod_Printf("ERROR: dmdma_ioctl_cmd_is_busy failed\n");
            return -1;
        }
        if (!busy)
        {
            break;
        }
    }

    if (busy)
    {
        Dmod_Printf("\nFAIL: transfer did not complete (timed out polling is_busy)\n\n");
        return -1;
    }

    size_t remaining = (size_t)-1;
    if (Dmod_Ioctl(handle, dmdma_ioctl_cmd_get_remaining, &remaining) != 0)
    {
        Dmod_Printf("ERROR: dmdma_ioctl_cmd_get_remaining failed\n");
        return -1;
    }
    if (remaining != 0)
    {
        Dmod_Printf("\nFAIL: %u byte(s) reported remaining after completion\n\n", (unsigned)remaining);
        return -1;
    }

    for (uint32_t i = 0; i < DMDMA_TEST_BYTES; i++)
    {
        if (src[i] != dst[i])
        {
            Dmod_Printf("\nFAIL: mismatch at offset %u (src=0x%02X, dst=0x%02X)\n\n", i, src[i], dst[i]);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    Dmod_Printf("\n=== DMDMA Device-Node Smoke Test ===\n\n");

    if (argc < 2)
    {
        Dmod_Printf("Usage: dmdma_test_dev <path-to-dmdma-stream-device>\n");
        Dmod_Printf("e.g. dmdma_test_dev /dev/dmdma1/0  (DMA2 stream 0)\n\n");
        Dmod_Printf("Opens the given device node through dmdevfs/dmdrvi and\n");
        Dmod_Printf("verifies a memory-to-memory transfer through it\n");
        Dmod_Printf("completes and copies correctly.\n\n");
        return -1;
    }

    const char *path = argv[1];

    Dmod_Printf("Opening '%s'...\n", path);
    void *handle = Dmod_FileOpen(path, "r+");
    if (handle == NULL)
    {
        Dmod_Printf("ERROR: could not open '%s' (is dmdevfs mounted and dmdma configured?)\n", path);
        return -1;
    }

    dmheap_context_t *heap = dmheap_get_context_by_name(DMDMA_TEST_HEAP_NAME);
    if (heap == NULL)
    {
        Dmod_FileClose(handle);
        Dmod_Printf("ERROR: no dmheap context named '%s' found\n", DMDMA_TEST_HEAP_NAME);
        return -1;
    }

    uint8_t *src = dmheap_malloc(heap, DMDMA_TEST_BYTES, DMDMA_TEST_HEAP_NAME);
    uint8_t *dst = dmheap_malloc(heap, DMDMA_TEST_BYTES, DMDMA_TEST_HEAP_NAME);
    int result = -1;

    if (src == NULL || dst == NULL)
    {
        Dmod_Printf("ERROR: out of memory (%u bytes from the '%s' heap)\n", DMDMA_TEST_BYTES, DMDMA_TEST_HEAP_NAME);
    }
    else
    {
        for (uint32_t i = 0; i < DMDMA_TEST_BYTES; i++)
        {
            src[i] = (uint8_t)((i * 7U) + 1U);
            dst[i] = 0;
        }

        result = run_transfer_test(handle, src, dst);
        if (result == 0)
        {
            Dmod_Printf("\nPASS: '%s' copied %u byte(s) correctly\n\n", path, DMDMA_TEST_BYTES);
        }
    }

    dmheap_free(heap, src, true);
    dmheap_free(heap, dst, true);
    Dmod_FileClose(handle);
    return result;
}
