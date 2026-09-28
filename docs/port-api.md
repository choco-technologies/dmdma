# dmdma_port API Reference

`dmdma_port` is the architecture-specific half of the driver (see
`dmf-man dmdma` / [dmdma.md](dmdma.md) for the overall driver+port picture).
It implements the register-level `dmod_dmdma_port_api(...)` functions
declared in `include/dmdma_port.h`, which `dmdma.c` calls to turn a
`dmdrvi_open()`/`ioctl()` call into real hardware activity.

**This API is a Built-in API, not a DIF** - a module that wants to call it
directly (rather than going through a `/dev/dmdmaN/M` device node) links
`dmdma_port_if` at build time (`dmod_link_modules(your_module dmdma_port)`
or `target_link_libraries(your_target dmdma_port_if)`) and calls these
functions like ordinary C functions; the loader resolves them dynamically at
runtime, same as any other DMOD Built-in API. This is what `dmdma.c` itself
does, and what `tests/dmdma_test_port.c` in this repo does as a
hardware-level smoke test with no `dmdevfs`/`dmdrvi`/config involved.

Currently supported architectures: `stm32f4`, `stm32f7` (identical DMA
controller IP block, so nearly all logic is shared - see
[port-implementation.md](port-implementation.md)).

## Capability Queries

```c
uint8_t dmdma_port_get_stream_count(dmdma_controller_t controller);
```
Number of streams `controller` has (8 on every currently supported STM32F4/F7
part), or `0` if `controller` isn't a valid controller for this port. Drives
how many minor devices (`/dev/dmdmaN/0` .. `/dev/dmdmaN/<count-1>`) `dmdma.c`
announces for that controller.

```c
bool dmdma_port_supports_memory_to_memory(dmdma_controller_t controller);
```
Whether `controller` can perform `dmdma_direction_memory_to_memory`
transfers - a hardware fact (only DMA2 can, on every STM32F4/F7 part checked
so far), not something software can enable on DMA1.

## Stream Reservation

```c
int  dmdma_port_stream_acquire(dmdma_controller_t controller, dmdma_stream_t stream);
void dmdma_port_stream_release(dmdma_controller_t controller, dmdma_stream_t stream);
```
Claim/release the underlying hardware resource for a stream (clock gating,
register reset to a known-idle state). Reservation *bookkeeping* - which
streams are currently considered "open" - is architecture-independent and
lives in `dmdma.c`; these two only touch registers for a stream the caller
has already determined is free. `_acquire()` returns `0` on success, a
negative value if `controller`/`stream` is out of range.

## Transfer Control

```c
int dmdma_port_stream_start(dmdma_controller_t controller, dmdma_stream_t stream,
                             const dmdma_transfer_config_t *config);
```
Configure *and* enable the stream in one call - re-arming a stream always
means a full reconfigure, so there is no separate "configure without
starting" step. Programs source/destination addresses, element widths,
increment flags, circular mode, priority, and (for peripheral transfers)
the request line, then enables the stream. The IRQ flags needed for
completion/half-complete/error reporting are added automatically. Returns
`0` on success, a negative value if `config` is invalid (e.g.
`element_count` is `0` or exceeds the hardware counter width) or
`controller`/`stream` is out of range.

```c
int dmdma_port_stream_start_ex(dmdma_controller_t controller, dmdma_stream_t stream,
                                const dmdma_transfer_config_t *config,
                                const dmdma_stream_options_t *options);
```
Same as `_stream_start()` plus FIFO mode/threshold, burst lengths and the
flow controller (`options` already validated by the core; `NULL` means the
`_stream_start()` defaults). On STM32F4/F7: `DMA_SxFCR.DMDIS`/`FTH`,
`DMA_SxCR.PBURST`/`MBURST` (mapped from source/destination by direction)
and `DMA_SxCR.PFCTRL`. `_stream_start()` is `_stream_start_ex(..., NULL)`.

```c
void dmdma_port_stream_stop(dmdma_controller_t controller, dmdma_stream_t stream);
```
Abort a transfer in progress (or a no-op if the stream isn't running).

```c
bool dmdma_port_stream_is_busy(dmdma_controller_t controller, dmdma_stream_t stream);
```
Whether a transfer is currently in flight on this stream.

```c
size_t dmdma_port_stream_get_remaining(dmdma_controller_t controller, dmdma_stream_t stream);
```
Elements left in the current (or most recently completed) transfer - `0`
once a non-circular transfer finishes.

## Interrupt Handler Registration

```c
int dmdma_port_add_interrupt_handler(dmdma_port_interrupt_handler_t handler, void *user_ptr);
int dmdma_port_remove_interrupt_handler(void *user_ptr);
```
A single dispatcher covers every controller/stream this port manages -
`dmdma.c` registers exactly one, at `dmod_init()`, and uses the
`(controller, stream)` the port hands back in each callback to look up which
reserved stream's `dmdma_interrupt_handler_t` to invoke (see
`internal_interrupt_handler()` in `src/dmdma.c`). Same shape as
`dmfmc_port_add_interrupt_handler()`/`dmuart_port_add_interrupt_handler()`.
`user_ptr` is passed back unchanged to `handler` on every event; pass the
same `user_ptr` to `_remove_interrupt_handler()` to unregister (a mismatched
`user_ptr` is a no-op, so unrelated callers can't accidentally clear each
other's registration).

```c
typedef void (*dmdma_port_interrupt_handler_t)(void *user_ptr, dmdma_controller_t controller,
                                                dmdma_stream_t stream, dmdma_event_t event);
```

## Example: raw register-level transfer (no `dmdevfs`/`dmdrvi`)

```c
#include "dmdma_port.h"

/* DMA2 (controller 1) is the only mem-to-mem capable controller */
#define CONTROLLER 1U
#define STREAM     0U

if (dmdma_port_get_stream_count(CONTROLLER) == 0 ||
    !dmdma_port_supports_memory_to_memory(CONTROLLER)) {
    /* not available on this target */
    return -1;
}

if (dmdma_port_stream_acquire(CONTROLLER, STREAM) != 0) {
    return -1; /* already reserved, or out of range */
}

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
    .element_count          = size,
};

if (dmdma_port_stream_start(CONTROLLER, STREAM, &transfer) != 0) {
    dmdma_port_stream_release(CONTROLLER, STREAM);
    return -1;
}

while (dmdma_port_stream_is_busy(CONTROLLER, STREAM)) {
    /* poll, or register an interrupt handler instead - see below */
}

size_t remaining = dmdma_port_stream_get_remaining(CONTROLLER, STREAM);
dmdma_port_stream_release(CONTROLLER, STREAM);
/* remaining == 0 on success */
```

See `tests/dmdma_test_port.c` in this repo for the complete, runnable
version of this example (an on-target diagnostic -
`dmdma_test_port /path/to/file`).

## Example: interrupt-driven completion

```c
static void on_dma_event(void *user_ptr, dmdma_controller_t controller,
                          dmdma_stream_t stream, dmdma_event_t event)
{
    if (event & dmdma_event_complete) { /* ... */ }
    if (event & dmdma_event_error)    { /* ... */ }
}

dmdma_port_add_interrupt_handler(on_dma_event, my_context);
/* ... acquire + start a transfer as above ... */
/* later: */
dmdma_port_remove_interrupt_handler(my_context);
```

## See Also

[dmdma.md](dmdma.md) (driver+port overview, device-node examples),
[api.md](api.md) (`dmdma`'s `dmdrvi`-facing types/ioctl commands),
[port-implementation.md](port-implementation.md) (adding a new MCU port)
