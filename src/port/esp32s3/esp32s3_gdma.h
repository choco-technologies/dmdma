#ifndef ESP32S3_GDMA_H
#define ESP32S3_GDMA_H

#include <stdint.h>

/*
 * ESP32-S3 GDMA (general DMA): one controller, 5 channel pairs. Channel n has an IN (receive: peripheral -> memory) and an OUT (transmit:
 * memory -> peripheral) side with registers of their own. A transfer is a linked list of descriptors in the internal RAM; the
 * peripheral is chosen by PERI_SEL, there is no peripheral data register address. Registers: ESP32-S3 Technical Reference Manual, GDMA.
 */
#define ESP32S3_GDMA_CHANNELS           5U
#define ESP32S3_GDMA_BASE               0x6003F000UL
#define ESP32S3_GDMA_CH_STRIDE          0xC0UL
#define GDMA_REG(ch, off)               (ESP32S3_GDMA_BASE + (ch) * ESP32S3_GDMA_CH_STRIDE + (off))

/* IN side */
#define GDMA_IN_CONF0                   0x00UL
#define GDMA_IN_CONF1                   0x04UL
#define GDMA_IN_INT_RAW                 0x08UL
#define GDMA_IN_INT_ST                  0x0CUL
#define GDMA_IN_INT_ENA                 0x10UL
#define GDMA_IN_INT_CLR                 0x14UL
#define GDMA_IN_LINK                    0x20UL
#define GDMA_IN_PRI                     0x44UL
#define GDMA_IN_PERI_SEL                0x48UL
/* OUT side */
#define GDMA_OUT_CONF0                  0x60UL
#define GDMA_OUT_CONF1                  0x64UL
#define GDMA_OUT_INT_RAW                0x68UL
#define GDMA_OUT_INT_ST                 0x6CUL
#define GDMA_OUT_INT_ENA                0x70UL
#define GDMA_OUT_INT_CLR                0x74UL
#define GDMA_OUT_LINK                   0x80UL
#define GDMA_OUT_PRI                    0xA4UL
#define GDMA_OUT_PERI_SEL               0xA8UL
/* Shared */
#define GDMA_MISC_CONF                  (ESP32S3_GDMA_BASE + 0x3C8UL)
#define GDMA_MISC_CLK_EN                (1UL << 3)

/* IN_CONF0 / OUT_CONF0 */
#define GDMA_CONF0_RST                  (1UL << 0)
#define GDMA_IN_CONF0_MEM_TRANS_EN      (1UL << 4)      /* the IN side receives what the OUT side of the same channel sends */
#define GDMA_OUT_CONF0_AUTO_WRBACK      (1UL << 2)      /* the owner bit of a sent descriptor is cleared */
/* IN_LINK / OUT_LINK: the address (the low 20 bits; the RAM is at 0x3FCxxxxx), the controls */
#define GDMA_LINK_ADDR_M                0x000FFFFFUL
#define GDMA_INLINK_STOP                (1UL << 21)
#define GDMA_INLINK_START               (1UL << 22)
#define GDMA_OUTLINK_STOP               (1UL << 20)
#define GDMA_OUTLINK_START              (1UL << 21)
/* Interrupts */
#define GDMA_IN_INT_DONE                (1UL << 0)
#define GDMA_IN_INT_SUC_EOF             (1UL << 1)
#define GDMA_IN_INT_ERR_EOF             (1UL << 2)
#define GDMA_IN_INT_DSCR_ERR            (1UL << 3)
#define GDMA_IN_INT_DSCR_EMPTY          (1UL << 4)
#define GDMA_IN_INT_FIFO_ALL            0x3FFUL
#define GDMA_OUT_INT_DONE               (1UL << 0)
#define GDMA_OUT_INT_EOF                (1UL << 1)
#define GDMA_OUT_INT_DSCR_ERR           (1UL << 2)
#define GDMA_OUT_INT_TOTAL_EOF          (1UL << 3)
#define GDMA_OUT_INT_FIFO_ALL           0x1FFUL

/* Descriptor (hal/dma_types.h): word 0 = size [11:0], length [23:12], err_eof [28], suc_eof [30], owner [31] (1: the DMA) */
typedef struct
{
    volatile uint32_t   dw0;
    volatile uint32_t   buffer;
    volatile uint32_t   next;
} gdma_desc_t;
#define GDMA_DESC_SIZE_MAX              4092U           /* a multiple of 4 below the 12 bits */
#define GDMA_DESC_OWNER                 (1UL << 31)
#define GDMA_DESC_SUC_EOF               (1UL << 30)
#define GDMA_DESC_ERR_EOF               (1UL << 28)

/* Peripherals (PERI_SEL) a request selects: SPI2 0, SPI3 1, UHCI0 2, I2S0 3, I2S1 4, LCD_CAM 5, AES 6, SHA 7, ADC 8, RMT 9 */
#define GDMA_PERI_MAX                   9U

/* System: the clock and the reset of the GDMA, PERIP_CLK_EN1 / PERIP_RST_EN1 bit 6 */
#define ESP32S3_SYSTEM_BASE             0x600C0000UL
#define ESP32S3_PERIP_CLK_EN1_REG       (ESP32S3_SYSTEM_BASE + 0x1CUL)
#define ESP32S3_PERIP_RST_EN1_REG       (ESP32S3_SYSTEM_BASE + 0x24UL)
#define ESP32S3_GDMA_PERIP_BIT          (1UL << 6)

/*
 * The interrupt sources of the channels (IN 64..68, OUT 69..73; level) all go to one level 1 line of the CPU, 17 (the others in use: the
 * GPIO 2, the UARTs 3, 9 and 12, the USB Serial/JTAG 13, the I2C 4 and 5, the software one of the flash 7).
 */
#define ESP32S3_GDMA_IN_SOURCE(ch)      (64UL + (ch))
#define ESP32S3_GDMA_OUT_SOURCE(ch)     (69UL + (ch))
#define ESP32S3_GDMA_CPU_INTR           17U
#define ESP32S3_INTMTX_MAP_REG(source)  (0x600C2000UL + 4UL * (source))

/* The internal RAM (the data bus): what the GDMA reaches without the cache and what its descriptors can address */
#define ESP32S3_INTERNAL_RAM_START      0x3FC88000UL
#define ESP32S3_INTERNAL_RAM_END        0x3FCF0000UL

#endif /* ESP32S3_GDMA_H */
