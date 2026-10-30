/* LinuxKPI's SET_SYSTEM_SLEEP_PM_OPS is gated on CONFIG_PM_SLEEP, which this
 * build does not define (there is no device PM under XNU), so the macros
 * collapse and pm_sleep_ptr() would turn every hook into int 0. Ath12k's
 * pci.c then fails with "initializer element is not a compile-time constant".
 * Keep the hooks as written and let the struct carry them; nothing calls PM.
 */
#ifndef _LINUXKPI_LINUX_PM_EXT_H_
#define _LINUXKPI_LINUX_PM_EXT_H_

#include_next <linux/pm.h>

#undef pm_sleep_ptr
#define pm_sleep_ptr(_p) (_p)

#ifndef SET_SYSTEM_SLEEP_PM_OPS
#define SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) .suspend = suspend_fn, .resume = resume_fn,
#endif

#ifndef SET_LATE_SYSTEM_SLEEP_PM_OPS
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) .suspend_late = suspend_fn, .resume_early = resume_fn,
#endif

#endif /* _LINUXKPI_LINUX_PM_EXT_H_ */
