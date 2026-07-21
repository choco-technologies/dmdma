# dmdma

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmdma/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmdma/actions/workflows/ci.yml)

DMOD DMA controller driver module.

## Description

`dmdma` is a driver module implementing the `dmdrvi` interface for STM32 DMA
controllers (DMA1/DMA2 on STM32F4/F7). Each physical controller is exposed
as `/dev/dmdmaN`, with every stream the controller has as its own openable
minor device (`/dev/dmdmaN/0`, `/dev/dmdmaN/1`, ...) - opening one reserves
it, closing releases it. Transfers (memory-to-memory, or
peripheral↔memory for a consumer driver like a future DMA-backed `dmuart`)
are configured and driven entirely through `ioctl()`, since a DMA stream has
no natural byte-stream semantics.

See [docs/dmdma.md](docs/dmdma.md) for the full picture and usage examples.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Usage

```c
/* The common case: open a stream through the dmdrvi device node dmdevfs
 * exposes once dmdma is configured (see configs/). */
#include "dmdma_types.h"
int fd = open("/dev/dmdma1/0", O_RDWR);

/* Driver authors talking to the port directly instead - see
 * docs/port-api.md. */
#include "dmdma_port.h"
```

## Documentation

See the `docs/` directory:

- **[dmdma.md](docs/dmdma.md)** - Driver overview, numbering scheme,
  configuration, and usage examples
- **[api.md](docs/api.md)** - `dmdma`'s `dmdrvi`-facing types and full
  `dmdma_ioctl_cmd_t` reference
- **[port-api.md](docs/port-api.md)** - `dmdma_port`'s Built-in API, for
  driver authors calling it directly instead of going through a device node
- **[port-implementation.md](docs/port-implementation.md)** - How to add a
  new MCU port

View documentation using `dmf-man dmdma` / `dmf-man dmdma api` /
`dmf-man dmdma_port port-api`.

## Hardware Port

This module ships two DMOD modules: the architecture-independent
`dmdma` and `dmdma_port`, which contains the
architecture-specific implementation. The active architecture is selected via
`DMOD_CPU_FAMILY` (default: `stm32f7`; `stm32f4` is also supported):

```bash
cmake .. -DDMOD_CPU_FAMILY=stm32f7
cmake .. -DDMOD_CPU_FAMILY=stm32f4
```

stm32f4 and stm32f7 share the identical DMA controller IP block, so nearly
all port logic lives in `src/port/stm32_common/` and each family's
`src/port/<family>/port.c` is just a thin lifecycle + IRQ-forwarding shim.
See [docs/port-implementation.md](docs/port-implementation.md) for how to add
another architecture. Port-specific files:

```
├── include/
│   ├── dmdma_port.h
│   └── port/
│       ├── stm32_common_regs.h
│       ├── stm32f4_regs.h
│       └── stm32f7_regs.h
├── src/port/
│   ├── CMakeLists.txt
│   ├── stm32_common/
│   │   ├── stm32_common.h
│   │   └── stm32_common.c
│   ├── stm32f4/
│   │   ├── config.cmake
│   │   └── port.c
│   └── stm32f7/
│       ├── config.cmake
│       └── port.c
└── dmdma_port.dmr
```
## Project Structure

```
dmdma/
├── configs/           # Board/MCU config files read by dmdevfs at boot
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   ├── dmdma.h
│   ├── dmdma_port.h
│   └── dmdma_types.h
├── src/
│   ├── dmdma.c
│   └── port/          # See "Hardware Port" above
├── CMakeLists.txt
├── Makefile
├── dmdma.dmr
├── dmdma_port.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
