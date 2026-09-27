#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmdma.h"
#include "dmdma_port.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmosi.h"

#include <errno.h>

/* "DMDA" packed into a uint32_t - see dmdrvi context magic-field convention */
#define DMDMA_CONTEXT_MAGIC   0x444D4441UL

/* Generous upper bound on physical DMA controllers a board can have - real
 * hardware (STM32F4/F7) has at most 2. */
#define DMDMA_MAX_CONTROLLERS 4U

/* Matches every currently supported architecture (STM32F4/F7: 8 streams per
 * controller). Bump alongside a port that has more. */
#define DMDMA_MAX_STREAMS     8U

#define DMDMA_FLUSH_TIMEOUT_ITERATIONS   1000000UL

/**
 * @brief Which API reserved a stream slot
 *
 * A slot is reserved through exactly one of two mutually exclusive paths -
 * dmdrvi_open() (device node) or dmdma_lease_acquire() (direct call) - and
 * this tag is what makes the two paths share one reservation pool instead of
 * fighting over separate bookkeeping. It also lets internal_interrupt_handler()
 * invoke the right callback shape (dmdrvi's vs. the lease's) for a given
 * stream, and stops a handle obtained from one API from being fed into the
 * other's calls (slot_from_handle()/slot_from_lease() each only accept their
 * own owner tag).
 */
typedef enum
{
    dmdma_slot_owner_none = 0,
    dmdma_slot_owner_dmdrvi,
    dmdma_slot_owner_lease,
} dmdma_slot_owner_t;

/**
 * @brief One DMA stream within a controller
 *
 * This is also the opaque handle dmdrvi_open()/_close() and
 * dmdma_lease_acquire()/_release() hand back and forth - a stream is
 * reserved for the lifetime of whichever call reserved it.
 */
typedef struct
{
    dmdma_stream_t      stream;    /**< Stream index within the controller */
    bool                reserved;  /**< Currently open/leased (== reserved on the port) */
    dmdma_slot_owner_t  owner;     /**< Which API currently holds this slot */
    union
    {
        dmdma_interrupt_handler_t  dmdrvi_handler; /**< owner == dmdma_slot_owner_dmdrvi */
        struct
        {
            dmdma_lease_callback_t callback;
            void                  *user_ptr;
        } lease;                                    /**< owner == dmdma_slot_owner_lease */
    } cb;
    /* Lazily created the first time a transfer on this slot sets
     * timeout_ms != 0 (see arm_timeout()/disarm_timeout()), then reused for
     * the rest of this slot's lifetime regardless of how many
     * open/close or acquire/release cycles it goes through - only
     * dmdrvi_free() (whole-context teardown) ever destroys it, so no
     * open/close/acquire/release path may reset this back to NULL without
     * leaking the dmosi_timer_t it points to. */
    dmosi_timer_t        timer;
    /* Whether the transfer currently armed on this stream is circular - the
     * hardware's "complete" flag fires at *every* wrap point for a circular
     * transfer, not just a final one (see stm32_dma_stream_irq()), so
     * internal_interrupt_handler() needs this to tell a genuinely-finished
     * one-shot transfer apart from a circular one that's merely still
     * going - otherwise the watchdog armed by arm_timeout() gets disarmed
     * on the very first wrap instead of running for its full timeout_ms. */
    bool                 circular;
} dmdma_stream_slot_t;

/**
 * @brief DMDRVI context structure - one per physical DMA controller
 */
struct dmdrvi_context
{
    uint32_t             magic;
    dmdma_config_t        config;
    uint8_t               stream_count;
    dmdma_stream_slot_t   streams[DMDMA_MAX_STREAMS];
};

/* Indexed by controller number so the single port-level interrupt dispatcher
 * (registered once, see dmod_init() below) can find which context - and
 * within it, which stream's handler - a hardware event belongs to. */
static dmdrvi_context_t g_contexts[DMDMA_MAX_CONTROLLERS];

static int is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == DMDMA_CONTEXT_MAGIC;
}

/* Validates that `handle` is one of `context`'s own stream slots and that it
 * is currently open *through dmdrvi*, before it gets dereferenced - a slot
 * reserved via dmdma_lease_acquire() is the same C type but must never be
 * driven through the dmdrvi ioctl/flush/etc. path (see slot_from_lease()
 * for the mirror-image check on the lease side). */
