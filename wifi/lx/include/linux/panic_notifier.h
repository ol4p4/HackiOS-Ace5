/* LinuxKPI has no atomic notifier chains. ath12k registers one panic handler
 * (core.c: ath12k_core_panic_notifier_register) to dump its state on panic;
 * this is the smallest surface that lets it compile. The registration is
 * accepted and never called: XNU has no panic notifier to attach to yet.
 */
#ifndef _LINUXKPI_LINUX_PANIC_NOTIFIER_H_
#define _LINUXKPI_LINUX_PANIC_NOTIFIER_H_

#include <linux/notifier.h>

struct atomic_notifier_head {
	struct notifier_block *head;
};

extern struct atomic_notifier_head panic_notifier_list;

int atomic_notifier_chain_register(struct atomic_notifier_head *nh,
    struct notifier_block *nb);
int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
    struct notifier_block *nb);

#endif /* _LINUXKPI_LINUX_PANIC_NOTIFIER_H_ */
