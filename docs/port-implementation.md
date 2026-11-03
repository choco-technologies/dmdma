# Adding a New MCU Port to dmdma

`dmdma_port` currently supports `stm32f4` and `stm32f7`. Both share the
exact same 8-stream DMA controller IP block (confirmed by dnx-rtos shipping
one combined "STM32F4F7" driver for both), so essentially all logic lives in
`src/port/stm32_common/stm32_common.c`, and each family's
`src/port/<family>/port.c` is only a few lines: `dmod_init`/`dmod_deinit`
logging, and 16 `DMOD_IRQ_HANDLER(...)` forwarding to
`stm32_dma_stream_irq()` (one per DMA1/DMA2 stream).

## Steps to add a family whose DMA IP differs (e.g. a hypothetical future family)

1. Create `src/port/<family>/config.cmake`, setting `DMOD_TOOLS_NAME` for the
   target architecture (see `src/port/stm32f7/config.cmake` for the pattern -
   it must match a directory under `dmod/configs/arch/...`).
2. Create `include/port/<family>_regs.h` for anything that genuinely differs
   from `stm32_common_regs.h` (base addresses, IRQ numbers, register bit
   layout if the controller isn't a drop-in match).
3. Decide how much logic can stay shared:
   - If the DMA_SxCR/NDTR/PAR/MxAR/FCR bit layout is identical (true for
     every STM32F4/F7 part so far), add the family to
     `src/port/CMakeLists.txt`'s `COMMON_SOURCES` selection so it also
     compiles `stm32_common/stm32_common.c`, and write only
     `src/port/<family>/port.c` (lifecycle + IRQ forwarding), following the
     existing `stm32f4`/`stm32f7` files.
   - If the register layout genuinely differs, implement the
     `dmod_dmdma_port_api_declaration(...)` functions from `dmdma_port.h`
     directly in `src/port/<family>/port.c` instead of relying on
     `stm32_common.c`.
4. Build by selecting the new family: `cmake .. -DDMOD_CPU_FAMILY=<family>`.
5. Add a `configs/mcu/<mcu>.ini` for at least one MCU on the new family - see
   `configs/mcu/stm32f746zg.ini`.
6. Do not introduce a module-specific variable (e.g. `<MODULE>_MCU_SERIES`) for
   this - `DMOD_CPU_FAMILY` is the ecosystem-wide convention, already wired
   into `dmf-get` package resolution.

## Notes for whoever validates the STM32F4 port

The STM32F7 port's register bit positions and IRQ vector numbers were
transcribed from RM0385 (and cross-checked against `dmuart`'s own STM32F7
port, which uses the same vector table) but have **not** been run against
real hardware yet. The STM32F4 port shares 100% of its logic with F7 through
`stm32_common.c` on the assumption that the DMA controller and IRQ vector
placement are register-identical between the two families, which matches
ST's reference manuals (RM0090 vs RM0385) but is worth confirming against
the exact silicon revision before shipping. In particular, double check:
- `STM32_DMA1_BASE`/`STM32_DMA2_BASE` and the 16 `STM32_DMA*_STREAM*_IRQn`
  values (assumed identical on both families, in `stm32_common_regs.h`).
- Only DMA2 (controller 1) can perform memory-to-memory transfers - this is
  asserted in `stm32_common.c`'s `_supports_memory_to_memory()`.

## ESP32-S3: GDMA

`src/port/esp32s3/port.c` is the whole port (its own controller, nothing is shared with `stm32_common`). One controller (0) with 5
streams (`/dev/dmdma0/0` .. `4`), memory-to-memory capable. Stream n is the channel pair n of the GDMA: memory -> peripheral
uses its OUT side, peripheral -> memory the IN side, memory -> memory both.

What differs from the STM32 DMA, and so from the transfer config:

* The GDMA works on **linked lists of descriptors** (12 bytes each, 4092 bytes of data at most) that the port builds from the config; they
  come from the heap tagged `"dma"`, which has to be in the internal RAM (it is on dmod-boot: the linker region `dma`).
* There is **no peripheral register address**: the peripheral is chosen by `request` = its PERI_SEL (SPI2 0, SPI3 1, UHCI0 2, I2S0 3,
  I2S1 4, LCD_CAM 5, AES 6, SHA 7, ADC 8, RMT 9) and the address on the peripheral side of the config is ignored.
* The memory buffers must be in the **internal RAM** (0x3FC88000..0x3FCF0000): the external RAM is behind the cache, which the GDMA does
  not see (`-EINVAL` otherwise). On dmod-boot the heap puts allocations in the external RAM first, so take buffers from the `"dma"`
  heap (`Dmod_MallocTagged("dma", size)`); a module's static data is not in the internal RAM either.
* Source and destination widths are equal (the GDMA moves bytes), the memory side increments; flow control by the peripheral, FIFO and
  bursts are not supported (`-ENOTSUP`). A transfer is at most 256 KB.
* A memory-to-memory transfer needs the PERI_SEL of a peripheral that can do it; the port takes the highest one (9 down to 1) that no
  other running stream is bound to.
* The end of a transfer is read from the owner bits of the descriptors (the GDMA clears them). `get_remaining()` therefore has the
  granularity of a descriptor. A circular transfer (peripheral directions only) is a ring of an even number of descriptors: the first
  half finished is `dmdma_event_half_complete`, the second one `dmdma_event_complete`.
* All ten interrupt sources go to one level 1 line of the CPU (17).

`tests/dmdma_test_port` takes the controller and a size limit as optional arguments (`dmdma_test_port <file> 0 12000` on the ESP32-S3);
`dmdma_test_lease` uses static buffers and so does not apply to it as it is.