static dmdma_stream_slot_t *slot_from_handle(dmdrvi_context_t context, void *handle)
{
    if (!is_valid_context(context) || handle == NULL)
    {
        return NULL;
    }

    dmdma_stream_slot_t *slot = (dmdma_stream_slot_t *)handle;
    if (slot < &context->streams[0] || slot >= &context->streams[context->stream_count])
    {
        return NULL;
    }

    return (slot->reserved && slot->owner == dmdma_slot_owner_dmdrvi) ? slot : NULL;
}

/* Finds which controller's context owns `slot` - the lease API identifies a
 * stream purely by the opaque dmdma_lease_t it was handed back (unlike
 * dmdrvi, which always carries context alongside handle), so this is how
 * lease calls recover the controller number they need for port calls.
 * DMDMA_MAX_CONTROLLERS is small (<=4) so a linear scan is fine. */
static dmdrvi_context_t context_from_slot(dmdma_stream_slot_t *slot)
{
    for (uint32_t i = 0; i < DMDMA_MAX_CONTROLLERS; i++)
    {
        dmdrvi_context_t context = g_contexts[i];
        if (is_valid_context(context) && slot >= &context->streams[0] && slot < &context->streams[context->stream_count])
        {
            return context;
        }
    }

    return NULL;
}

/* Validates that `lease` is a real, currently-leased stream slot before it
 * gets dereferenced - the mirror image of slot_from_handle() for the lease
 * API (see the comment there for why the owner check matters). */
static dmdma_stream_slot_t *slot_from_lease(dmdma_lease_t lease)
{
    dmdma_stream_slot_t *slot = (dmdma_stream_slot_t *)lease;
    if (slot == NULL || context_from_slot(slot) == NULL)
    {
        return NULL;
    }

    return (slot->reserved && slot->owner == dmdma_slot_owner_lease) ? slot : NULL;
}

/* Shared by dmdma_ioctl_cmd_start_transfer and dmdma_lease_start() so both
 * reservation paths reject the same malformed configs the same way, rather
 * than relying on whatever a given port's _stream_start() happens to check.
 */
static int validate_transfer_config(dmdma_controller_t controller, const dmdma_transfer_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }
    if (config->element_count == 0)
    {
        return -EINVAL;
    }
    if (config->destination_address == NULL)
    {
        return -EINVAL;
    }

    if (config->direction == dmdma_direction_memory_to_memory)
    {
        if (!dmdma_port_supports_memory_to_memory(controller))
        {
            return -ENOTSUP;
        }
        if (config->request != DMDMA_REQUEST_NONE)
        {
            /* mem-to-mem needs no peripheral trigger - a caller supplying
             * one is almost certainly confused about which address is "the
             * peripheral one" for this direction. */
            return -EINVAL;
        }
        if (config->source_address == NULL)
        {
            return -EINVAL;
        }
    }
    else
    {
        if (config->source_address == NULL)
        {
            return -EINVAL;
        }
        if (config->request == DMDMA_REQUEST_NONE)
        {
            /* A peripheral direction with no request line can never fire -
             * almost certainly a caller that forgot to set it. */
            return -EINVAL;
        }
    }

    if (((uintptr_t)config->source_address % (uintptr_t)config->source_width) != 0)
    {
        return -EINVAL;
    }
    if (((uintptr_t)config->destination_address % (uintptr_t)config->destination_width) != 0)
    {
        return -EINVAL;
    }

    return 0;
}

/* [dmdma] is the fast path; anything else falls back to whichever section
 * comes first in the file, so e.g. [dma1]/[dma2]-named sections work too. */
static const char *detect_config_section(dmini_context_t config)
{
    if (dmini_has_key(config, "dmdma", "controller"))
    {
        return "dmdma";
    }

    if (dmini_section_count(config) > 0)
    {
        const char *first = dmini_section_name(config, 0);
        if (first != NULL)
        {
            return first;
        }
    }

    return "dmdma";
}

/* ---- Transfer watchdog (dmosi_timer-backed) ----
 *
 * dmdma_transfer_config_t.timeout_ms, armed by validate-and-start on either
 * reservation path. The timer object itself is created lazily and then
 * kept for the rest of the slot's lifetime (see the comment on
 * dmdma_stream_slot_t.timer) - only dmdrvi_free() ever destroys it.
 */
