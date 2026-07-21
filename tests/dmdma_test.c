#include "dmod.h"
#include "dmdma_port.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief dmdma_test - on-target DMA memory-to-memory smoke test.
 *
 * Usage:
 *   dmdma_test <path-to-file>
 *
 * Reads up to DMDMA_TEST_BUFFER_SIZE bytes from the given file and uses
 * DMA2 (the only controller able to do memory-to-memory transfers - see
 * dmdma_port_supports_memory_to_memory()) to copy them to a second buffer,
 * then verifies the copy is byte-for-byte identical.
 *
 * Talks to dmdma_port.h directly (the same Built-in API dmdma.c itself
 * calls), not through the dmdrvi DIF - this exercises the actual
 * register-level DMA logic without needing dmdma's own driver context/config
 * machinery, and without needing to bundle dmdma or dmdrvi/dmini alongside
 * this module - only dmdma_port itself.
 *
 * Bundle dmdma_port.dmf and dmdma_test.dmf together (e.g. drop both into
 * <dmod-boot>/build/dmf/ and reflash - see the dmod-ecosystem skill's
 * "Testing a locally-built module on real hardware" section) and run from
 * the shell: `dmdma_test /path/to/some/file`.
 */

#define DMDMA_TEST_BUFFER_SIZE            4096U
#define DMDMA_TEST_BUSY_POLL_ITERATIONS   1000000UL
#define DMDMA_TEST_CONTROLLER             1U /* DMA2 - mem-to-mem capable */
#define DMDMA_TEST_STREAM                 0U

static uint8_t g_src[DMDMA_TEST_BUFFER_SIZE];
static uint8_t g_dst[DMDMA_TEST_BUFFER_SIZE];

int main(int argc, char *argv[])
{
    Dmod_Printf("\n=== DMDMA On-Target Smoke Test ===\n\n");

    if (argc < 2)
    {
        Dmod_Printf("Usage: dmdma_test <path-to-file>\n");
        Dmod_Printf("Reads up to %u bytes from the given file and verifies\n", DMDMA_TEST_BUFFER_SIZE);
        Dmod_Printf("that DMA2 can copy them byte-for-byte in memory.\n\n");
        return -1;
    }

    const char *path = argv[1];

    Dmod_Printf("Opening '%s'...\n", path);
    void *file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
    {
        Dmod_Printf("ERROR: could not open '%s'\n", path);
        return -1;
    }

    size_t bytes_read = Dmod_FileRead(g_src, 1, sizeof(g_src), file);
    Dmod_FileClose(file);

    if (bytes_read == 0)
    {
        Dmod_Printf("ERROR: '%s' is empty (or could not be read)\n", path);
        return -1;
    }

    Dmod_Printf("Read %u byte(s) from '%s'\n", (unsigned)bytes_read, path);

    for (size_t i = 0; i < sizeof(g_dst); i++)
    {
        g_dst[i] = 0;
    }

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

    Dmod_Printf("Acquiring DMA%u stream %u...\n", DMDMA_TEST_CONTROLLER + 1U, DMDMA_TEST_STREAM);
    if (dmdma_port_stream_acquire(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM) != 0)
    {
        Dmod_Printf("ERROR: failed to acquire stream\n");
        return -1;
    }

    dmdma_transfer_config_t transfer;
    transfer.direction             = dmdma_direction_memory_to_memory;
    transfer.request               = DMDMA_REQUEST_NONE;
    transfer.source_address        = g_src;
    transfer.destination_address   = g_dst;
    transfer.source_width          = dmdma_data_width_byte;
    transfer.destination_width     = dmdma_data_width_byte;
    transfer.source_increment      = true;
    transfer.destination_increment = true;
    transfer.circular              = false;
    transfer.priority              = dmdma_priority_medium;
    transfer.element_count         = bytes_read;

    Dmod_Printf("Starting memory-to-memory transfer of %u byte(s)...\n", (unsigned)bytes_read);
    int ret = dmdma_port_stream_start(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM, &transfer);
    if (ret != 0)
    {
        Dmod_Printf("ERROR: dmdma_port_stream_start() failed (%d)\n", ret);
        dmdma_port_stream_release(DMDMA_TEST_CONTROLLER, DMDMA_TEST_STREAM);
        return -1;
    }

    bool busy = true;
    uint32_t iterations = 0;
    for (; iterations < DMDMA_TEST_BUSY_POLL_ITERATIONS; iterations++)
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

    Dmod_Printf("Transfer complete after %u poll iteration(s), %u byte(s) remaining\n",
                (unsigned)iterations, (unsigned)remaining);

    if (remaining != 0)
    {
        Dmod_Printf("\nFAIL: %u byte(s) reported remaining after completion\n\n", (unsigned)remaining);
        return -1;
    }

    /* dmod's bare-metal string.c (linked into every Application module, see
     * src/module/string.c) doesn't provide memcmp - compare by hand. */
    for (size_t i = 0; i < bytes_read; i++)
    {
        if (g_src[i] != g_dst[i])
        {
            Dmod_Printf("\nFAIL: mismatch at offset %u (src=0x%02X, dst=0x%02X)\n\n",
                        (unsigned)i, g_src[i], g_dst[i]);
            return -1;
        }
    }

    Dmod_Printf("\nPASS: DMA%u copied %u byte(s) correctly\n\n", DMDMA_TEST_CONTROLLER + 1U, (unsigned)bytes_read);
    return 0;
}
