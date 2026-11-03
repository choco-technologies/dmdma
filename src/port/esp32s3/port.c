#define DMOD_ENABLE_REGISTRATION ON
#include "dmdma_port.h"
#include "dmod.h"
#include "esp32s3_gdma.h"
#include <errno.h>
#include <string.h>

/*
 * dmdma_port for the ESP32-S3: the GDMA, one controller (0) with 5 streams; stream n is the channel pair n. The direction of a transfer
 * chooses the side: memory -> peripheral uses the OUT side, peripheral -> memory the IN side, memory -> memory both (the IN side takes
 * what the OUT side of the same channel sends).
 *
 * The GDMA works on linked lists of descriptors in the internal RAM, 4092 bytes at most each: a transfer is cut into them, and the
 * descriptors come from the heap tagged "dma" (the internal RAM the DMA reaches). The peripheral is not addressed by a register:
 * `request` is its PERI_SEL (SPI2 0, SPI3 1, UHCI0 2, I2S0 3, I2S1 4, LCD_CAM 5, AES 6, SHA 7, ADC 8, RMT 9) and the address on the
 * peripheral side of the transfer config is ignored. The memory buffers have to be in the internal RAM (the external RAM is behind the
 * cache); source and destination widths are equal (the GDMA moves bytes); the options of the STM32 DMA (flow controller of the
 * peripheral, FIFO, bursts) are not supported (-ENOTSUP).
 *
 * The end of a transfer is read from the owner bits of the descriptors: the GDMA clears the owner bit of a descriptor it has finished
 * (the IN side always, the OUT side with auto write-back). A circular transfer is a ring of an even number of descriptors; the first
 * half finished is dmdma_event_half_complete, the second one dmdma_event_complete, and a finished half is given back to the GDMA.
 * get_remaining() has the granularity of a descriptor.
 */

typedef struct
{
    bool                acquired;
    volatile bool       running;
    bool                circular;
    dmdma_direction_t   direction;
    gdma_desc_t        *tx;                 /* the OUT chain (the source of a memory -> peripheral or memory -> memory transfer) */
    gdma_desc_t        *rx;                 /* the IN chain (the destination of a peripheral -> memory or memory -> memory transfer) */
    uint32_t            count;              /* descriptors in a chain */
    uint32_t            width;
    int                 peri;               /* PERI_SEL in use, -1 none */
} stream_t;

static stream_t streams[ESP32S3_GDMA_CHANNELS];
static unsigned acquired_streams;
static dmdma_port_interrupt_handler_t irq_handler;
static void *irq_user_ptr;

static inline volatile uint32_t *reg(uintptr_t address)
{
    return (volatile uint32_t *)address;
}

static inline uint32_t intenable_read(void)
{
    uint32_t v;
    __asm__ __volatile__("rsr.intenable %0" : "=a"(v));
    return v;
}

static inline void intenable_write(uint32_t v)
{
    __asm__ __volatile__("wsr.intenable %0; rsync" ::"a"(v) : "memory");
}

static bool valid(dmdma_controller_t controller, dmdma_stream_t stream)
{
    return controller == 0U && stream < ESP32S3_GDMA_CHANNELS;
}

static bool internal_ram(const void *address, size_t size)
{
    uintptr_t start = (uintptr_t)address;
    return start >= ESP32S3_INTERNAL_RAM_START && start < ESP32S3_INTERNAL_RAM_END && size <= ESP32S3_INTERNAL_RAM_END - start;
}

static void free_chains(stream_t *s)
{
    if (s->tx != NULL)
        Dmod_Free(s->tx);
    if (s->rx != NULL)
        Dmod_Free(s->rx);
    s->tx = NULL;
    s->rx = NULL;
    s->count = 0;
}

/* The owner bits of the descriptors [from, to) of the chain are all cleared: the GDMA has finished them */
static bool range_done(const gdma_desc_t *chain, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++)
    {
        if (chain[i].dw0 & GDMA_DESC_OWNER)
            return false;
    }
    return true;
}