static void deliver_event(dmdrvi_context_t context, dmdma_stream_slot_t *slot, dmdma_event_t event)
{
    switch (slot->owner)
    {
        case dmdma_slot_owner_dmdrvi:
            if (slot->cb.dmdrvi_handler != NULL)
            {
                slot->cb.dmdrvi_handler(context, slot, event);
            }
            break;

        case dmdma_slot_owner_lease:
            if (slot->cb.lease.callback != NULL)
            {
                slot->cb.lease.callback((dmdma_lease_t)slot, event, slot->cb.lease.user_ptr);
            }
            break;

        default:
            break;
    }
}

static void dmdma_timeout_fired(void *arg);

static void disarm_timeout(dmdma_stream_slot_t *slot)
{
    if (slot->timer != NULL)
    {
        dmosi_timer_stop(slot->timer);
    }
}

static int arm_timeout(dmdma_stream_slot_t *slot, uint32_t timeout_ms)
{
    if (timeout_ms == 0)
    {
        disarm_timeout(slot);
        return 0;
    }

    if (slot->timer == NULL)
    {
        slot->timer = dmosi_timer_create(dmdma_timeout_fired, slot, timeout_ms, false /* one-shot */);
        if (slot->timer == NULL)
        {
            return -ENOMEM;
        }
    }
    else
    {
        dmosi_timer_stop(slot->timer);
        dmosi_timer_set_period(slot->timer, timeout_ms);
    }

    return dmosi_timer_start(slot->timer);
}

/* dmosi_timer callback (fires from timer/interrupt context, same as a real
 * hardware DMA event) - stops the stream and delivers dmdma_event_timeout to
 * whichever owner is still holding it. If the slot was released/closed in
 * the meantime, arm_timeout()'s disarm_timeout() call already stopped this
 * timer, so there is no unsynchronized-teardown window to worry about here. */
static void dmdma_timeout_fired(void *arg)
{
    dmdma_stream_slot_t *slot = (dmdma_stream_slot_t *)arg;
    dmdrvi_context_t context = context_from_slot(slot);
    if (context == NULL || !slot->reserved)
    {
        return;
    }

    dmdma_port_stream_stop(context->config.controller, slot->stream);
    deliver_event(context, slot, dmdma_event_timeout);
}

/* ---- Interrupt dispatch ----
 *
 * A single dispatcher is registered with the port once, at dmod_init() -
 * covering every controller/stream the port manages, since dmdma_port's own
 * _add_interrupt_handler() is a single global slot (see dmdma_port.h). The
 * port hands back (controller, stream) with each event, which is enough to
 * find the right context (g_contexts[]) and, within it, the right stream's
 * registered handler - no user_ptr needed.
 */
static void internal_interrupt_handler(void *user_ptr, dmdma_controller_t controller,
                                        dmdma_stream_t stream, dmdma_event_t event)
{
    (void)user_ptr;

    if (controller >= DMDMA_MAX_CONTROLLERS)
    {
        return;
    }

    dmdrvi_context_t context = g_contexts[controller];
    if (!is_valid_context(context) || stream >= context->stream_count)
    {
        return;
    }

    dmdma_stream_slot_t *slot = &context->streams[stream];
    if (!slot->reserved)
    {
        return;
    }

    /* A terminal event settles the watchdog race in the transfer's favor -
     * no point letting a timeout fire moments later for a transfer that
     * already finished (or already reported its own error). dmdma_event_complete
     * is only terminal for a one-shot transfer - a circular one fires it at
     * *every* wrap, so disarming there too would stop the watchdog after its
     * first lap instead of running for its full timeout_ms (see
     * dmdma_stream_slot_t.circular and timeout_ms's doc comment in
     * dmdma_types.h). */
    if (event & dmdma_event_error)
    {
        disarm_timeout(slot);
    }
    else if ((event & dmdma_event_complete) && !slot->circular)
    {
        disarm_timeout(slot);
    }

    deliver_event(context, slot, event);
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("DMDMA driver module initialized\n");

    for (uint32_t i = 0; i < DMDMA_MAX_CONTROLLERS; i++)
    {
        g_contexts[i] = NULL;
    }

    dmdma_port_add_interrupt_handler(internal_interrupt_handler, NULL);
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("DMDMA driver module deinitialized\n");
    dmdma_port_remove_interrupt_handler(NULL);
    return 0;
}

