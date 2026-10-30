/* LinuxKPI's net/cfg80211.h has cfg80211_find_elem for a plain element id,
 * but not the extended-element variant (id 255 with the extension id in the
 * first payload byte) that ath12k's mac.c uses for non-inheritance.
 */
#ifndef _LINUXKPI_NET_CFG80211_EXT_H_
#define _LINUXKPI_NET_CFG80211_EXT_H_

#include_next <net/cfg80211.h>

#ifndef WLAN_EID_EXTENSION
#define WLAN_EID_EXTENSION 221
#endif

static inline const struct element *
cfg80211_find_ext_elem(u8 ext_id, const u8 *data, size_t len)
{
	const struct element *elem = (const void *)data;
	const void *end = data + len;

	while ((const void *)(elem + 1) <= end &&
	    (const u8 *)elem + 2 + elem->datalen <= end) {
		if (elem->id == WLAN_EID_EXTENSION && elem->datalen >= 1 &&
		    elem->data[0] == ext_id)
			return elem;
		elem = (const void *)(elem->data + elem->datalen);
	}
	return (NULL);
}

#endif /* _LINUXKPI_NET_CFG80211_EXT_H_ */
