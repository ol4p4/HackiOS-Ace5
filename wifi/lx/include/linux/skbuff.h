/* COMPILE-ONLY: LinuxKPI's skbuff.h has neither skb_cow_data nor pskb_put.
 * ath12k 6.12's dp_tx uses them to make a metadata tail writable. The stub
 * below fails the cow (so dp_tx takes its -ENOMEM path) rather than silently
 * corrupting a frame; real behaviour belongs with the Wi-Fi data plane.
 */
#ifndef _LINUXKPI_LINUX_SKBUFF_EXT_H_
#define _LINUXKPI_LINUX_SKBUFF_EXT_H_

#include_next <linux/skbuff.h>

struct sk_buff;

static inline int
skb_cow_data(struct sk_buff *skb, int tailbits, struct sk_buff **trailer)
{
	(void)skb;
	(void)tailbits;
	*trailer = NULL;
	return (-ENOMEM);
}

static inline void *
pskb_put(struct sk_buff *skb, struct sk_buff *tail, int len)
{
	(void)skb;
	(void)tail;
	(void)len;
	return (NULL);
}

#endif /* _LINUXKPI_LINUX_SKBUFF_EXT_H_ */
