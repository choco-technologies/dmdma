#include "stm32_common.h"
#include "dmdma_port.h"
#include "dmod.h"
#include "dmosi.h"
#include "port/stm32_common_regs.h"

#include <stdint.h>

#define STM32_RCC_AHB1ENR   (*(volatile uint32_t *)(STM32_RCC_BASE + 0x30U))

/* NVIC IRQ numbers, indexed [controller][stream] */
static const uint32_t dma_irqn[STM32_DMA_CONTROLLER_COUNT][STM32_DMA_STREAM_COUNT] = {
    {
        STM32_DMA1_STREAM0_IRQn, STM32_DMA1_STREAM1_IRQn, STM32_DMA1_STREAM2_IRQn, STM32_DMA1_STREAM3_IRQn,
        STM32_DMA1_STREAM4_IRQn, STM32_DMA1_STREAM5_IRQn, STM32_DMA1_STREAM6_IRQn, STM32_DMA1_STREAM7_IRQn,
    },
    {
        STM32_DMA2_STREAM0_IRQn, STM32_DMA2_STREAM1_IRQn, STM32_DMA2_STREAM2_IRQn, STM32_DMA2_STREAM3_IRQn,
        STM32_DMA2_STREAM4_IRQn, STM32_DMA2_STREAM5_IRQn, STM32_DMA2_STREAM6_IRQn, STM32_DMA2_STREAM7_IRQn,
    },
};

/* Single global dispatcher - dmdma.c registers exactly one, covering every
 * controller/stream this port manages (same shape as dmfmc's port). */
static dmdma_port_interrupt_handler_t g_irq_handler  = NULL;
static void                          *g_irq_user_ptr = NULL;

static int validate(dmdma_controller_t controller, dmdma_stream_t stream)
{
    return (controller < STM32_DMA_CONTROLLER_COUNT && stream < STM32_DMA_STREAM_COUNT) ? 0 : -1;
}

static volatile DMA_TypeDef *get_dma(dmdma_controller_t controller)
{
    uint32_t base = (controller == 0U) ? STM32_DMA1_BASE : STM32_DMA2_BASE;
    return (volatile DMA_TypeDef *)base;
}

static volatile DMA_Stream_TypeDef *get_stream(dmdma_controller_t controller, dmdma_stream_t stream)
{
    uint32_t base = (controller == 0U) ? STM32_DMA1_BASE : STM32_DMA2_BASE;
    return (volatile DMA_Stream_TypeDef *)(base + DMA_STREAM_OFFSET + ((uint32_t)stream * DMA_STREAM_SIZE));
}

static void nvic_enable_irq(uint32_t irqn)
{
    /* Same reasoning as every other dmod STM32 port: keep the priority at or
     * below dmosi's minimum maskable priority, since the ISR calls back into
     * core code that may touch FreeRTOS ISR-safe API. */
    NVIC_IP[irqn]          = (uint8_t)dmosi_get_min_interrupt_priority();
    NVIC_ISER[irqn >> 5U]  = 1U << (irqn & 0x1FU);
}

static void nvic_disable_irq(uint32_t irqn)
{
    NVIC_ICER[irqn >> 5U] = 1U << (irqn & 0x1FU);
}

/* Disabling a stream is not instantaneous - the hardware finishes whatever
 * AHB beat is in flight before EN actually reads back as 0 (RM0090/RM0385,
 * DMA stream configuration procedure). Every register other than EN in
 * SxCR, plus NDTR/PAR/MxAR/FCR, must not be touched while EN is still set. */
static void stream_disable_and_wait(volatile DMA_Stream_TypeDef *s)
{
    if (s->CR & DMA_SxCR_EN)
    {
        s->CR &= ~DMA_SxCR_EN;
        while (s->CR & DMA_SxCR_EN) { }
    }
}

static uint32_t stream_flag_mask(uint8_t shift)
{
    return DMA_FLAG_FEIF(shift) | DMA_FLAG_DMEIF(shift) | DMA_FLAG_TEIF(shift) |
           DMA_FLAG_HTIF(shift) | DMA_FLAG_TCIF(shift);
}

