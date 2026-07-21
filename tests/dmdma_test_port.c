#include "dmod.h"
#include "dmdma_port.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief dmdma_test_port - on-target DMA memory-to-memory smoke test,
 *        talking to dmdma_port.h directly.
 *
 * Usage:
 *   dmdma_test_port <path-to-file>
 *
 * Exercises the raw register-level API dmdma.c itself calls
 * (dmdma_port_stream_acquire/_start/_is_busy/_get_remaining/_release) with
 * no dmdevfs/dmdrvi/config involved - only dmdma_port needs to be bundled
 * alongside this module. See dmdma_test_dev.c for the counterpart that goes
 * through the actual device node instead.
 *
 * Reads up to DMDMA_TEST_MAX_BYTES bytes from the given file into a
 * heap-allocated buffer sized to what was actually read (never a fixed
 * multi-KB static buffer - this runs on embedded targets where RAM is
 * scarce) and uses DMA2 (the only controller able to do memory-to-memory
 * transfers - see dmdma_port_supports_memory_to_memory()) to copy them to a
 * second buffer, then verifies the copy is byte-for-byte identical.
 */

#define DMDMA_TEST_MAX_BYTES               512U
#define DMDMA_TEST_BUSY_POLL_ITERATIONS    1000000UL
#define DMDMA_TEST_CONTROLLER              1U /* DMA2 - mem-to-mem capable */
#define DMDMA_TEST_STREAM                  0U

/* Runs the transfer and verifies it; stream_acquire()/_release() are paired
 * within this function so main() doesn't need to track whether the stream
 * was actually reserved on every different failure path. */
static int run_transfer_test(const uint8_t *src, uint8_t *dst, size_t bytes_read)
{
    if (dmdma_port_get_stream_count(DMDMA_TEST_CONTROLLER) == 0)
    {
        Dmod_Printf("ERROR: controller %u not supported by this port\n", DMDMA_TEST_CONTROLLER);
        return -1;
    }
    if (!dmdma_port_supports_memory_to_memory(DMDMA_TEST_CONTROLLER))
    {
        Dmod_Printf("ERROR: controller %u cannot do memory-to-memory transfers\n", DMDMA_TEST_CONTROLLER);
        return -1;
    }
    if (dmdma_port_stream_acquire(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM) != 0)
    {
        Dmod_Printf("ERROR: failed to acquire DMA%u stream %u\n", DMDMA_TEST_CONTROLLER + 1U, DMDMA_TEST_STREAM);
        return -1;
    }

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
    transfer.element_count         = bytes_read;

    Dmod_Printf("Starting memory-to-memory transfer of %u byte(s)...\n", (unsigned)bytes_read);
    if (dmdma_port_stream_start(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM, &transfer) != 0)
    {
        Dmod_Printf("ERROR: dmdma_port_stream_start() failed\n");
        dmdma_port_stream_release(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM);
        return -1;
    }

    bool busy = true;
    for (uint32_t i = 0; i < DMDMA_TEST_BUSY_POLL_ITERATIONS; i++)
    {
        busy = dmdma_port_stream_is_busy(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM);
        if (!busy)
        {
            break;
        }
    }

    size_t remaining = dmdma_port_stream_get_remaining(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM);
    dmdma_port_stream_release(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM);

    if (busy)
    {
        Dmod_Printf("\nFAIL: transfer did not complete (timed out polling is_busy)\n\n");
        return -1;
    }
    if (remaining != 0)
    {
        Dmod_Printf("\nFAIL: %u byte(s) reported remaining after completion\n\n", (unsigned)remaining);
        return -1;
    }

    for (size_t i = 0; i < bytes_read; i++)
    {
        if (src[i] != dst[i])
        {
            Dmod_Printf("\nFAIL: mismatch at offset %u (src=0x%02X, dst=0x%02X)\n\n",
                        (unsigned)i, src[i], dst[i]);
            return -1;
        }
    }

    Dmod_Printf("\nPASS: DMA%u copied %u byte(s) correctly\n\n", DMDMA_TEST_CONTROLLER + 1U, (unsigned)bytes_read);
    return 0;
}

int main(int argc, char *argv[])
{
    Dmod_Printf("\n=== DMDMA Port-Level Smoke Test ===\n\n");

    if (argc < 2)
    {
        Dmod_Printf("Usage: dmdma_test_port <path-to-file>\n");
        Dmod_Printf("Reads up to %u bytes from the given file and verifies\n", DMDMA_TEST_MAX_BYTES);
        Dmod_Printf("that DMA2 can copy them byte-for-byte in memory.\n\n");
        return -1;
    }

    const char *path = argv[1];

    void *file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
    {
        Dmod_Printf("ERROR: could not open '%s'\n", path);
        return -1;
    }

    size_t file_size = Dmod_FileSize(file);
    size_t size = (file_size < DMDMA_TEST_MAX_BYTES) ? file_size : DMDMA_TEST_MAX_BYTES;
    if (size == 0)
    {
        Dmod_FileClose(file);
        Dmod_Printf("ERROR: '%s' is empty\n", path);
        return -1;
    }

    uint8_t *src = Dmod_Malloc(size);
    uint8_t *dst = Dmod_Malloc(size);
    int result = -1;

    if (src == NULL || dst == NULL)
    {
        Dmod_Printf("ERROR: out of memory (%u bytes)\n", (unsigned)size);
    }
    else
    {
        size_t bytes_read = Dmod_FileRead(src, 1, size, file);
        Dmod_Printf("Read %u byte(s) from '%s'\n", (unsigned)bytes_read, path);

        for (size_t i = 0; i < size; i++)
        {
            dst[i] = 0;
        }

        result = run_transfer_test(src, dst, bytes_read);
    }

    Dmod_FileClose(file);
    Dmod_Free(src);
    Dmod_Free(dst);
    return result;
}
