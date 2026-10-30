/* LinuxKPI's list.h has the splice helpers except list_cut_before, which
 * ath12k 6.12's dp_rx uses to split a queue at a list entry. This is the
 * kernel's own implementation, unchanged.
 */
#ifndef _LINUXKPI_LINUX_LIST_EXT_H_
#define _LINUXKPI_LINUX_LIST_EXT_H_

#include_next <linux/list.h>

static inline void
list_cut_before(struct list_head *list1, struct list_head *head,
    struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list1);
		return;
	}
	list1->next = head->next;
	list1->next->prev = list1;
	list1->prev = entry->prev;
	list1->prev->next = list1;
	head->next = entry;
	entry->prev = head;
}

#endif /* _LINUXKPI_LINUX_LIST_EXT_H_ */