static void stream_clear_flags(dmdma_controller_t controller, dmdma_stream_t stream)
{
    volatile DMA_TypeDef *DMA = get_dma(controller);
    uint8_t shift = dma_stream_flag_shift[stream % 4U];
    uint32_t mask = stream_flag_mask(shift);

    if (stream < 4U) DMA->LIFCR = mask;
    else              DMA->HIFCR = mask;
}

static uint32_t width_to_size_field(dmdma_data_width_t width)
{
    switch (width)
    {
        case dmdma_data_width_halfword: return 1U;
        case dmdma_data_width_word:     return 2U;
        case dmdma_data_width_byte:
        default:                        return 0U;
    }
}

/* ---- Capability queries ---- */

dmod_dmdma_port_api_declaration(1.0, uint8_t, _get_stream_count, ( dmdma_controller_t controller ))
{
    return (controller < STM32_DMA_CONTROLLER_COUNT) ? (uint8_t)STM32_DMA_STREAM_COUNT : 0U;
}

dmod_dmdma_port_api_declaration(1.0, bool, _supports_memory_to_memory, ( dmdma_controller_t controller ))
{
    /* RM0090/RM0385 hardware limitation: only DMA2 (controller 1) can
     * perform memory-to-memory transfers. */
    return controller == 1U;
}

/* ---- Stream reservation ---- */

dmod_dmdma_port_api_declaration(1.0, int, _stream_acquire, ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (validate(controller, stream) != 0) return -1;

    STM32_RCC_AHB1ENR |= (controller == 0U) ? RCC_AHB1ENR_DMA1EN : RCC_AHB1ENR_DMA2EN;

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    stream_disable_and_wait(s);
    stream_clear_flags(controller, stream);

    s->NDTR = 0U;
    s->PAR  = 0U;
    s->M0AR = 0U;
    s->M1AR = 0U;
    s->FCR  = DMA_SxFCR_RESET_VALUE;

    return 0;
}

dmod_dmdma_port_api_declaration(1.0, void, _stream_release, ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (validate(controller, stream) != 0) return;

    nvic_disable_irq(dma_irqn[controller][stream]);

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    stream_disable_and_wait(s);
    stream_clear_flags(controller, stream);
}

/* ---- Transfer control ---- */

dmod_dmdma_port_api_declaration(1.0, int, _stream_start,
    ( dmdma_controller_t controller, dmdma_stream_t stream, const dmdma_transfer_config_t *config ))
{
    if (validate(controller, stream) != 0 || config == NULL) return -1;
    if (config->element_count == 0U || config->element_count > 0xFFFFU) return -1;
    if (config->destination_address == NULL) return -1;
    if (config->direction != dmdma_direction_memory_to_memory && config->source_address == NULL) return -1;

    /* Which side of source/destination is "the peripheral" for register
     * purposes depends on direction: memory_to_peripheral swaps PAR/M0AR
     * relative to source/destination, peripheral_to_memory and
     * memory_to_memory both use PAR=source, M0AR=destination as-is. */
    bool peripheral_is_destination = (config->direction == dmdma_direction_memory_to_peripheral);

    uint32_t peripheral_addr = (uint32_t)(uintptr_t)(peripheral_is_destination
        ? config->destination_address : config->source_address);
    uint32_t memory_addr = (uint32_t)(uintptr_t)(peripheral_is_destination
        ? config->source_address : config->destination_address);
    dmdma_data_width_t peripheral_width = peripheral_is_destination
        ? config->destination_width : config->source_width;
    dmdma_data_width_t memory_width = peripheral_is_destination
        ? config->source_width : config->destination_width;
    bool peripheral_inc = peripheral_is_destination
        ? config->destination_increment : config->source_increment;
    bool memory_inc = peripheral_is_destination
        ? config->source_increment : config->destination_increment;

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    stream_disable_and_wait(s);
    stream_clear_flags(controller, stream);

    uint32_t cr = 0U;
    switch (config->direction)
    {
        case dmdma_direction_peripheral_to_memory: cr |= DMA_SxCR_DIR_PERIPH_TO_MEM; break;
        case dmdma_direction_memory_to_peripheral: cr |= DMA_SxCR_DIR_MEM_TO_PERIPH; break;
        case dmdma_direction_memory_to_memory:
        default:                                   cr |= DMA_SxCR_DIR_MEM_TO_MEM;    break;
    }

    cr |= width_to_size_field(peripheral_width) << DMA_SxCR_PSIZE_Pos;
    cr |= width_to_size_field(memory_width)     << DMA_SxCR_MSIZE_Pos;
    if (peripheral_inc)    cr |= DMA_SxCR_PINC;
    if (memory_inc)        cr |= DMA_SxCR_MINC;
    if (config->circular)  cr |= DMA_SxCR_CIRC | DMA_SxCR_HTIE;
    cr |= ((uint32_t)config->priority & 0x3U) << DMA_SxCR_PL_Pos;
    cr |= DMA_SxCR_TCIE | DMA_SxCR_TEIE;

    /* Memory-to-memory ignores CHSEL and requires the DMA itself to be the
     * flow controller - PFCTRL=0 (already the case, we never set it). */
    if (config->direction != dmdma_direction_memory_to_memory)
    {
        cr |= ((uint32_t)config->request & 0x7U) << DMA_SxCR_CHSEL_Pos;
    }

    s->PAR  = peripheral_addr;
    s->M0AR = memory_addr;
    s->NDTR = (uint32_t)config->element_count;

    nvic_enable_irq(dma_irqn[controller][stream]);
    s->CR = cr | DMA_SxCR_EN;

    return 0;
}