static void rearm(gdma_desc_t *chain, uint32_t from, uint32_t to, bool receive)
{
    for (uint32_t i = from; i < to; i++)
    {
        uint32_t size = chain[i].dw0 & 0xFFFU;
        chain[i].dw0 = size | (receive ? 0U : size << 12) | GDMA_DESC_OWNER;
    }
}

/* Cuts `bytes` at `buffer` into `count` descriptors (equal parts, the last one takes the rest); `next` of the last one loops when circular */
static void build_chain(gdma_desc_t *chain, uint32_t count, const uint8_t *buffer, uint32_t bytes, bool receive, bool circular)
{
    uint32_t part = (bytes + count - 1U) / count;
    uint32_t done = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t size = bytes - done < part ? bytes - done : part;
        chain[i].dw0 = size | (receive ? 0U : size << 12) | GDMA_DESC_OWNER;
        chain[i].buffer = (uint32_t)(uintptr_t)(buffer + done);
        chain[i].next = i + 1U < count ? (uint32_t)(uintptr_t)&chain[i + 1U] : (circular ? (uint32_t)(uintptr_t)&chain[0] : 0U);
        done += size;
    }
    if (!circular)
        chain[count - 1U].dw0 |= GDMA_DESC_SUC_EOF;
}

/* A memory-to-memory transfer needs the PERI_SEL of some peripheral that can do it (the GDMA ignores the requests of the others), one that
   no other running stream is bound to: the highest one that is free */
static uint32_t m2m_peri(unsigned stream)
{
    for (int id = (int)GDMA_PERI_MAX; id > 0; id--)
    {
        bool used = false;
        for (unsigned ch = 0; ch < ESP32S3_GDMA_CHANNELS; ch++)
        {
            if (ch != stream && streams[ch].running && streams[ch].peri == id)
                used = true;
        }
        if (!used)
            return (uint32_t)id;
    }
    return 0U;
}

static void channel_halt(unsigned ch)
{
    *reg(GDMA_REG(ch, GDMA_IN_LINK)) |= GDMA_INLINK_STOP;
    *reg(GDMA_REG(ch, GDMA_OUT_LINK)) |= GDMA_OUTLINK_STOP;
    *reg(GDMA_REG(ch, GDMA_IN_INT_ENA)) = 0;
    *reg(GDMA_REG(ch, GDMA_OUT_INT_ENA)) = 0;
    *reg(GDMA_REG(ch, GDMA_IN_CONF0)) |= GDMA_CONF0_RST;
    *reg(GDMA_REG(ch, GDMA_IN_CONF0)) &= ~GDMA_CONF0_RST;
    *reg(GDMA_REG(ch, GDMA_OUT_CONF0)) |= GDMA_CONF0_RST;
    *reg(GDMA_REG(ch, GDMA_OUT_CONF0)) &= ~GDMA_CONF0_RST;
    *reg(GDMA_REG(ch, GDMA_IN_INT_CLR)) = GDMA_IN_INT_FIFO_ALL;
    *reg(GDMA_REG(ch, GDMA_OUT_INT_CLR)) = GDMA_OUT_INT_FIFO_ALL;
}

static void controller_enable(bool enable)
{
    if (enable)
    {
        Dmod_EnterCritical();
        *reg(ESP32S3_PERIP_CLK_EN1_REG) |= ESP32S3_GDMA_PERIP_BIT;
        *reg(ESP32S3_PERIP_RST_EN1_REG) |= ESP32S3_GDMA_PERIP_BIT;
        *reg(ESP32S3_PERIP_RST_EN1_REG) &= ~ESP32S3_GDMA_PERIP_BIT;
        *reg(GDMA_MISC_CONF) |= GDMA_MISC_CLK_EN;
        for (unsigned ch = 0; ch < ESP32S3_GDMA_CHANNELS; ch++)
        {
            *reg(ESP32S3_INTMTX_MAP_REG(ESP32S3_GDMA_IN_SOURCE(ch))) = ESP32S3_GDMA_CPU_INTR;
            *reg(ESP32S3_INTMTX_MAP_REG(ESP32S3_GDMA_OUT_SOURCE(ch))) = ESP32S3_GDMA_CPU_INTR;
        }
        intenable_write(intenable_read() | (1UL << ESP32S3_GDMA_CPU_INTR));
        Dmod_ExitCritical();
    }
    else
    {
        Dmod_EnterCritical();
        intenable_write(intenable_read() & ~(1UL << ESP32S3_GDMA_CPU_INTR));
        *reg(GDMA_MISC_CONF) &= ~GDMA_MISC_CLK_EN;
        *reg(ESP32S3_PERIP_CLK_EN1_REG) &= ~ESP32S3_GDMA_PERIP_BIT;
        Dmod_ExitCritical();
    }
}

