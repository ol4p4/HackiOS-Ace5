/* LinuxKPI's rcupdate.h predates the lockdep assertions ath12k uses. There
 * is no lockdep under XNU, so the warning expands to nothing, as it does in
 * a Linux build without CONFIG_LOCKDEP.
 */
#ifndef _LINUXKPI_LINUX_RCUPDATE_EXT_H_
#define _LINUXKPI_LINUX_RCUPDATE_EXT_H_

#include_next <linux/rcupdate.h>

#ifndef RCU_LOCKDEP_WARN
#define RCU_LOCKDEP_WARN(c, s) do { } while (0)
#endif

#endif /* _LINUXKPI_LINUX_RCUPDATE_EXT_H_ */
