# DMDMA(3)

## NAME

dmdma - DMOD DMA Controller Driver Module

## SYNOPSIS

```c
#include <fcntl.h>
#include "dmdma_types.h"

int fd = open("/dev/dmdma1/0", O_RDWR);

dmdma_transfer_config_t transfer = { ... };
ioctl(fd, dmdma_ioctl_cmd_start_transfer, &transfer);

bool busy;
ioctl(fd, dmdma_ioctl_cmd_is_busy, &busy);

size_t remaining;
ioctl(fd, dmdma_ioctl_cmd_get_remaining, &remaining);

ioctl(fd, dmdma_ioctl_cmd_stop_transfer, NULL);
close(fd);
```

## DESCRIPTION

**dmdma** is a driver module implementing the `dmdrvi` interface for STM32
DMA controllers. It ships as two DMOD modules: the architecture-independent
`dmdma` (this one - config parsing, device numbering, the `dmdrvi` DIF) and
`dmdma_port` (the architecture-specific register-level implementation - see
`dmf-man dmdma_port` / [port-api.md](port-api.md)).

Unlike a byte-stream device (`dmuart`, `dmtty`), a DMA controller has no
natural read()/write() semantics - a stream is configured and driven
entirely through `ioctl()`. `dmdma` therefore leaves `read()`/`write()`
unsupported (always return 0) and exposes everything through the
`dmdma_ioctl_cmd_t` commands in `dmdma_types.h`.

### Controllers, streams, and device numbering

One `dmdma` context (one `dmdrvi_create()` call, one `[section]` in a config
file) manages exactly one physical DMA controller. `controller` becomes the
context's dmdrvi *major* number - controller 0 (DMA1) is exposed as
`/dev/dmdma0`, controller 1 (DMA2) as `/dev/dmdma1`.

A controller's individual streams are not listed in the config - `dmdma`
asks the port how many streams the controller has
(`dmdma_port_get_stream_count()`) and announces each one as its own *minor*
device of that same context, so DMA1 (8 streams on STM32F4/F7) shows up as
`/dev/dmdma0/0` through `/dev/dmdma0/7`.

**Opening a stream reserves it; closing releases it.** Unlike `dmuart`
(where the whole device is one always-available stream), a `dmdma` stream is
an exclusive hardware resource: `open()` fails with a NULL handle if that
stream is already open elsewhere. This is also how a peripheral driver
claims a specific stream/channel pairing dictated by the silicon (e.g. "RX
for USART2 is wired to DMA1 Stream5 Channel4") - it just opens
`/dev/dmdma0/5` directly, the same way `dmtty` attaches to an arbitrary
backing file.

Only one controller can perform memory-to-memory transfers - a hardware
limitation, not a software choice (`dmdma_port_supports_memory_to_memory()`
returns `false` for DMA1 on every STM32F4/F7 part checked so far, `true` for
DMA2).

### Configuration

```ini
[dma1]
driver_name=dmdma
driver_order=2
controller=0
```

`controller` is the only key. See [configuration files](../configs/README.md)
for the full set of shipped board/MCU configs, and `docs/dmdma.md` (this
file) plus [api.md](api.md) for what happens once a context exists.

## EXAMPLES

### Memory-to-memory copy through a device node

The most common use from outside the driver framework: open a stream on the
mem-to-mem-capable controller, hand it a source and destination buffer, and
wait.

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "dmdma_types.h"

int fd = open("/dev/dmdma1/0", O_RDWR);   /* DMA2 (controller 1), stream 0 */
if (fd < 0) {
    perror("/dev/dmdma1/0");
    return -1;
}

uint8_t src[64], dst[64];
/* ... fill src ... */

dmdma_transfer_config_t transfer = {
    .direction             = dmdma_direction_memory_to_memory,
    .request                = DMDMA_REQUEST_NONE,
    .source_address         = src,
    .destination_address    = dst,
    .source_width           = dmdma_data_width_byte,
    .destination_width      = dmdma_data_width_byte,
    .source_increment       = true,
    .destination_increment  = true,
    .circular               = false,
    .priority               = dmdma_priority_medium,
    .element_count          = sizeof(src),
};

if (ioctl(fd, dmdma_ioctl_cmd_start_transfer, &transfer) != 0) {
    perror("start_transfer");
    close(fd);
    return -1;
}

bool busy = true;
while (busy) {
    ioctl(fd, dmdma_ioctl_cmd_is_busy, &busy);
}

size_t remaining = 0;
ioctl(fd, dmdma_ioctl_cmd_get_remaining, &remaining);
/* remaining == 0 and dst now equals src */

close(fd);
```

See `tests/dmdma_test_dev.c` in this repo for a complete, runnable version
of this example (an on-target diagnostic - `dmdma_test_dev
/dev/dmdma1/0`).

### Peripheral-triggered transfer (from a consumer driver's port)

A peripheral driver (e.g. a future DMA-backed `dmuart` RX path) that already
knows which controller/stream/request line its hardware is wired to opens
that exact stream directly and drives it the same way, just with a real
peripheral address and request line instead of `DMDMA_REQUEST_NONE`:

```c
dmdma_transfer_config_t rx = {
    .direction             = dmdma_direction_peripheral_to_memory,
    .request                = 4,                    /* e.g. USART2_RX request/CHSEL */
    .source_address         = (const void *)&USART2->RDR,
    .destination_address    = rx_buffer,
    .source_width           = dmdma_data_width_byte,
    .destination_width      = dmdma_data_width_byte,
    .source_increment       = false,   /* peripheral register - fixed address */
    .destination_increment  = true,
    .circular               = true,    /* keep receiving into a ring */
    .priority               = dmdma_priority_high,
    .element_count           = sizeof(rx_buffer),
};

int fd = open("/dev/dmdma0/5", O_RDWR);  /* DMA1 Stream5, wired to USART2_RX */
ioctl(fd, dmdma_ioctl_cmd_start_transfer, &rx);
/* ... register for dmdma_event_half_complete/dmdma_event_complete via
 *     dmdma_ioctl_cmd_set_interrupt_handler to drain the ring as it fills ... */
```

### Asynchronous completion via an interrupt handler

```c
static void on_dma_event(dmdrvi_context_t context, void *handle, dmdma_event_t event)
{
    if (event & dmdma_event_complete)  { /* ... */ }
    if (event & dmdma_event_error)     { /* ... */ }
}

dmdma_interrupt_handler_t handler = on_dma_event;
ioctl(fd, dmdma_ioctl_cmd_set_interrupt_handler, &handler);

/* start a transfer as above - on_dma_event() fires from the stream's ISR */

/* later, to stop receiving callbacks: */
ioctl(fd, dmdma_ioctl_cmd_set_interrupt_handler, NULL);
```

## SEE ALSO

dmdrvi(3), dmini(3), [api.md](api.md) (full type/ioctl reference),
[port-api.md](port-api.md) (`dmdma_port` Built-in API, for driver authors),
[port-implementation.md](port-implementation.md) (adding a new MCU port),
[configs/README.md](../configs/README.md) (configuration files)

## AUTHOR

Patryk Kubiak

## LICENSE

MIT License - Copyright (c) 2025 Choco-Technologies
