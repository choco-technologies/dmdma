# dmdma API Reference

`dmdma` has no Built-in API of its own to link against - it is a `dmdrvi`
driver, reachable only through that DIF (open/close/ioctl/stat on a device
node, or `Dmod_GetNextDifModule()`/`Dmod_GetDifFunction()` for a module that
wants to call it dynamically without going through a mounted path - see
`dmdevfs`'s own `configure_driver()` for the canonical example of the
latter). This page documents `dmdma`'s side of that contract: the config
struct, device numbering, and the `dmdma_ioctl_cmd_t` commands. For the
underlying register-level API a port implements (and that a peripheral
driver's own port could call directly instead of going through a device
node), see [port-api.md](port-api.md).

## Types

### `dmdma_config_t` (`dmdma.h`)

```c
typedef struct
{
    dmdma_controller_t controller; /* Physical DMA controller this context manages (0-based) */
} dmdma_config_t;
```

Parsed from the `controller` ini key by `dmdma_dmdrvi_create()`. One context
manages exactly one controller; `controller` becomes the context's dmdrvi
major number (`controller=0` → `/dev/dmdma0`, `controller=1` → `/dev/dmdma1`).

### `dmdma_controller_t`, `dmdma_stream_t` (`dmdma_types.h`)

`uint8_t` identifiers for a physical DMA controller and a stream within it
(dmdrvi major and minor numbers respectively). Every stream a controller has
is its own openable device (`/dev/dmdmaN/0`, `/dev/dmdmaN/1`, ...) -
`dmdma_dmdrvi_create()` discovers the count via
`dmdma_port_get_stream_count()` and announces the rest via
`dmdrvi_device_available()`.

### `dmdma_direction_t`

```c
typedef enum
{
    dmdma_direction_memory_to_memory = 0,   /* Both ends are memory (e.g. bulk copy) */
    dmdma_direction_memory_to_peripheral,   /* Memory -> peripheral data register (e.g. UART TX) */
    dmdma_direction_peripheral_to_memory,   /* Peripheral data register -> memory (e.g. UART RX) */
} dmdma_direction_t;
```

`memory_to_memory` is only valid on a controller for which
`dmdma_port_supports_memory_to_memory(` is true (DMA2 on every STM32F4/F7
part checked so far, never DMA1).

### `dmdma_data_width_t`

```c
typedef enum
{
    dmdma_data_width_byte     = 1,
    dmdma_data_width_halfword = 2,
    dmdma_data_width_word     = 4,
} dmdma_data_width_t;
```

### `dmdma_priority_t`

```c
typedef enum
{
    dmdma_priority_low = 0,
    dmdma_priority_medium,
    dmdma_priority_high,
    dmdma_priority_very_high,
} dmdma_priority_t;
```

Arbitration priority against other streams active on the same controller at
the same time.

### `dmdma_request_t` / `DMDMA_REQUEST_NONE`

```c
typedef uint16_t dmdma_request_t;
#define DMDMA_REQUEST_NONE ((dmdma_request_t)0)
```

Selects which peripheral trigger a stream is bound to for
memory↔peripheral transfers (STM32F4/F7 `DMA_SxCR_CHSEL`, or a DMAMUX
request id on families that have one). The *value* is meaningless to
`dmdma` itself - it's defined by the port for the target architecture and
supplied by whichever peripheral driver's own port knows which request line
its hardware is wired to. Use `DMDMA_REQUEST_NONE` for
`dmdma_direction_memory_to_memory` transfers, which need no request line.
For a peripheral direction, the request is interpreted by the target port;
zero is a valid selector on STM32F4/F7 and means hardware DMA channel 0.
`DMDMA_REQUEST_NONE` is used only with memory-to-memory direction. The
STM32 port continues to accept values 8 through 15 as aliases for channels
0 through 7 so modules built against older releases keep working.

### `dmdma_transfer_config_t`

```c
typedef struct
{
    dmdma_direction_t   direction;
    dmdma_request_t     request;               /* DMDMA_REQUEST_NONE for mem-to-mem */
    const void         *source_address;
    void               *destination_address;
    dmdma_data_width_t  source_width;
    dmdma_data_width_t  destination_width;
    bool                source_increment;
    bool                destination_increment;
    bool                circular;              /* wrap instead of stopping at element_count */
    dmdma_priority_t    priority;
    size_t              element_count;         /* elements, not bytes */
} dmdma_transfer_config_t;
```

The argument to `dmdma_ioctl_cmd_start_transfer`. Which address is "the
peripheral one" follows the direction, exactly like the underlying
PAR/M0AR register pair:

