/* LinuxKPI has no device-graph helpers. ath12k uses them only to walk a
 * multi-device WSI topology (ath12k_core_get_wsi_info); the WCN7850 is a
 * single device with no of_node, so the walk stops at its first check and
 * these are never reached with a real node.
 */
#ifndef _LINUXKPI_LINUX_OF_GRAPH_H_
#define _LINUXKPI_LINUX_OF_GRAPH_H_

#include <linux/of.h>

static inline struct device_node *
of_graph_get_endpoint_by_regs(const struct device_node *np, int port, int reg)
{
	(void)np;
	(void)port;
	(void)reg;
	return (NULL);
}

static inline struct device_node *
of_graph_get_remote_endpoint(const struct device_node *ep)
{
	(void)ep;
	return (NULL);
}

static inline struct device_node *
of_graph_get_port_parent(const struct device_node *ep)
{
	(void)ep;
	return (NULL);
}

#endif /* _LINUXKPI_LINUX_OF_GRAPH_H_ */