/* ---- DMDRVI DIF implementation ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, dmdrvi_context_t, _create,
    ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (dev_num == NULL)
    {
        DMOD_LOG_ERROR("dmdma_dmdrvi_create: dev_num is required\n");
        return NULL;
    }

    dmdma_controller_t controller = 0;
    if (config != NULL)
    {
        const char *section = detect_config_section(config);
        controller = (dmdma_controller_t)dmini_get_int(config, section, "controller", 0);
    }

    if (controller >= DMDMA_MAX_CONTROLLERS)
    {
        DMOD_LOG_ERROR("dmdma: controller %u out of range\n", (unsigned)controller);
        return NULL;
    }

    if (g_contexts[controller] != NULL)
    {
        DMOD_LOG_ERROR("dmdma: controller %u already has an active context\n", (unsigned)controller);
        return NULL;
    }

    uint8_t stream_count = dmdma_port_get_stream_count(controller);
    if (stream_count == 0 || stream_count > DMDMA_MAX_STREAMS)
    {
        DMOD_LOG_ERROR("dmdma: controller %u not supported by this port\n", (unsigned)controller);
        return NULL;
    }

    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
    {
        return NULL;
    }

    context->magic = DMDMA_CONTEXT_MAGIC;
    context->config.controller = controller;
    context->stream_count = stream_count;
    for (uint8_t i = 0; i < stream_count; i++)
    {
        context->streams[i].stream = i;
        context->streams[i].reserved = false;
        context->streams[i].owner = dmdma_slot_owner_none;
        context->streams[i].cb.dmdrvi_handler = NULL;
        context->streams[i].timer = NULL;
        context->streams[i].circular = false;
    }

    g_contexts[controller] = context;

    /* Stream 0 is reported directly as the create() dev_num; every other
     * stream on this controller is announced right away via
     * dmdrvi_device_available(), so e.g. controller 0 with 8 streams shows
     * up as /dev/dmdma0/0 .. /dev/dmdma0/7. */
    dev_num->major = controller;
    dev_num->minor = 0;
    dev_num->flags = DMDRVI_NUM_MAJOR | DMDRVI_NUM_MINOR;
    dev_num->alt_name[0] = '\0';

    for (uint8_t i = 1; i < stream_count; i++)
    {
        dmdrvi_dev_num_t extra;
        extra.major = controller;
        extra.minor = i;
        extra.flags = DMDRVI_NUM_MAJOR | DMDRVI_NUM_MINOR;
        extra.alt_name[0] = '\0';
        dmdrvi_device_available(context, &extra);
    }

    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
    {
        return;
    }

    for (uint8_t i = 0; i < context->stream_count; i++)
    {
        if (context->streams[i].reserved)
        {
            dmdma_port_stream_stop(context->config.controller, i);
            dmdma_port_stream_release(context->config.controller, i);
        }
        if (context->streams[i].timer != NULL)
        {
            dmosi_timer_destroy(context->streams[i].timer);
            context->streams[i].timer = NULL;
        }
    }

    g_contexts[context->config.controller] = NULL;
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, void*, _open,
    ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    (void)flags;

    if (!is_valid_context(context) || dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters in dmdma_dmdrvi_open\n");
        return NULL;
    }

    if (dev_num->major != context->config.controller || dev_num->minor >= context->stream_count)
    {
        DMOD_LOG_ERROR("dmdma: invalid stream %u/%u\n", (unsigned)dev_num->major, (unsigned)dev_num->minor);
        return NULL;
    }

    dmdma_stream_slot_t *slot = &context->streams[dev_num->minor];
    if (slot->reserved)
    {
        DMOD_LOG_ERROR("dmdma: stream %u already open\n", (unsigned)dev_num->minor);
        return NULL;
    }

    if (dmdma_port_stream_acquire(context->config.controller, slot->stream) != 0)
    {
        DMOD_LOG_ERROR("dmdma: failed to acquire stream %u\n", (unsigned)dev_num->minor);
        return NULL;
    }

    slot->reserved = true;
    slot->owner    = dmdma_slot_owner_dmdrvi;
    slot->cb.dmdrvi_handler = NULL;
    return slot;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    dmdma_stream_slot_t *slot = slot_from_handle(context, handle);
    if (slot == NULL)
    {
        return;
    }

    disarm_timeout(slot);
    dmdma_port_stream_stop(context->config.controller, slot->stream);
    dmdma_port_stream_release(context->config.controller, slot->stream);
    slot->reserved = false;
    slot->owner    = dmdma_slot_owner_none;
    slot->cb.dmdrvi_handler = NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, dmdrvi_ssize_t, _read,
    ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    /* Not a byte-stream device - a stream is driven entirely through ioctl()
     * (see dmdma_ioctl_cmd_t in dmdma_types.h). */
    (void)context; (void)handle; (void)buffer;

    if (offset < 0)
    {
        return -EINVAL;
    }
    if (size > (size_t)INT64_MAX)
    {
        return -EOVERFLOW;
    }

    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, dmdrvi_ssize_t, _write,
    ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    (void)context; (void)handle; (void)buffer;

    if (offset < 0)
    {
        return -EINVAL;
    }
    if (size > (size_t)INT64_MAX)
    {
        return -EOVERFLOW;
    }

    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, int, _ioctl,
    ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    dmdma_stream_slot_t *slot = slot_from_handle(context, handle);
    if (slot == NULL)
    {
        DMOD_LOG_ERROR("Invalid handle in dmdma_dmdrvi_ioctl\n");
        return -EINVAL;
    }

    if (command < 0 || command >= dmdma_ioctl_cmd_max)
    {
        DMOD_LOG_ERROR("Invalid ioctl command %d\n", command);
        return -EINVAL;
    }

    dmdma_controller_t controller = context->config.controller;

    switch ((dmdma_ioctl_cmd_t)command)
    {
        case dmdma_ioctl_cmd_start_transfer:
        {
            const dmdma_transfer_config_t *cfg = (const dmdma_transfer_config_t *)arg;
            int rc = validate_transfer_config(controller, cfg);
            if (rc != 0)
            {
                DMOD_LOG_ERROR("dmdma: rejecting transfer config on controller %u (rc=%d)\n", (unsigned)controller, rc);
                return rc;
            }

            slot->circular = cfg->circular;
            rc = dmdma_port_stream_start(controller, slot->stream, cfg);
            if (rc == 0)
            {
                rc = arm_timeout(slot, cfg->timeout_ms);
            }
            return rc;
        }

        case dmdma_ioctl_cmd_stop_transfer:
        {
            /* Same was_busy-before-stop reasoning as dmdma_lease_abort() -
             * see the comment there. */
            bool was_busy = dmdma_port_stream_is_busy(controller, slot->stream);
            disarm_timeout(slot);
            dmdma_port_stream_stop(controller, slot->stream);
            if (was_busy)
            {
                deliver_event(context, slot, dmdma_event_aborted);
            }
            return 0;
        }

        case dmdma_ioctl_cmd_is_busy:
            if (arg == NULL) return -EINVAL;
            *(bool *)arg = dmdma_port_stream_is_busy(controller, slot->stream);
            return 0;

        case dmdma_ioctl_cmd_get_remaining:
            if (arg == NULL) return -EINVAL;
            *(size_t *)arg = dmdma_port_stream_get_remaining(controller, slot->stream);
            return 0;

        case dmdma_ioctl_cmd_set_interrupt_handler:
            slot->cb.dmdrvi_handler = (arg != NULL) ? *(dmdma_interrupt_handler_t *)arg : NULL;
            return 0;

        default:
            return -EINVAL;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    dmdma_stream_slot_t *slot = slot_from_handle(context, handle);
    if (slot == NULL)
    {
        return -EINVAL;
    }

    /* "Flush" for a DMA stream means: block until whatever transfer is in
     * flight finishes, mirroring dmuart_dmdrvi_flush() waiting for TC. */
    uint32_t timeout = DMDMA_FLUSH_TIMEOUT_ITERATIONS;
    while (dmdma_port_stream_is_busy(context->config.controller, slot->stream) && --timeout > 0)
    {
    }

    return (timeout > 0) ? 0 : -ETIMEDOUT;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, int, _stat,
    ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    (void)path;

    if (!is_valid_context(context) || stat == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters in dmdma_dmdrvi_stat\n");
        return -EINVAL;
    }

    stat->size = 0;    /* Not a byte-addressable device */
    stat->mode = 0666;
    return 0;
}

