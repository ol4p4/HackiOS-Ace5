/* LinuxKPI has SET_SYSTEM_SLEEP_PM_OPS but not the late/early variant ath12k
 * 6.12 uses. There is no device PM under XNU, so the late hooks are dropped.
 */
#ifndef _LINUXKPI_LINUX_PM_EXT_H_
#define _LINUXKPI_LINUX_PM_EXT_H_

#include_next <linux/pm.h>

#ifndef SET_LATE_SYSTEM_SLEEP_PM_OPS
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#endif

#endif /* _LINUXKPI_LINUX_PM_EXT_H_ */
