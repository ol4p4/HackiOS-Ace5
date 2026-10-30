/*
 * Included before every file of the driver. FreeBSD's 802.11 headers define the access categories
 * (WME_AC_BE ...) as macros and ath11k's core.h declares them as an enumeration with the same values:
 * the headers are read first, here, and the macros dropped, so the driver's own declaration stands.
 */
#include <net/mac80211.h>
#undef WME_AC_BE
#undef WME_AC_BK
#undef WME_AC_VI
#undef WME_AC_VO
#undef WME_NUM_AC
/* Linux's own headers pull these in transitively; LinuxKPI's set does not, and
 * ath12k uses mdelay() and from_timer() without including them. */
#include <linux/delay.h>
#include <linux/timer.h>
#include <lx_more.h>
