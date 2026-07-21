#ifndef DMDMA_STM32_COMMON_H
#define DMDMA_STM32_COMMON_H

#include "dmdma_types.h"

/**
 * @brief Shared ISR core for a DMA stream interrupt
 *
 * Reads and clears the stream's status flags, builds the resulting
 * dmdma_event_t, and forwards it to whichever handler dmdma.c has
 * registered via dmdma_port_add_interrupt_handler(). Called from each
 * family's own DMOD_IRQ_HANDLER(...) (see stm32f4/port.c, stm32f7/port.c) -
 * the NVIC vector number itself is kept per-family even though it happens
 * to currently be identical on both, the same convention dmfmc's port uses.
 */
void stm32_dma_stream_irq(dmdma_controller_t controller, dmdma_stream_t stream);

#endif /* DMDMA_STM32_COMMON_H */