/* ---- DMA lease API (see dmdma_lease.h) ----
 *
 * A plain Module API (dmod is the one and only implementation of it), so
 * these are ordinary dmod_dmdma_api_declaration() definitions - registered
 * for direct calling by any other module the moment it includes
 * dmdma_lease.h and links against dmdma, no DIF-style runtime lookup
 * involved. */

dmod_dmdma_api_declaration(1.0, dmdma_lease_t, _lease_acquire,
    ( dmdma_controller_t controller, dmdma_stream_t stream ))
{
    if (controller >= DMDMA_MAX_CONTROLLERS)
    {
        return NULL;
    }

    dmdrvi_context_t context = g_contexts[controller];
    if (!is_valid_context(context) || stream >= context->stream_count)
    {
        DMOD_LOG_ERROR("dmdma: invalid lease stream %u/%u\n", (unsigned)controller, (unsigned)stream);
        return NULL;
    }

    dmdma_stream_slot_t *slot = &context->streams[stream];
    if (slot->reserved)
    {
        DMOD_LOG_ERROR("dmdma: stream %u/%u already reserved\n", (unsigned)controller, (unsigned)stream);
        return NULL;
    }

    if (dmdma_port_stream_acquire(controller, slot->stream) != 0)
    {
        DMOD_LOG_ERROR("dmdma: failed to acquire stream %u/%u for lease\n", (unsigned)controller, (unsigned)stream);
        return NULL;
    }

    slot->reserved = true;
    slot->owner    = dmdma_slot_owner_lease;
    slot->cb.lease.callback = NULL;
    slot->cb.lease.user_ptr = NULL;
    return (dmdma_lease_t)slot;
}

