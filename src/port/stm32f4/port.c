#define DMOD_ENABLE_REGISTRATION    ON
#include "dmdma_port.h"
#include "dmod.h"
#include "../stm32_common/stm32_common.h"
#include "port/stm32_common_regs.h"
#include "port/stm32f4_regs.h"

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("dmdma port module initialized (stm32f4)\n");
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmdma port module deinitialized (stm32f4)\n");
    return 0;
}

/* ---- ISR handlers ----
 *
 * All register-level logic lives in stm32_common.c, shared with STM32F7
 * (identical DMA controller IP block); only the NVIC IRQ numbers are
 * declared per family, forwarding to stm32_dma_stream_irq(). */

DMOD_IRQ_HANDLER(STM32_DMA1_STREAM0_IRQn) { stm32_dma_stream_irq(0, 0); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM1_IRQn) { stm32_dma_stream_irq(0, 1); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM2_IRQn) { stm32_dma_stream_irq(0, 2); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM3_IRQn) { stm32_dma_stream_irq(0, 3); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM4_IRQn) { stm32_dma_stream_irq(0, 4); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM5_IRQn) { stm32_dma_stream_irq(0, 5); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM6_IRQn) { stm32_dma_stream_irq(0, 6); }
DMOD_IRQ_HANDLER(STM32_DMA1_STREAM7_IRQn) { stm32_dma_stream_irq(0, 7); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM0_IRQn) { stm32_dma_stream_irq(1, 0); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM1_IRQn) { stm32_dma_stream_irq(1, 1); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM2_IRQn) { stm32_dma_stream_irq(1, 2); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM3_IRQn) { stm32_dma_stream_irq(1, 3); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM4_IRQn) { stm32_dma_stream_irq(1, 4); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM5_IRQn) { stm32_dma_stream_irq(1, 5); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM6_IRQn) { stm32_dma_stream_irq(1, 6); }
DMOD_IRQ_HANDLER(STM32_DMA2_STREAM7_IRQn) { stm32_dma_stream_irq(1, 7); }