dmod_dmdma_port_api_declaration(1.0, uint8_t, _get_stream_count, (dmdma_controller_t controller))
{
    return controller == 0U ? ESP32S3_GDMA_CHANNELS : 0U;
}

dmod_dmdma_port_api_declaration(1.0, bool, _supports_memory_to_memory, (dmdma_controller_t controller))
{
    return controller == 0U;
}

dmod_dmdma_port_api_declaration(1.0, int, _stream_acquire, (dmdma_controller_t controller, dmdma_stream_t stream))
{
    if (!valid(controller, stream))
        return -EINVAL;
    stream_t *s = &streams[stream];
    if (s->acquired)
        return -EBUSY;
    if (acquired_streams == 0U)
        controller_enable(true);
    acquired_streams++;
    memset(s, 0, sizeof(*s));
    s->peri = -1;
    s->acquired = true;
    channel_halt(stream);
    return 0;
}

dmod_dmdma_port_api_declaration(1.0, void, _stream_release, (dmdma_controller_t controller, dmdma_stream_t stream))
{
    if (!valid(controller, stream) || !streams[stream].acquired)
        return;
    stream_t *s = &streams[stream];
    s->running = false;
    channel_halt(stream);
    free_chains(s);
    s->acquired = false;
    if (--acquired_streams == 0U)
        controller_enable(false);
}

