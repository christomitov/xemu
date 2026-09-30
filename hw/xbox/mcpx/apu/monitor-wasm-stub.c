/*
 * Wasm stubs for the MCPX APU debug monitor.
 *
 * The native monitor plays the current frame through an SDL audio stream for
 * audible debugging. There is no SDL in the browser build, so every hook is a
 * no-op and the throttler runs unthrottled (queued_bytes stays -1).
 */
#include "apu_int.h"

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 0;
    d->monitor.queued_bytes_high = 0;
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
}
