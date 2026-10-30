/* The atomic notifier chain LinuxKPI does not have (see
 * include/linux/panic_notifier.h). ath12k only registers a panic handler, so
 * accepting the registration is enough for the driver to come up; nothing
 * calls the handler under XNU yet.
 */
#include <linux/panic_notifier.h>

struct atomic_notifier_head panic_notifier_list;

int
atomic_notifier_chain_register(struct atomic_notifier_head *nh,
    struct notifier_block *nb)
{
	(void)nh;
	(void)nb;
	return (0);
}

int
atomic_notifier_chain_unregister(struct atomic_notifier_head *nh,
    struct notifier_block *nb)
{
	(void)nh;
	(void)nb;
	return (0);
}