dmod_dmdma_port_api_declaration(1.0, int, _stream_start_ex,
                                (dmdma_controller_t controller, dmdma_stream_t stream, const dmdma_transfer_config_t *config,
                                 const dmdma_stream_options_t *options))
{
    if (!valid(controller, stream) || config == NULL)
        return -EINVAL;
    stream_t *s = &streams[stream];
    if (!s->acquired)
        return -ENODEV;
    if (options != NULL && (options->flow_controller != dmdma_flow_controller_dma || options->fifo_threshold != dmdma_fifo_direct ||
                            options->source_burst != dmdma_burst_single || options->destination_burst != dmdma_burst_single))
        return -ENOTSUP;

    bool m2m = config->direction == dmdma_direction_memory_to_memory;
    bool m2p = config->direction == dmdma_direction_memory_to_peripheral;
    uint32_t width = (uint32_t)config->source_width;
    if (config->element_count == 0U || config->source_width != config->destination_width)
        return -EINVAL;
    if (config->element_count > 0x40000U / width)
        return -EINVAL;
    uint32_t bytes = (uint32_t)config->element_count * width;
    if (!m2m && config->request > GDMA_PERI_MAX)
        return -EINVAL;
    if (m2m && config->circular)
        return -ENOTSUP;

    /* the memory side(s): in the internal RAM, incrementing (the peripheral side has no address) */
    const uint8_t *src = m2m || m2p ? (const uint8_t *)config->source_address : NULL;
    uint8_t *dst = m2m || !m2p ? (uint8_t *)config->destination_address : NULL;
    if ((src != NULL && (!internal_ram(src, bytes) || !config->source_increment)) ||
        (dst != NULL && (!internal_ram(dst, bytes) || !config->destination_increment)))
        return (src != NULL && !config->source_increment) || (dst != NULL && !config->destination_increment) ? -ENOTSUP : -EINVAL;
    if ((m2m || m2p) && src == NULL)
        return -EINVAL;
    if ((m2m || !m2p) && dst == NULL)
        return -EINVAL;

    /* a transfer that is running is replaced */
    s->running = false;
    channel_halt(stream);
    free_chains(s);

    uint32_t count = (bytes + GDMA_DESC_SIZE_MAX - 1U) / GDMA_DESC_SIZE_MAX;
    if (config->circular)
        count = count < 2U ? 2U : (count + (count & 1U));          /* a ring of two halves at least; bytes has to give each a byte */
    if (config->circular && bytes < count)
        return -EINVAL;
    if (src != NULL)
    {
        s->tx = Dmod_MallocTagged("dma", count * sizeof(gdma_desc_t));
        if (s->tx == NULL)
            return -ENOMEM;
    }
    if (dst != NULL)
    {
        s->rx = Dmod_MallocTagged("dma", count * sizeof(gdma_desc_t));
        if (s->rx == NULL)
        {
            free_chains(s);
            return -ENOMEM;
        }
    }
    if (!internal_ram(s->tx != NULL ? (void *)s->tx : (void *)s->rx, count * sizeof(gdma_desc_t)) ||
        (s->tx != NULL && s->rx != NULL && !internal_ram(s->rx, count * sizeof(gdma_desc_t))))
    {
        free_chains(s);
        return -ENOMEM;                                             /* the "dma" heap is not the internal RAM */
    }
    s->count = count;
    s->circular = config->circular;
    s->direction = config->direction;
    s->width = width;
    if (s->tx != NULL)
        build_chain(s->tx, count, src, bytes, false, config->circular);
    if (s->rx != NULL)
        build_chain(s->rx, count, dst, bytes, true, config->circular);

    uint32_t pri = (uint32_t)config->priority * 5U;                 /* low 0, medium 5, high 10, very high 15 */
    uint32_t peri = m2m ? m2m_peri(stream) : (uint32_t)config->request;
    s->peri = (int)peri;
    __asm__ __volatile__("memw" ::: "memory");

    s->running = true;
    if (s->rx != NULL)
    {
        *reg(GDMA_REG(stream, GDMA_IN_CONF0)) = m2m ? GDMA_IN_CONF0_MEM_TRANS_EN : 0U;
        *reg(GDMA_REG(stream, GDMA_IN_PERI_SEL)) = peri;
        *reg(GDMA_REG(stream, GDMA_IN_PRI)) = pri;
        *reg(GDMA_REG(stream, GDMA_IN_LINK)) = (uint32_t)(uintptr_t)s->rx & GDMA_LINK_ADDR_M;
        *reg(GDMA_REG(stream, GDMA_IN_INT_CLR)) = GDMA_IN_INT_FIFO_ALL;
        *reg(GDMA_REG(stream, GDMA_IN_INT_ENA)) = GDMA_IN_INT_DONE | GDMA_IN_INT_SUC_EOF | GDMA_IN_INT_ERR_EOF | GDMA_IN_INT_DSCR_ERR;
        *reg(GDMA_REG(stream, GDMA_IN_LINK)) |= GDMA_INLINK_START;
    }
    if (s->tx != NULL)
    {
        *reg(GDMA_REG(stream, GDMA_OUT_CONF0)) = GDMA_OUT_CONF0_AUTO_WRBACK;
        *reg(GDMA_REG(stream, GDMA_OUT_PERI_SEL)) = peri;
        *reg(GDMA_REG(stream, GDMA_OUT_PRI)) = pri;
        *reg(GDMA_REG(stream, GDMA_OUT_LINK)) = (uint32_t)(uintptr_t)s->tx & GDMA_LINK_ADDR_M;
        *reg(GDMA_REG(stream, GDMA_OUT_INT_CLR)) = GDMA_OUT_INT_FIFO_ALL;
        *reg(GDMA_REG(stream, GDMA_OUT_INT_ENA)) = GDMA_OUT_INT_DONE | GDMA_OUT_INT_DSCR_ERR | GDMA_OUT_INT_TOTAL_EOF;
        *reg(GDMA_REG(stream, GDMA_OUT_LINK)) |= GDMA_OUTLINK_START;
    }
    return 0;
}

dmod_dmdma_port_api_declaration(1.0, int, _stream_start,
                                (dmdma_controller_t controller, dmdma_stream_t stream, const dmdma_transfer_config_t *config))
{
    return dmdma_port_stream_start_ex(controller, stream, config, NULL);
}

