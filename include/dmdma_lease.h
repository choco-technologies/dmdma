#ifndef DMDMA_LEASE_H
#define DMDMA_LEASE_H

#include "dmod.h"
#include "dmdma_defs.h"
#include "dmdma_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DMA lease API
 *
 * An alternative to opening /dev/dmdmaN/M through dmdrvi, for peripheral
 * drivers (dmuart, dmspi, dmsdio, ...) that want to reserve and drive a DMA
 * stream directly from their own code instead of going through the VFS path
 * lookup. Both reservation paths share the exact same bookkeeping in
 * dmdma.c - a stream already open as a device node cannot also be leased,
 * and vice versa (see "Streams are devices, not a pool" in docs/README.md).
 *
 * This is a plain Module API (dmod's README, "Module API" section) - dmdma
 * is the one and only implementation of it, not a plugin-style interface
 * with several interchangeable backends (that's what DIF is for, e.g.
 * dmfsi with dmramfs/dmffs simultaneously). A consumer just includes this
 * header and calls the functions directly, e.g. dmdma_lease_acquire(...) -
 * no Dmod_GetModuleContext()/Dmod_GetDifFunction() lookup involved, same as
 * calling dmdma_port_stream_start() from within dmdma.c itself. Whether a
 * given board actually needs DMA is a *build-time* choice (whether "dmdma"
 * is declared as a dependency and bundled into that firmware image at
 * all - see dmod_link_modules() in the consumer's CMakeLists.txt), not a
 * runtime NULL-check.
 */

/**
 * @brief Opaque handle to one leased (controller, stream) pair
 */
struct dmdma_lease;
typedef struct dmdma_lease *dmdma_lease_t;

/**
 * @brief Lease event callback
 *
 * Fired from interrupt context for dmdma_event_complete/_half_complete/_error
 * (whatever the hardware reported) or dmdma_event_timeout (the
 * dmdma_transfer_config_t.timeout_ms watchdog armed at dmdma_lease_start()
 * elapsed - see dmdma.c), and fired synchronously (from within the calling
 * thread) with dmdma_event_aborted by dmdma_lease_abort() itself.
 *
 * @param lease    The lease the event belongs to (the same value _lease_acquire() returned)
 * @param event    Which event(s) fired
 * @param user_ptr Caller-owned pointer supplied to dmdma_lease_set_callback(), unchanged
 */
typedef void (*dmdma_lease_callback_t)(dmdma_lease_t lease, dmdma_event_t event, void *user_ptr);

/* --- Reservation ---
 *
 * Fails (returns NULL) if controller/stream is out of range for this build,
 * or the stream is already reserved through any API combination.
 */
dmod_dmdma_api(1.0, dmdma_lease_t, _lease_acquire, ( dmdma_controller_t controller, dmdma_stream_t stream ) );

/**
 * Release the lease: aborts any transfer in flight, disables the interrupt
 * routing for the stream (so a racing ISR cannot fire into it once this
 * returns), then returns the stream to the free pool. A driver must release
 * every lease it holds before it unloads - dmdma has no way to know when a
 * consumer module goes away, so nothing else guarantees a stale callback
 * can never be invoked after that point.
 */
dmod_dmdma_api(1.0, void, _lease_release, ( dmdma_lease_t lease ) );

/* --- Transfer control ---
 *
 * Same semantics and validation as dmdma_ioctl_cmd_start_transfer /
 * dmdma_ioctl_cmd_stop_transfer on the device-node path - controller,
 * request-line/direction consistency, address alignment, and element count
 * are all checked before anything touches the port.
 */
dmod_dmdma_api(1.0, int,    _lease_start,         ( dmdma_lease_t lease, const dmdma_transfer_config_t *config ) );

/**
 * Same as dmdma_lease_start() with explicit stream options - FIFO mode and
 * threshold, burst lengths and the flow controller (see
 * dmdma_stream_options_t). options == NULL behaves exactly like
 * dmdma_lease_start(). Invalid combinations (burst without FIFO, a memory
 * burst that does not fit the FIFO threshold, peripheral flow control for a
 * memory-to-memory or circular transfer, ...) are rejected with -EINVAL
 * before the port is touched.
 */
dmod_dmdma_api(1.0, int,    _lease_start_ex,      ( dmdma_lease_t lease, const dmdma_transfer_config_t *config,
                                                     const dmdma_stream_options_t *options ) );

/**
 * Abort whatever transfer is in flight; safe to call when idle. Fires the
 * lease's callback with dmdma_event_aborted, but only if a transfer was
 * actually in flight at the time of the call - aborting an already-idle
 * (or already-finished) stream is a no-op and does not fire a spurious
 * event on top of whatever dmdma_event_complete already fired for it.
 */
dmod_dmdma_api(1.0, void,   _lease_abort,         ( dmdma_lease_t lease ) );
dmod_dmdma_api(1.0, bool,   _lease_is_busy,       ( dmdma_lease_t lease ) );
dmod_dmdma_api(1.0, size_t, _lease_get_remaining, ( dmdma_lease_t lease ) );

/**
 * Register (callback != NULL) or remove (callback == NULL) this lease's
 * event callback.
 */
dmod_dmdma_api(1.0, int, _lease_set_callback, ( dmdma_lease_t lease, dmdma_lease_callback_t callback, void *user_ptr ) );

#ifdef __cplusplus
}
#endif

#endif // DMDMA_LEASE_H
