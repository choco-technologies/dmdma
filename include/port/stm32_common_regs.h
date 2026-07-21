#ifndef DMDMA_STM32_COMMON_REGS_H
#define DMDMA_STM32_COMMON_REGS_H

#include <stdint.h>

/* ======================================================================
 *      Common STM32F4/F7 DMA Controller Register Definitions
 *
 * Both families use the identical 8-stream DMA IP (RM0090 / RM0385,
 * "DMA controller" chapter) - confirmed by dnx-rtos shipping one combined
 * "STM32F4F7" driver for both. Base addresses and IRQ vector numbers are
 * assumed identical too (per family "regs.h" is reserved for whatever, if
 * anything, turns out to actually differ once validated on real hardware -
 * see stm32f4_regs.h / stm32f7_regs.h).
 * ====================================================================== */

/**
 * @brief DMA controller-level registers (interrupt status/clear, shared by all 8 streams)
 */
typedef struct
{
    volatile uint32_t LISR;    /**< Low interrupt status register (streams 0-3) */
    volatile uint32_t HISR;    /**< High interrupt status register (streams 4-7) */
    volatile uint32_t LIFCR;   /**< Low interrupt flag clear register (streams 0-3) */
    volatile uint32_t HIFCR;   /**< High interrupt flag clear register (streams 4-7) */
} DMA_TypeDef;

/**
 * @brief DMA stream registers. One controller has 8 of these, starting at
 *        controller_base + 0x10, 0x18 bytes apart.
 */
typedef struct
{
    volatile uint32_t CR;      /**< Configuration register */
    volatile uint32_t NDTR;    /**< Number of data register */
    volatile uint32_t PAR;     /**< Peripheral address register */
    volatile uint32_t M0AR;    /**< Memory 0 address register */
    volatile uint32_t M1AR;    /**< Memory 1 address register (double-buffer mode, unused here) */
    volatile uint32_t FCR;     /**< FIFO control register */
} DMA_Stream_TypeDef;

#define DMA_STREAM_OFFSET           0x10U
#define DMA_STREAM_SIZE             0x18U

/* DMA_SxCR bits */
#define DMA_SxCR_EN                 (1U << 0)
#define DMA_SxCR_DMEIE               (1U << 1)
#define DMA_SxCR_TEIE                (1U << 2)
#define DMA_SxCR_HTIE                (1U << 3)
#define DMA_SxCR_TCIE                (1U << 4)
#define DMA_SxCR_PFCTRL              (1U << 5)
#define DMA_SxCR_DIR_Pos             6U
#define DMA_SxCR_DIR_Msk             (0x3U << DMA_SxCR_DIR_Pos)
#define DMA_SxCR_DIR_PERIPH_TO_MEM   (0x0U << DMA_SxCR_DIR_Pos)
#define DMA_SxCR_DIR_MEM_TO_PERIPH   (0x1U << DMA_SxCR_DIR_Pos)
#define DMA_SxCR_DIR_MEM_TO_MEM      (0x2U << DMA_SxCR_DIR_Pos)
#define DMA_SxCR_CIRC                (1U << 8)
#define DMA_SxCR_PINC                (1U << 9)
#define DMA_SxCR_MINC                (1U << 10)
#define DMA_SxCR_PSIZE_Pos           11U
#define DMA_SxCR_PSIZE_Msk           (0x3U << DMA_SxCR_PSIZE_Pos)
#define DMA_SxCR_MSIZE_Pos           13U
#define DMA_SxCR_MSIZE_Msk           (0x3U << DMA_SxCR_MSIZE_Pos)
#define DMA_SxCR_PL_Pos              16U
#define DMA_SxCR_PL_Msk              (0x3U << DMA_SxCR_PL_Pos)
#define DMA_SxCR_DBM                 (1U << 18)
#define DMA_SxCR_CT                  (1U << 19)
#define DMA_SxCR_PBURST_Pos          21U
#define DMA_SxCR_PBURST_Msk          (0x3U << DMA_SxCR_PBURST_Pos)
#define DMA_SxCR_MBURST_Pos          23U
#define DMA_SxCR_MBURST_Msk          (0x3U << DMA_SxCR_MBURST_Pos)
#define DMA_SxCR_CHSEL_Pos           25U
#define DMA_SxCR_CHSEL_Msk           (0x7U << DMA_SxCR_CHSEL_Pos)

