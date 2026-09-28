#ifndef DMDMA_PORT_H
#define DMDMA_PORT_H

#include "dmod_types.h"
#include "dmdma_port_defs.h"
#include "dmdma_types.h"

/* --- Capability queries ---
 *
 * Let the arch-independent core (dmdma_dmdrvi_create()) discover how many
 * streams a given controller has and whether it can do memory-to-memory
 * transfers, instead of hardcoding e.g. "2 controllers x 8 streams,
 * mem-to-mem only on the 2nd" into core code. Stream count drives how many
 * minor devices (/dev/dmdmaN/0 .. /dev/dmdmaN/<count-1>) the core announces
 * for a controller.
 */

dmod_dmdma_port_api(1.0, uint8_t, _get_stream_count, ( dmdma_controller_t controller ) );
dmod_dmdma_port_api(1.0, bool,    _supports_memory_to_memory, ( dmdma_controller_t controller ) );

/* --- Stream reservation ---
 *
 * Reservation bookkeeping itself (which streams are currently open) is
 * arch-independent and lives in dmdma.c, driven by dmdrvi_open()/_close();
 * these two just claim/release the underlying hardware resource (clock
 * gating, register reset) for a stream the core has already determined is
 * free.
 */

dmod_dmdma_port_api(1.0, int,  _stream_acquire, ( dmdma_controller_t controller, dmdma_stream_t stream ) );
dmod_dmdma_port_api(1.0, void, _stream_release, ( dmdma_controller_t controller, dmdma_stream_t stream ) );

/* --- Transfer control ---
 *
 * Implements the dmdma_ioctl_cmd_* commands declared in dmdma_types.h for
 * an already-reserved stream. _stream_start() both configures and enables
 * the stream in one call (like every other in-flight configuration on this
 * stream, re-arming always means a full reconfigure) - the IRQ flags needed
 * for the requested event(s) are added automatically.
 */

dmod_dmdma_port_api(1.0, int,    _stream_start, ( dmdma_controller_t controller, dmdma_stream_t stream,
                                                   const dmdma_transfer_config_t *config ) );
/* Like _stream_start(), with the stream options already validated by the
 * core (options == NULL means the _stream_start() defaults). */
dmod_dmdma_port_api(1.0, int,    _stream_start_ex, ( dmdma_controller_t controller, dmdma_stream_t stream,
                                                      const dmdma_transfer_config_t *config,
                                                      const dmdma_stream_options_t *options ) );
dmod_dmdma_port_api(1.0, void,   _stream_stop,  ( dmdma_controller_t controller, dmdma_stream_t stream ) );
dmod_dmdma_port_api(1.0, bool,   _stream_is_busy, ( dmdma_controller_t controller, dmdma_stream_t stream ) );
dmod_dmdma_port_api(1.0, size_t, _stream_get_remaining, ( dmdma_controller_t controller, dmdma_stream_t stream ) );

/* --- Interrupt handler registration ---
 *
 * A single handler covers every controller/stream this port manages - core
 * registers exactly one internal dispatcher here at dmod_init() and uses
 * the (controller, stream) the port passes back to look up which reserved
 * stream's dmdma_interrupt_handler_t to invoke. Same shape as
 * dmfmc_port_add_interrupt_handler()/dmuart_port_add_interrupt_handler().
 */

dmod_dmdma_port_api(1.0, int, _add_interrupt_handler,
    ( dmdma_port_interrupt_handler_t handler, void *user_ptr ) );
dmod_dmdma_port_api(1.0, int, _remove_interrupt_handler,
    ( void *user_ptr ) );

#endif // DMDMA_PORT_H
