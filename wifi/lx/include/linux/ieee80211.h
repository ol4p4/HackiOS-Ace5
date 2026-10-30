/* LinuxKPI's copy of Linux's 802.11 constants is older than ath12k: two names
 * the driver uses are absent. They are protocol constants, not behaviour.
 */
#ifndef _LINUXKPI_LINUX_IEEE80211_EXT_H_
#define _LINUXKPI_LINUX_IEEE80211_EXT_H_

#include_next <linux/ieee80211.h>

#ifndef WLAN_EID_NON_TX_BSSID_CAP
#define WLAN_EID_NON_TX_BSSID_CAP 83
#endif

#ifndef IEEE80211_EHT_PHY_CAP4_PSR_SR_SUPP
#define IEEE80211_EHT_PHY_CAP4_PSR_SR_SUPP 0x02
#endif

#endif /* _LINUXKPI_LINUX_IEEE80211_EXT_H_ */
