/* Linux renamed from_timer() to timer_container_of() in 6.16, and LinuxKPI
 * follows LINUXKPI_VERSION. ath12k 6.12 still calls from_timer(); both spell
 * the same container_of.
 */
#ifndef _LINUXKPI_LINUX_TIMER_EXT_H_
#define _LINUXKPI_LINUX_TIMER_EXT_H_

#include_next <linux/timer.h>

#ifndef from_timer
#define from_timer(var, arg, field) container_of(arg, typeof(*(var)), field)
#endif

#endif /* _LINUXKPI_LINUX_TIMER_EXT_H_ */
