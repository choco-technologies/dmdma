#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmdma.h"
#include "dmdma_port.h"
#include "dmdrvi.h"
#include "dmini.h"

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
 * @brief One DMA stream within a controller
 *
 * This is also the opaque handle dmdrvi_open()/_close() hand back and forth -
 * a stream is reserved for the lifetime of the open() that returned it.
 */
typedef struct
{
    dmdma_stream_t              stream;    /**< Stream index within the controller */
    bool                        reserved;  /**< Currently open (== reserved on the port) */
    dmdma_interrupt_handler_t   handler;   /**< Completion/error callback, NULL = none registered */
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
 * is currently open, before it gets dereferenced. */
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

    return slot->reserved ? slot : NULL;
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
    if (slot->reserved && slot->handler != NULL)
    {
        slot->handler(context, slot, event);
    }
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
        context->streams[i].handler = NULL;
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
    slot->handler  = NULL;
    return slot;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmdma, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    dmdma_stream_slot_t *slot = slot_from_handle(context, handle);
    if (slot == NULL)
    {
        return;
    }

    dmdma_port_stream_stop(context->config.controller, slot->stream);
    dmdma_port_stream_release(context->config.controller, slot->stream);
    slot->reserved = false;
    slot->handler  = NULL;
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
            if (arg == NULL)
            {
                return -EINVAL;
            }

            const dmdma_transfer_config_t *cfg = (const dmdma_transfer_config_t *)arg;
            if (cfg->direction == dmdma_direction_memory_to_memory &&
                !dmdma_port_supports_memory_to_memory(controller))
            {
                DMOD_LOG_ERROR("dmdma: controller %u cannot do memory-to-memory transfers\n", (unsigned)controller);
                return -ENOTSUP;
            }

            return dmdma_port_stream_start(controller, slot->stream, cfg);
        }

        case dmdma_ioctl_cmd_stop_transfer:
            dmdma_port_stream_stop(controller, slot->stream);
            return 0;

        case dmdma_ioctl_cmd_is_busy:
            if (arg == NULL) return -EINVAL;
            *(bool *)arg = dmdma_port_stream_is_busy(controller, slot->stream);
            return 0;

        case dmdma_ioctl_cmd_get_remaining:
            if (arg == NULL) return -EINVAL;
            *(size_t *)arg = dmdma_port_stream_get_remaining(controller, slot->stream);
            return 0;

        case dmdma_ioctl_cmd_set_interrupt_handler:
            slot->handler = (arg != NULL) ? *(dmdma_interrupt_handler_t *)arg : NULL;
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
