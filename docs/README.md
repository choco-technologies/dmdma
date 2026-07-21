# dmdma / dmdma_port Documentation

This directory ships in **both** the `dmdma` and `dmdma_port` release
packages (see each module's `.dmr`), so it covers the whole driver+port
pair regardless of which one you installed.

## Contents

- **[dmdma.md](dmdma.md)** - Driver overview: numbering scheme, configuration,
  and usage examples (device-node memory-to-memory copy, a
  peripheral-triggered transfer sketch, interrupt-driven completion).
- **[api.md](api.md)** - `dmdma`'s `dmdrvi`-facing API: config struct, types,
  and the full `dmdma_ioctl_cmd_t` reference.
- **[port-api.md](port-api.md)** - `dmdma_port`'s Built-in API: every
  `dmdma_port_*` function, with a raw register-level usage example and an
  interrupt-driven one.
- **[port-implementation.md](port-implementation.md)** - How to add a new
  MCU port.

## Quick Reference

```c
/* Consuming dmdma through the dmdrvi device node (the common case): */
#include "dmdma_types.h"
int fd = open("/dev/dmdma1/0", O_RDWR);

/* Consuming dmdma_port directly (driver authors, see port-api.md): */
#include "dmdma_port.h"
```

View documentation using `dmf-man`:

```bash
dmf-man dmdma                      # dmdma.md - overview and examples
dmf-man dmdma api                  # api.md - dmdma types/ioctl reference
dmf-man dmdma_port port-api        # port-api.md - dmdma_port function reference
dmf-man dmdma port-implementation  # adding a new MCU port
```
