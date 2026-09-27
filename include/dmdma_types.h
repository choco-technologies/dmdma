#ifndef DMDMA_TYPES_H
#define DMDMA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief DMA controller identifier (dmdrvi major number)
 *
 * One dmdma context (one dmdrvi_create() call, one ini section) manages
 * exactly one physical DMA controller - e.g. controller 0 = DMA1,
 * controller 1 = DMA2 on STM32F4/F7. Exposed as /dev/dmdmaN.
 */
typedef uint8_t dmdma_controller_t;

/**
 * @brief DMA stream/channel identifier within a controller (dmdrvi minor number)
 *
 * Every stream a controller has is a separately-openable dmdrvi device
 * (/dev/dmdmaN/0, /dev/dmdmaN/1, ...) - see "Streams are devices, not a
 * pool" in docs/README.md. Opening one reserves it; closing releases it.
 */
typedef uint8_t dmdma_stream_t;

/**
 * @brief Direction of a DMA transfer
 */
typedef enum
{
    dmdma_direction_memory_to_memory = 0,   /**< Both ends are memory (e.g. bulk copy) */
    dmdma_direction_memory_to_peripheral,   /**< Memory -> peripheral data register (e.g. UART TX) */
    dmdma_direction_peripheral_to_memory,   /**< Peripheral data register -> memory (e.g. UART RX) */
} dmdma_direction_t;

/**
 * @brief Width of a single transferred element
 */
typedef enum
{
    dmdma_data_width_byte     = 1,
    dmdma_data_width_halfword = 2,
    dmdma_data_width_word     = 4,
} dmdma_data_width_t;

/**
 * @brief Arbitration priority of a stream relative to other active streams
 */
typedef enum
{
    dmdma_priority_low = 0,
    dmdma_priority_medium,
    dmdma_priority_high,
    dmdma_priority_very_high,
} dmdma_priority_t;

/**
 * @brief Peripheral DMA request line selector
 *
 * Selects which peripheral trigger a stream is bound to for
 * memory<->peripheral transfers (e.g. STM32F4/F7 DMA_SxCR_CHSEL, or a
 * DMAMUX request id on families that have one). The value is meaningless to
 * the architecture-independent core - it is defined and interpreted by the
 * dmdma port for the target architecture, and supplied by whichever
 * peripheral driver's own port (e.g. dmuart's stm32f7 port) knows which
 * request line its hardware is wired to. Use DMDMA_REQUEST_NONE for
 * dmdma_direction_memory_to_memory transfers, which need no request line.
 */
typedef uint16_t dmdma_request_t;

#define DMDMA_REQUEST_NONE ((dmdma_request_t)0)

/**
 * @brief Full description of a transfer to start on a reserved stream
 */
typedef struct
{
    dmdma_direction_t   direction;             /**< Transfer direction */
    dmdma_request_t     request;               /**< Peripheral request line (DMDMA_REQUEST_NONE for mem-to-mem) */
    const void         *source_address;        /**< Where elements are read from */
    void               *destination_address;   /**< Where elements are written to */
    dmdma_data_width_t  source_width;          /**< Element width at the source */
    dmdma_data_width_t  destination_width;     /**< Element width at the destination */
    bool                source_increment;      /**< Auto-increment the source address after each element */
    bool                destination_increment; /**< Auto-increment the destination address after each element */
    bool                circular;              /**< Wrap back to the start instead of stopping at element_count */
    dmdma_priority_t    priority;              /**< Arbitration priority against other active streams */
    size_t              element_count;         /**< Number of elements (not bytes) to transfer */
    uint32_t            timeout_ms;            /**< Abort the transfer and fire dmdma_event_timeout if it hasn't
                                                 *   reached dmdma_event_complete within this many milliseconds of
                                                 *   being started (dmosi_timer-backed, see dmdma.c); 0 = no
                                                 *   watchdog, wait indefinitely. For a circular transfer this is a
                                                 *   total-run-time cap (dmdma_event_half_complete wrap points do
                                                 *   not reset it) rather than a per-element deadline. */
} dmdma_transfer_config_t;