dmod_dmdma_api_declaration(1.0, void, _lease_release, ( dmdma_lease_t lease ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return;
    }

    dmdrvi_context_t context = context_from_slot(slot);
    dmdma_controller_t controller = context->config.controller;

    /* Stop the hardware, then disable/reset it via the port, *then* clear
     * the software reservation - the same ordering _close() uses, so a
     * racing ISR can never observe reserved==true with the stream already
     * disabled, or vice versa. Disarming (not destroying, see
     * dmdma_stream_slot_t.timer) the watchdog first means a timeout that
     * was already in flight can never fire into this slot after release. */
    disarm_timeout(slot);
    dmdma_port_stream_stop(controller, slot->stream);
    dmdma_port_stream_release(controller, slot->stream);

    slot->reserved = false;
    slot->owner    = dmdma_slot_owner_none;
    slot->cb.lease.callback = NULL;
    slot->cb.lease.user_ptr = NULL;
}

dmod_dmdma_api_declaration(1.0, int, _lease_start,
    ( dmdma_lease_t lease, const dmdma_transfer_config_t *config ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return -EINVAL;
    }

    dmdrvi_context_t context = context_from_slot(slot);
    dmdma_controller_t controller = context->config.controller;

    int rc = validate_transfer_config(controller, config);
    if (rc != 0)
    {
        DMOD_LOG_ERROR("dmdma: rejecting lease transfer config on controller %u (rc=%d)\n", (unsigned)controller, rc);
        return rc;
    }

    slot->circular = config->circular;
    rc = dmdma_port_stream_start(controller, slot->stream, config);
    if (rc == 0)
    {
        rc = arm_timeout(slot, config->timeout_ms);
    }
    return rc;
}

dmod_dmdma_api_declaration(1.0, void, _lease_abort, ( dmdma_lease_t lease ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return;
    }

    dmdrvi_context_t context = context_from_slot(slot);
    dmdma_controller_t controller = context->config.controller;

    /* Read busy state *before* stopping - stopping an already-idle stream is
     * a harmless no-op, but firing "aborted" for it would misreport a
     * transfer that had already finished (and already got its own
     * dmdma_event_complete) as cancelled instead. */
    bool was_busy = dmdma_port_stream_is_busy(controller, slot->stream);
    disarm_timeout(slot);
    dmdma_port_stream_stop(controller, slot->stream);

    /* Fired synchronously (there is no hardware event for a software abort) -
     * see dmdma_event_aborted in dmdma_types.h. */
    if (was_busy)
    {
        deliver_event(context, slot, dmdma_event_aborted);
    }
}

dmod_dmdma_api_declaration(1.0, bool, _lease_is_busy, ( dmdma_lease_t lease ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return false;
    }

    dmdrvi_context_t context = context_from_slot(slot);
    return dmdma_port_stream_is_busy(context->config.controller, slot->stream);
}

dmod_dmdma_api_declaration(1.0, size_t, _lease_get_remaining, ( dmdma_lease_t lease ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return 0;
    }

    dmdrvi_context_t context = context_from_slot(slot);
    return dmdma_port_stream_get_remaining(context->config.controller, slot->stream);
}

dmod_dmdma_api_declaration(1.0, int, _lease_set_callback,
    ( dmdma_lease_t lease, dmdma_lease_callback_t callback, void *user_ptr ))
{
    dmdma_stream_slot_t *slot = slot_from_lease(lease);
    if (slot == NULL)
    {
        return -EINVAL;
    }

    slot->cb.lease.callback = callback;
    slot->cb.lease.user_ptr = user_ptr;
    return 0;
}