dmod_dmdma_port_api_declaration(1.0, void, _stream_stop, ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (validate(controller, stream) != 0) return;

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    stream_disable_and_wait(s);
    stream_clear_flags(controller, stream);
}

dmod_dmdma_port_api_declaration(1.0, bool, _stream_is_busy, ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (validate(controller, stream) != 0) return false;

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    return (s->CR & DMA_SxCR_EN) != 0U;
}

dmod_dmdma_port_api_declaration(1.0, size_t, _stream_get_remaining, ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (validate(controller, stream) != 0) return 0U;

    volatile DMA_Stream_TypeDef *s = get_stream(controller, stream);
    return (size_t)s->NDTR;
}

/* ---- Interrupt handler registration ---- */

dmod_dmdma_port_api_declaration(1.0, int, _add_interrupt_handler,
    ( dmdma_port_interrupt_handler_t handler, void *user_ptr ))
{
    if (handler == NULL) return -1;

    g_irq_handler  = handler;
    g_irq_user_ptr = user_ptr;
    return 0;
}

dmod_dmdma_port_api_declaration(1.0, int, _remove_interrupt_handler, ( void *user_ptr ))
{
    if (g_irq_user_ptr == user_ptr)
    {
        g_irq_handler  = NULL;
        g_irq_user_ptr = NULL;
    }
    return 0;
}

/* ---- ISR core (called from each family's DMOD_IRQ_HANDLER) ---- */

void stm32_dma_stream_irq(dmdma_controller_t controller, dmdma_stream_t stream)
{
    volatile DMA_TypeDef *DMA = get_dma(controller);
    uint8_t shift = dma_stream_flag_shift[stream % 4U];
    uint32_t sr = (stream < 4U) ? DMA->LISR : DMA->HISR;

    uint32_t event_flags = 0U;
    if (sr & DMA_FLAG_TCIF(shift)) event_flags |= (uint32_t)dmdma_event_complete;
    if (sr & DMA_FLAG_HTIF(shift)) event_flags |= (uint32_t)dmdma_event_half_complete;
    if (sr & (DMA_FLAG_TEIF(shift) | DMA_FLAG_DMEIF(shift) | DMA_FLAG_FEIF(shift)))
        event_flags |= (uint32_t)dmdma_event_error;

    stream_clear_flags(controller, stream);

    if (g_irq_handler != NULL && event_flags != 0U)
        g_irq_handler(g_irq_user_ptr, controller, stream, (dmdma_event_t)event_flags);
}
