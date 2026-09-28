#include "dmod.h"
#include "dmdma.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief dmdma_test_lease - on-target smoke test for the DMA lease API
 *        (dmdma_lease.h), exercised exactly the way a peripheral driver
 *        (dmuart, dmspi, dmsdio, ...) is meant to use it: dmdma_lease_*()
 *        called directly, like any other Module API function (see
 *        dmdma_lease.h) - no runtime lookup involved.
 *
 * Usage:
 *   dmdma_test_lease <controller> <stream>
 *   e.g. dmdma_test_lease 1 0   (DMA2, the mem-to-mem capable controller,
 *                                stream 0)
 *
 * Unlike dmdma_test_dev.c (device node / dmdrvi) and dmdma_test_port.c (raw
 * dmdma_port.h), this tool never touches dmdevfs, dmdrvi, or dmdma_port -
 * it links directly against dmdma (see tests/CMakeLists.txt). It also
 * doesn't need dmdma_test_dev/_test_port's shared "dma" dmheap context - a
 * plain static buffer is a perfectly valid memory-to-memory DMA endpoint,
 * and not every board carves out a dedicated DMA heap region (see
 * linker/common.ld's optional "dma" MEMORY region).
 */

/* Large enough that a circular transfer's wrap period is milliseconds, not
 * microseconds - a tiny buffer wraps (and re-fires dmdma_event_complete) so
 * fast in circular mode that the resulting interrupt storm can starve every
 * other task on the board (confirmed on real STM32F746G-DISCO hardware:
 * a 64-byte circular buffer here reliably wedged the system). The one-shot
 * completion test earlier only ever fires a single interrupt regardless of
 * size, so this size increase is free for it too. */
#define DMDMA_TEST_BYTES                   16384U
#define DMDMA_TEST_BUSY_POLL_ITERATIONS    1000000UL
#define DMDMA_TEST_TIMEOUT_MS              50U
#define DMDMA_TEST_TIMEOUT_MARGIN_MS       100U

static volatile dmdma_event_t s_last_event     = (dmdma_event_t)0;
static volatile int           s_callback_count = 0;
static void                  *s_expected_user_ptr = NULL;
static volatile bool          s_user_ptr_mismatch = false;

static void on_lease_event(dmdma_lease_t lease, dmdma_event_t event, void *user_ptr)
{
    (void)lease;
    s_last_event = event;
    s_callback_count++;
    if (user_ptr != s_expected_user_ptr)
    {
        s_user_ptr_mismatch = true;
    }
}

/* This tool targets DMOD_USE_STDLIB=OFF builds (see cortex-m7's
 * tools-cfg.cmake) - no atoi()/strtoul(), so argv is parsed by hand. */
static uint32_t parse_uint(const char *s)
{
    uint32_t value = 0;
    while (*s >= '0' && *s <= '9')
    {
        value = (value * 10U) + (uint32_t)(*s - '0');
        s++;
    }
    return value;
}

static bool wait_until_idle(dmdma_lease_t lease)
{
    for (uint32_t i = 0; i < DMDMA_TEST_BUSY_POLL_ITERATIONS; i++)
    {
        if (!dmdma_lease_is_busy(lease))
        {
            return true;
        }
    }
    return false;
}

static uint32_t s_src_words[DMDMA_TEST_BYTES / 4U];
static uint32_t s_dst_words[DMDMA_TEST_BYTES / 4U];
#define s_src ((uint8_t *)s_src_words)   /* word aligned for the FIFO/burst run */
#define s_dst ((uint8_t *)s_dst_words)

/* Invalid stream option combinations must be rejected before touching the port. */
static bool check_option_rejections(dmdma_lease_t lease, const dmdma_transfer_config_t *words)
{
    dmdma_stream_options_t opt = { dmdma_flow_controller_dma, dmdma_fifo_direct, dmdma_burst_4, dmdma_burst_4 };
    if (dmdma_lease_start_ex(lease, words, &opt) == 0)
    {
        Dmod_Printf("FAIL: burst in direct mode was accepted\n");
        return false;
    }
    opt.fifo_threshold = dmdma_fifo_half;   /* 4 words x 4 bytes = 16 > 8 */
    if (dmdma_lease_start_ex(lease, words, &opt) == 0)
    {
        Dmod_Printf("FAIL: memory burst larger than the FIFO threshold was accepted\n");
        return false;
    }
    opt.fifo_threshold  = dmdma_fifo_full;
    opt.flow_controller = dmdma_flow_controller_peripheral;
    if (dmdma_lease_start_ex(lease, words, &opt) == 0)
    {
        Dmod_Printf("FAIL: peripheral flow control for memory-to-memory was accepted\n");
        return false;
    }
    Dmod_Printf("PASS: invalid stream options correctly rejected\n");
    return true;
}

/* FIFO mode, full threshold, 4-beat word bursts on both sides (the setup
 * SDIO/SDMMC needs, minus the peripheral flow control mem-to-mem can't use). */
static bool check_fifo_burst_transfer(dmdma_lease_t lease, const dmdma_transfer_config_t *bytes)
{
    dmdma_transfer_config_t words = *bytes;
    words.source_width      = dmdma_data_width_word;
    words.destination_width = dmdma_data_width_word;
    words.element_count     = DMDMA_TEST_BYTES / 4U;
    for (uint32_t i = 0; i < DMDMA_TEST_BYTES / 4U; i++)
    {
        s_src_words[i] = (i * 2654435761U) ^ 0xA5A5A5A5U;
        s_dst_words[i] = 0;
    }
    if (!check_option_rejections(lease, &words))
    {
        return false;
    }
    dmdma_stream_options_t opt = { dmdma_flow_controller_dma, dmdma_fifo_full, dmdma_burst_4, dmdma_burst_4 };
    if (dmdma_lease_start_ex(lease, &words, &opt) != 0 || !wait_until_idle(lease))
    {
        Dmod_Printf("FAIL: FIFO/burst transfer did not start or complete\n");
        return false;
    }
    for (uint32_t i = 0; i < DMDMA_TEST_BYTES / 4U; i++)
    {
        if (s_src_words[i] != s_dst_words[i])
        {
            Dmod_Printf("FAIL: FIFO/burst mismatch at word %u\n", i);
            return false;
        }
    }
    Dmod_Printf("PASS: FIFO mode with 4-beat bursts copied %u byte(s) correctly\n", DMDMA_TEST_BYTES);
    return true;
}

int main(int argc, char *argv[])
{
    Dmod_Printf("\n=== DMDMA Lease API Smoke Test ===\n\n");

    if (argc < 3)
    {
        Dmod_Printf("Usage: dmdma_test_lease <controller> <stream>\n");
        Dmod_Printf("e.g. dmdma_test_lease 1 0  (DMA2 stream 0 - mem-to-mem capable)\n\n");
        return -1;
    }

    dmdma_controller_t controller = (dmdma_controller_t)parse_uint(argv[1]);
    dmdma_stream_t stream = (dmdma_stream_t)parse_uint(argv[2]);

    uint8_t *src = s_src;
    uint8_t *dst = s_dst;
    dmdma_lease_t lease = NULL;
    int result = -1;

    for (uint32_t i = 0; i < DMDMA_TEST_BYTES; i++)
    {
        src[i] = (uint8_t)((i * 5U) + 3U);
        dst[i] = 0;
    }

    /* --- 1. Acquire --- */

    lease = dmdma_lease_acquire(controller, stream);
    if (lease == NULL)
    {
        Dmod_Printf("ERROR: could not acquire controller %u stream %u\n", (unsigned)controller, (unsigned)stream);
        goto cleanup;
    }
    Dmod_Printf("Acquired lease on controller %u stream %u\n", (unsigned)controller, (unsigned)stream);

    /* --- 2. The same stream cannot be acquired twice, through any API combination --- */

    {
        dmdma_lease_t second = dmdma_lease_acquire(controller, stream);
        if (second != NULL)
        {
            Dmod_Printf("FAIL: double-acquire of controller %u stream %u unexpectedly succeeded\n",
                        (unsigned)controller, (unsigned)stream);
            dmdma_lease_release(second);
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: double-acquire correctly rejected\n");
    }

    /* --- 3. Idle abort must not fire a callback - nothing was in flight --- */

    {
        int idle_marker = 0xAAAA;
        s_expected_user_ptr = &idle_marker;
        s_callback_count = 0;
        dmdma_lease_set_callback(lease, on_lease_event, &idle_marker);
        dmdma_lease_abort(lease);
        if (s_callback_count != 0)
        {
            Dmod_Printf("FAIL: aborting an idle stream fired %d callback(s), expected 0\n", s_callback_count);
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: aborting an idle stream fired no callback\n");
    }

    /* --- 4. A real transfer completes, copies correctly, and notifies the right owner --- */

    {
        int user_marker = 0x1234;
        s_expected_user_ptr = &user_marker;
        s_callback_count = 0;
        s_user_ptr_mismatch = false;
        dmdma_lease_set_callback(lease, on_lease_event, &user_marker);

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
        transfer.timeout_ms            = 0; /* no watchdog for this run */

        Dmod_Printf("Starting memory-to-memory transfer of %u byte(s) via the lease API...\n", DMDMA_TEST_BYTES);
        if (dmdma_lease_start(lease, &transfer) != 0)
        {
            Dmod_Printf("ERROR: dmdma_lease_start() failed\n");
            goto cleanup_lease;
        }

        if (!wait_until_idle(lease))
        {
            Dmod_Printf("FAIL: transfer did not complete (timed out polling is_busy)\n");
            goto cleanup_lease;
        }

        size_t remaining = dmdma_lease_get_remaining(lease);
        if (remaining != 0)
        {
            Dmod_Printf("FAIL: %u byte(s) reported remaining after completion\n", (unsigned)remaining);
            goto cleanup_lease;
        }

        for (uint32_t i = 0; i < DMDMA_TEST_BYTES; i++)
        {
            if (src[i] != dst[i])
            {
                Dmod_Printf("FAIL: mismatch at offset %u (src=0x%02X, dst=0x%02X)\n", i, src[i], dst[i]);
                goto cleanup_lease;
            }
        }

        if (s_callback_count < 1 || (s_last_event & dmdma_event_complete) == 0)
        {
            Dmod_Printf("FAIL: completion callback did not fire with dmdma_event_complete (count=%d, last_event=0x%X)\n",
                        s_callback_count, (unsigned)s_last_event);
            goto cleanup_lease;
        }
        if (s_user_ptr_mismatch)
        {
            Dmod_Printf("FAIL: callback received the wrong user_ptr\n");
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: transfer completed, data verified, callback fired with the right user_ptr\n");

        /* --- 5. Malformed configs must be rejected before touching hardware --- */

        dmdma_transfer_config_t bad = transfer;
        bad.element_count = 0;
        if (dmdma_lease_start(lease, &bad) == 0)
        {
            Dmod_Printf("FAIL: zero-length transfer was accepted\n");
            goto cleanup_lease;
        }

        bad = transfer;
        bad.request = 1; /* mem-to-mem must not carry a request line */
        if (dmdma_lease_start(lease, &bad) == 0)
        {
            Dmod_Printf("FAIL: mem-to-mem transfer with a request line was accepted\n");
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: malformed transfer configs correctly rejected\n");

        /* --- 5b. Stream options: FIFO mode and bursts (dmdma_lease_start_ex) --- */

        if (!check_fifo_burst_transfer(lease, &transfer))
        {
            goto cleanup_lease;
        }

        /* --- 6. Abort while a transfer is genuinely in flight notifies the owner --- */
        /*
         * Circular transfers never self-clear their enable bit - is_busy()
         * stays true until something explicitly stops the stream - so
         * unlike a one-shot transfer, there is no race between "did it
         * finish on its own" and "did our abort() call catch it in flight".
         */
        transfer.circular = true;
        if (dmdma_lease_start(lease, &transfer) != 0)
        {
            Dmod_Printf("ERROR: dmdma_lease_start() failed on the abort run\n");
            goto cleanup_lease;
        }
        if (!dmdma_lease_is_busy(lease))
        {
            Dmod_Printf("FAIL: circular transfer was not busy right after starting\n");
            goto cleanup_lease;
        }

        s_callback_count = 0;
        s_last_event = (dmdma_event_t)0;
        dmdma_lease_abort(lease);

        if (dmdma_lease_is_busy(lease))
        {
            Dmod_Printf("FAIL: stream still busy right after dmdma_lease_abort()\n");
            goto cleanup_lease;
        }
        if (s_callback_count < 1 || s_last_event != dmdma_event_aborted)
        {
            Dmod_Printf("FAIL: abort did not fire a dmdma_event_aborted callback (count=%d, last_event=0x%X)\n",
                        s_callback_count, (unsigned)s_last_event);
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: abort correctly stopped the stream and notified the owner\n");

        /* --- 7. A watchdog (timeout_ms) auto-aborts and reports dmdma_event_timeout --- */
        /*
         * Same circular trick as the abort test above - it never completes
         * on its own, so the only way it can stop is the dmosi_timer watchdog
         * armed by dmdma_lease_start() firing.
         */
        transfer.timeout_ms = DMDMA_TEST_TIMEOUT_MS;
        if (dmdma_lease_start(lease, &transfer) != 0)
        {
            Dmod_Printf("ERROR: dmdma_lease_start() failed on the timeout run\n");
            goto cleanup_lease;
        }

        s_callback_count = 0;
        s_last_event = (dmdma_event_t)0;
        Dmod_ThreadSleep(DMDMA_TEST_TIMEOUT_MS + DMDMA_TEST_TIMEOUT_MARGIN_MS);

        if (dmdma_lease_is_busy(lease))
        {
            Dmod_Printf("FAIL: stream still busy %u ms after its %u ms watchdog should have fired\n",
                        DMDMA_TEST_TIMEOUT_MS + DMDMA_TEST_TIMEOUT_MARGIN_MS, DMDMA_TEST_TIMEOUT_MS);
            goto cleanup_lease;
        }
        if (s_callback_count < 1 || s_last_event != dmdma_event_timeout)
        {
            Dmod_Printf("FAIL: watchdog did not fire a dmdma_event_timeout callback (count=%d, last_event=0x%X)\n",
                        s_callback_count, (unsigned)s_last_event);
            goto cleanup_lease;
        }
        Dmod_Printf("PASS: watchdog auto-stopped the stream and reported dmdma_event_timeout\n");
    }

    /* --- 8. Release must return the stream to the free pool cleanly --- */

    dmdma_lease_release(lease);
    lease = dmdma_lease_acquire(controller, stream);
    if (lease == NULL)
    {
        Dmod_Printf("FAIL: could not re-acquire controller %u stream %u after release\n",
                    (unsigned)controller, (unsigned)stream);
        goto cleanup;
    }
    Dmod_Printf("PASS: stream cleanly re-acquired after release\n");

    result = 0;
    Dmod_Printf("\nPASS: all dmdma lease API checks passed on controller %u stream %u\n\n",
                (unsigned)controller, (unsigned)stream);

cleanup_lease:
    if (lease != NULL)
    {
        dmdma_lease_release(lease);
    }
cleanup:
    return result;
}