| `direction`              | `source_address`     | `destination_address` |
|---------------------------|-----------------------|------------------------|
| `peripheral_to_memory`    | peripheral register  | memory buffer          |
| `memory_to_peripheral`    | memory buffer        | peripheral register    |
| `memory_to_memory`        | memory buffer        | memory buffer          |

### `dmdma_stream_options_t` (optional, `dmdma_lease_start_ex()`)

```c
typedef struct
{
    dmdma_flow_controller_t flow_controller;    /* dma (default) | peripheral */
    dmdma_fifo_threshold_t  fifo_threshold;     /* direct (default) | quarter | half | three_quarters | full */
    dmdma_burst_t           source_burst;       /* single (default) | 4 | 8 | 16 beats */
    dmdma_burst_t           destination_burst;
} dmdma_stream_options_t;
```

A zero-initialized structure (or `NULL`) is exactly the behavior of
`dmdma_lease_start()`. Rules checked by the core before the port is touched
(`-EINVAL` otherwise):

- bursts need FIFO mode; direct mode also needs equal source/destination widths,
- the memory-side burst (beats x memory element width) must fit the FIFO
  threshold (4/8/12/16 bytes) an integral number of times,
- `dmdma_flow_controller_peripheral` is only valid for memory<->peripheral,
  non-circular transfers; the hardware then ignores `element_count` and the
  peripheral ends the transfer (e.g. SDIO/SDMMC).

In FIFO mode the FIFO-error flag is not reported as `dmdma_event_error`
(the FIFO error interrupt is not enabled); in direct mode it still is.

## Lease API (`dmdma_lease.h`)

| Function | Description |
|----------|-------------|
| `dmdma_lease_acquire(controller, stream)` | Reserve a stream (shared pool with `/dev/dmdmaN/M`) |
| `dmdma_lease_release(lease)` | Abort, disable the IRQ, return the stream |
| `dmdma_lease_start(lease, config)` | Validate and start a transfer (direct mode, single transfers) |
| `dmdma_lease_start_ex(lease, config, options)` | Same, with `dmdma_stream_options_t` |
| `dmdma_lease_abort(lease)` / `_is_busy()` / `_get_remaining()` | Transfer control |
| `dmdma_lease_set_callback(lease, callback, user_ptr)` | Completion/error/timeout/abort callback |

### `dmdma_event_t`

```c
typedef enum
{
    dmdma_event_complete      = (1 << 0), /* element_count elements transferred (or a circular wrap point) */
    dmdma_event_half_complete = (1 << 1), /* half of element_count transferred - circular only */
    dmdma_event_error         = (1 << 2), /* transfer, FIFO, or direct-mode error */
} dmdma_event_t;
```

A bitmask, since more than one flag can be observed in the same hardware
status read (e.g. `complete` together with `error`).

### `dmdma_interrupt_handler_t`

```c
typedef void (*dmdma_interrupt_handler_t)(dmdrvi_context_t context, void *handle, dmdma_event_t event);
```

Registered via `dmdma_ioctl_cmd_set_interrupt_handler`; called from the
stream's ISR when one of `event`'s bits fires. `handle` is the same value
`dmdrvi_open()` returned for that stream.

## IOCTL Commands (`dmdma_ioctl_cmd_t`)

All commands operate on a handle returned by `dmdrvi_open()` (equivalently,
`open()` on a `/dev/dmdmaN/M` path).

| Command | `arg` | Description |
|---|---|---|
| `dmdma_ioctl_cmd_start_transfer` | `const dmdma_transfer_config_t*` | Configure and start a transfer on this stream. Returns `-ENOTSUP` if `direction` is `memory_to_memory` on a controller that doesn't support it. |
| `dmdma_ioctl_cmd_stop_transfer` | `NULL` | Abort a transfer in progress. |
| `dmdma_ioctl_cmd_is_busy` | `bool*` | Whether a transfer is currently in flight. |
| `dmdma_ioctl_cmd_get_remaining` | `size_t*` | Elements left in the current/last transfer. |
| `dmdma_ioctl_cmd_set_interrupt_handler` | `dmdma_interrupt_handler_t*`, or `NULL` to remove | Register/unregister the completion/error callback for this stream. |

`dmdrvi_flush()` on a `dmdma` handle blocks until the stream's current
transfer finishes (or times out), mirroring `dmuart`'s flush waiting for TX
complete. `dmdrvi_read()`/`dmdrvi_write()` are always unsupported (return 0)
- there is no byte-stream semantics for a DMA stream, only `ioctl()`.

## Examples

See [dmdma.md](dmdma.md) for full worked examples (memory-to-memory copy
through a device node, a peripheral-triggered transfer sketch, and
asynchronous completion via an interrupt handler), and
`tests/dmdma_test_dev.c` in this repo for a complete, runnable one.