dmod_dmdma_port_api_declaration(1.0, void, _stream_stop, (dmdma_controller_t controller, dmdma_stream_t stream))
{
    if (!valid(controller, stream) || !streams[stream].acquired)
        return;
    streams[stream].running = false;
    channel_halt(stream);                                           /* the descriptors are freed by the next start or the release */
}

dmod_dmdma_port_api_declaration(1.0, bool, _stream_is_busy, (dmdma_controller_t controller, dmdma_stream_t stream))
{
    return valid(controller, stream) && streams[stream].running;
}

dmod_dmdma_port_api_declaration(1.0, size_t, _stream_get_remaining, (dmdma_controller_t controller, dmdma_stream_t stream))
{
    if (!valid(controller, stream))
        return 0U;
    const stream_t *s = &streams[stream];
    const gdma_desc_t *chain = s->direction == dmdma_direction_memory_to_peripheral ? s->tx : s->rx;
    uint32_t bytes = 0;

    if (chain == NULL || s->width == 0U)
        return 0U;
    for (uint32_t i = 0; i < s->count; i++)
    {
        if (chain[i].dw0 & GDMA_DESC_OWNER)
            bytes += chain[i].dw0 & 0xFFFU;
    }
    return bytes / s->width;
}

dmod_dmdma_port_api_declaration(1.0, int, _add_interrupt_handler, (dmdma_port_interrupt_handler_t handler, void *user_ptr))
{
    if (handler == NULL)
        return -EINVAL;
    irq_handler = handler;
    irq_user_ptr = user_ptr;
    return 0;
}

dmod_dmdma_port_api_declaration(1.0, int, _remove_interrupt_handler, (void *user_ptr))
{
    if (irq_user_ptr == user_ptr)
    {
        irq_handler = NULL;
        irq_user_ptr = NULL;
    }
    return 0;
}

static void gdma_irq(void)
{
    for (unsigned ch = 0; ch < ESP32S3_GDMA_CHANNELS; ch++)
    {
        uint32_t in_st = *reg(GDMA_REG(ch, GDMA_IN_INT_ST));
        uint32_t out_st = *reg(GDMA_REG(ch, GDMA_OUT_INT_ST));
        if (in_st == 0U && out_st == 0U)
            continue;
        *reg(GDMA_REG(ch, GDMA_IN_INT_CLR)) = in_st;
        *reg(GDMA_REG(ch, GDMA_OUT_INT_CLR)) = out_st;

        stream_t *s = &streams[ch];
        if (!s->running)
            continue;

        uint32_t event = 0;
        if ((in_st & (GDMA_IN_INT_ERR_EOF | GDMA_IN_INT_DSCR_ERR)) || (out_st & GDMA_OUT_INT_DSCR_ERR))
        {
            event = dmdma_event_error;
            s->running = false;
        }
        else
        {
            bool receive = s->direction != dmdma_direction_memory_to_peripheral;
            gdma_desc_t *chain = receive ? s->rx : s->tx;
            uint32_t half = s->count / 2U;
            if (s->circular)
            {
                if (range_done(chain, 0, half))
                {
                    event |= dmdma_event_half_complete;
                    rearm(chain, 0, half, receive);
                }
                if (range_done(chain, half, s->count))
                {
                    event |= dmdma_event_complete;
                    rearm(chain, half, s->count, receive);
                }
            }
            else if (range_done(chain, 0, s->count))
            {
                event = dmdma_event_complete;
                s->running = false;
            }
        }
        if (event != 0U && irq_handler != NULL)
            irq_handler(irq_user_ptr, 0, (dmdma_stream_t)ch, (dmdma_event_t)event);
    }
}

DMOD_IRQ_HANDLER(17)
{
    gdma_irq();
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    Dmod_Printf("dmdma port module initialized (esp32s3)\n");
    return 0;
}

int dmod_deinit(void)
{
    for (unsigned i = 0; i < ESP32S3_GDMA_CHANNELS; i++)
        dmdma_port_stream_release(0, (dmdma_stream_t)i);
    Dmod_Printf("dmdma port module deinitialized (esp32s3)\n");
    return 0;
}