/* DMA_SxFCR bits */
#define DMA_SxFCR_FTH_Pos            0U
#define DMA_SxFCR_FTH_Msk            (0x3U << DMA_SxFCR_FTH_Pos)
#define DMA_SxFCR_DMDIS              (1U << 2)
#define DMA_SxFCR_FS_Pos             3U
#define DMA_SxFCR_FS_Msk             (0x7U << DMA_SxFCR_FS_Pos)
#define DMA_SxFCR_FEIE                (1U << 7)
#define DMA_SxFCR_RESET_VALUE        0x00000021U  /**< Direct mode enabled, FTH=01 */

/*
 * Per-stream interrupt flag layout within LISR/HISR/LIFCR/HIFCR (RM0090
 * "DMA interrupt status register" / "DMA interrupt flag clear register"):
 * each register packs 4 streams as 6-bit groups at bit offsets
 * {0, 6, 16, 22} (bits 5 and 11-15 unused/reserved padding), and within each
 * group: bit+0=FEIF, bit+1=reserved, bit+2=DMEIF, bit+3=TEIF, bit+4=HTIF,
 * bit+5=TCIF. LISR/LIFCR cover streams 0-3, HISR/HIFCR cover streams 4-7
 * with the exact same intra-register layout.
 */
static const uint8_t dma_stream_flag_shift[4] = { 0U, 6U, 16U, 22U };

#define DMA_FLAG_FEIF(shift)   (1U << ((shift) + 0U))
#define DMA_FLAG_DMEIF(shift)  (1U << ((shift) + 2U))
#define DMA_FLAG_TEIF(shift)   (1U << ((shift) + 3U))
#define DMA_FLAG_HTIF(shift)   (1U << ((shift) + 4U))
#define DMA_FLAG_TCIF(shift)   (1U << ((shift) + 5U))

/* NVIC Interrupt Set/Clear-Enable registers (ARMv7-M, common to Cortex-M4/M7) */
#define NVIC_ISER                ((volatile uint32_t *)0xE000E100UL)
#define NVIC_ICER                ((volatile uint32_t *)0xE000E180UL)

/* NVIC Interrupt Priority Registers (ARMv7-M, common to Cortex-M4/M7).
 * Byte-addressable, one 8-bit entry per external interrupt line. */
#define NVIC_IP                   ((volatile uint8_t *)0xE000E400UL)

/* ======================================================================
 *      Base Addresses, Clock Bits and IRQ Numbers
 *
 * Assumed identical between STM32F4 and STM32F7 (same DMA1/DMA2 controller
 * placement on the AHB1 bus, same vector table slots for their 16 stream
 * IRQs on every part either family's port has been checked against so far).
 * ====================================================================== */

#define STM32_RCC_BASE            0x40023800U
#define STM32_DMA1_BASE           0x40026000U
#define STM32_DMA2_BASE           0x40026400U

/* Number of physical DMA controllers and streams-per-controller */
#define STM32_DMA_CONTROLLER_COUNT   2U
#define STM32_DMA_STREAM_COUNT       8U

/* RCC AHB1ENR DMA clock enable bits */
#define RCC_AHB1ENR_DMA1EN           (1U << 21)
#define RCC_AHB1ENR_DMA2EN           (1U << 22)

/*
 * NVIC IRQ numbers for DMA streams (RM0090 / RM0385 vector table). Only
 * DMA2 (controller 1) can do memory-to-memory transfers - a hardware
 * limitation of this DMA IP, not a software choice.
 */
#define STM32_DMA1_STREAM0_IRQn    11
#define STM32_DMA1_STREAM1_IRQn    12
#define STM32_DMA1_STREAM2_IRQn    13
#define STM32_DMA1_STREAM3_IRQn    14
#define STM32_DMA1_STREAM4_IRQn    15
#define STM32_DMA1_STREAM5_IRQn    16
#define STM32_DMA1_STREAM6_IRQn    17
#define STM32_DMA1_STREAM7_IRQn    47
#define STM32_DMA2_STREAM0_IRQn    56
#define STM32_DMA2_STREAM1_IRQn    57
#define STM32_DMA2_STREAM2_IRQn    58
#define STM32_DMA2_STREAM3_IRQn    59
#define STM32_DMA2_STREAM4_IRQn    60
#define STM32_DMA2_STREAM5_IRQn    68
#define STM32_DMA2_STREAM6_IRQn    69
#define STM32_DMA2_STREAM7_IRQn    70

#endif /* DMDMA_STM32_COMMON_REGS_H */
