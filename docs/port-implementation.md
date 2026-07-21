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