/*
 * Which address is "the peripheral one" depends on direction, exactly as it
 * does in the underlying hardware register pair (PAR/M0AR on STM32):
 *   - peripheral_to_memory: source_address = peripheral register, destination_address = memory buffer
 *   - memory_to_peripheral: source_address = memory buffer, destination_address = peripheral register
 *   - memory_to_memory:     both addresses are plain memory - `request` is ignored,
 *                           and this direction is only valid on a controller for
 *                           which dmdma_port_supports_memory_to_memory() is true
 */

/**
 * @brief Reason a DMA completion/error event fired
 *
 * A bitmask so a single dmhaman call can report more than one flag observed
 * in the same status read (e.g. transfer-complete together with a FIFO
 * error), same shape as dmuart_int_trigger_t / dmfmc_interrupt_event_t.
 */
typedef enum
{
    dmdma_event_complete      = (1 << 0), /**< element_count elements transferred (or a circular wrap point) */
    dmdma_event_half_complete = (1 << 1), /**< Half of element_count transferred - circular transfers only */
    dmdma_event_error         = (1 << 2), /**< Transfer, FIFO, or direct-mode error reported by the hardware */
    dmdma_event_aborted       = (1 << 3), /**< Fired synchronously by dmdma_lease_abort() / dmdma_ioctl_cmd_stop_transfer
                                            *   while a transfer was actually in flight - an explicit, caller-initiated cancel */
    dmdma_event_timeout       = (1 << 4), /**< dmdma_transfer_config_t.timeout_ms elapsed before the transfer
                                            *   completed - dmdma itself stopped the stream via the dmosi_timer
                                            *   armed at dmdma_lease_start()/dmdma_ioctl_cmd_start_transfer time */
} dmdma_event_t;

/**
 * @brief IOCTL commands for the DMDMA device
 *
 * Sent to a handle returned by opening a specific stream device
 * (/dev/dmdmaN/M). There is no read()/write() support - a stream is
 * configured and driven entirely through these commands, matching how
 * dnx-rtos's own DMA driver and every other dmod driver with no natural
 * byte-stream semantics (dmfmc, dmgpio) expose themselves.
 */
typedef enum
{
    dmdma_ioctl_cmd_start_transfer = 1,     /**< arg = const dmdma_transfer_config_t* - configure and start */
    dmdma_ioctl_cmd_stop_transfer,          /**< Abort a transfer in progress; arg = NULL */
    dmdma_ioctl_cmd_is_busy,                /**< arg = bool* - whether a transfer is currently in flight */
    dmdma_ioctl_cmd_get_remaining,          /**< arg = size_t* - elements left in the current/last transfer */
    dmdma_ioctl_cmd_set_interrupt_handler,  /**< arg = dmdma_interrupt_handler_t*, NULL to remove */

    dmdma_ioctl_cmd_max
} dmdma_ioctl_cmd_t;

/**
 * @brief Opaque driver context type (forward declaration)
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

/**
 * @brief Parameters passed to a dmhaman-registered interrupt handler
 */
typedef struct
{
    void          *handle;  /**< Stream handle (as returned by dmdrvi_open()) the event belongs to */
    dmdma_event_t  event;   /**< Which event(s) fired */
} dmdma_interrupt_params_t;

/**
 * @brief DMA interrupt handler function type (dmhaman-facing)
 *
 * @param context  Context of the driver instance (the controller)
 * @param handle   Stream handle the event belongs to
 * @param event    Which event(s) fired
 */
typedef void (*dmdma_interrupt_handler_t)(dmdrvi_context_t context, void *handle, dmdma_event_t event);

/**
 * @brief DMA port interrupt handler function type
 *
 * Called by the port layer when a stream raises an interrupt. A single
 * handler is registered for the whole controller (see
 * dmdma_port_add_interrupt_handler() in dmdma_port.h) - core looks up which
 * reserved stream's dmdma_interrupt_handler_t to invoke from `stream`.
 *
 * @param user_ptr    User pointer supplied at registration time (driver context)
 * @param controller  Controller the interrupt belongs to
 * @param stream      Stream that raised the interrupt
 * @param event       Which event(s) fired
 */
typedef void (*dmdma_port_interrupt_handler_t)(void *user_ptr, dmdma_controller_t controller,
                                                dmdma_stream_t stream, dmdma_event_t event);

#endif /* DMDMA_TYPES_H */
