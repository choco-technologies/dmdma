# DMDMA Configuration Files

This directory contains pre-configured DMA controller settings, read by
`dmdevfs` at boot to create a dmdma driver context for each physical DMA
controller. Without a config section declaring `driver_name=dmdma`
somewhere under the board's `/configs` tree, dmdevfs never calls
`dmdma_dmdrvi_create()` and no `/dev/dmdmaN` device ever appears.

## Directory Structure

```
configs/
├── board/                           # Board-specific configurations
│   ├── nucleo-f401re/
│   │   └── dma.ini
│   ├── nucleo-f411re/
│   │   └── dma.ini
│   ├── nucleo-f446re/
│   │   └── dma.ini
│   ├── nucleo-f767zi/
│   │   └── dma.ini
│   ├── stm32f4-discovery/
│   │   └── dma.ini
│   ├── stm32f429i-discovery/
│   │   └── dma.ini
│   ├── stm32f746g-disco/
│   │   └── dma.ini
│   └── stm32f769i-discovery/
│       └── dma.ini
└── mcu/                             # MCU-specific configurations
    ├── stm32f401re.ini
    ├── stm32f405rg.ini
    ├── stm32f407vg.ini
    ├── stm32f411re.ini
    ├── stm32f429zi.ini
    ├── stm32f439zi.ini
    ├── stm32f446re.ini
    ├── stm32f469ni.ini
    ├── stm32f722re.ini
    ├── stm32f746zg.ini
    ├── stm32f767zi.ini
    └── stm32f769ni.ini
```

Unlike `dmuart`/`dmgpio`, DMA controller availability, stream count, and
mem-to-mem capability are facts about the MCU alone - no board-specific pin
wiring is involved (DMA has no GPIO to configure), so a board's `dma.ini` is
never more than the matching `mcu/*.ini` for that board's chip, just filed
under `board/<name>/` too. It's still shipped per board (not only per MCU)
so that a board's aggregate `/configs` tree - assembled from every driver
module's own `board/<name>/` folder, the same way `dmuart`/`dmgpio`'s pin
and instance configs are - picks up DMA automatically instead of requiring
each board integrator to remember to add it separately.

## Configuration Format

```ini
[dma1]
driver_name=dmdma
driver_order=2
controller=0

[dma2]
driver_name=dmdma
driver_order=2
controller=1
```

### Parameters (per driver section)

- **driver_order**: `2` - configured after GPIO (order `1` in configs that
  combine both, e.g. `dmuart`'s or `dmfmc`'s board configs) and before any
  peripheral driver that wants to use DMA once it's up.

- **controller**: Physical DMA controller this context manages (0-based;
  0 = DMA1, 1 = DMA2 on STM32F4/F7). Becomes the context's dmdrvi major
  number, so `controller=0` is exposed as `/dev/dmdma0`.

Streams are not listed in the config - dmdma queries the port for how many
streams a controller has and exposes each one as its own device
(`/dev/dmdma0/0` .. `/dev/dmdma0/7`). A consumer (typically another driver's
port, e.g. a future DMA-backed dmuart RX/TX path) reserves one by opening
that specific path directly, the same way `dmtty` attaches to an arbitrary
backing file - see the main [README.md](../README.md) for the full model.

## Board Configurations

| Board | Folder | MCU | DMA Controllers |
|-------|--------|-----|------------------|
| NUCLEO-F401RE | `board/nucleo-f401re/` | STM32F401RE | DMA1 (0), DMA2 (1, mem-to-mem) |
| NUCLEO-F411RE | `board/nucleo-f411re/` | STM32F411RE | DMA1 (0), DMA2 (1, mem-to-mem) |
| NUCLEO-F446RE | `board/nucleo-f446re/` | STM32F446RE | DMA1 (0), DMA2 (1, mem-to-mem) |
| NUCLEO-F767ZI | `board/nucleo-f767zi/` | STM32F767ZI | DMA1 (0), DMA2 (1, mem-to-mem) |
| STM32F4-DISCOVERY | `board/stm32f4-discovery/` | STM32F407VG | DMA1 (0), DMA2 (1, mem-to-mem) |
| STM32F429I-DISCOVERY | `board/stm32f429i-discovery/` | STM32F429ZI | DMA1 (0), DMA2 (1, mem-to-mem) |
| STM32F746G-DISCO | `board/stm32f746g-disco/` | STM32F746NG | DMA1 (0), DMA2 (1, mem-to-mem) |
| STM32F769I-DISCOVERY | `board/stm32f769i-discovery/` | STM32F769NI | DMA1 (0), DMA2 (1, mem-to-mem) |

## MCU Configurations

| MCU | File |
|-----|------|
| STM32F401RE | `mcu/stm32f401re.ini` |
| STM32F405RG | `mcu/stm32f405rg.ini` |
| STM32F407VG | `mcu/stm32f407vg.ini` |
| STM32F411RE | `mcu/stm32f411re.ini` |
| STM32F429ZI | `mcu/stm32f429zi.ini` |
| STM32F439ZI | `mcu/stm32f439zi.ini` |
| STM32F446RE | `mcu/stm32f446re.ini` |
| STM32F469NI | `mcu/stm32f469ni.ini` |
| STM32F722RE | `mcu/stm32f722re.ini` |
| STM32F746ZG | `mcu/stm32f746zg.ini` |
| STM32F767ZI | `mcu/stm32f767zi.ini` |
| STM32F769NI | `mcu/stm32f769ni.ini` |

Every entry above lists the same DMA1 (controller 0) / DMA2 (controller 1,
mem-to-mem capable) pair - every STM32F4/F7 part has exactly two DMA
controllers with eight streams each.
