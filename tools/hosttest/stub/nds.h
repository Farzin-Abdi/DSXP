/* Host stub for <nds.h>.
 *
 * The tests compile the real source files, which pull this in for the
 * tick counter. The clock advances by a simulated 5ms on every read,
 * which is roughly what one MPEG frame decode costs on the real ARM9 -
 * that is what makes the decoder's time budget observable off target
 * instead of a no-op.
 */
#ifndef DSXP_HOST_NDS_STUB_H
#define DSXP_HOST_NDS_STUB_H

#include <stdint.h>

#ifndef TICK_FREQ
#define TICK_FREQ 523656u
#endif

#ifndef DSXP_HOST_OWN_TICKS
#define DSXP_HOST_TICK_STEP ((uint64_t)TICK_FREQ * 5u / 1000u)   /* 5ms */

static uint64_t g_hostTicks = 0;
static inline uint64_t tickGetCount(void)
{
    uint64_t now = g_hostTicks;
    g_hostTicks += DSXP_HOST_TICK_STEP;
    return now;
}
#endif

/* The host has no interrupts to mask; the ring's critical section is
 * still exercised for its logic, just not for its exclusion. */
static inline int  enterCriticalSection(void) { return 0; }
static inline void leaveCriticalSection(int o) { (void)o; }

#endif
