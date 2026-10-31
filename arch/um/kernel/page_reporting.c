// SPDX-License-Identifier: GPL-2.0
/*
 * Free page reporting for UML. The guest's memory above the kernel image is
 * a shared mapping of a host file, so dropping the range of a free page
 * frees the host's memory behind it. A page that the guest uses again reads
 * as zeros, and the host allocates it again when the guest first touches it.
 *
 * The page allocator reports blocks of pageblock_order or larger once they
 * have been free for a while. page_reporting.page_reporting_order sets a
 * smaller order, which returns more memory at the cost of more reports.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_reporting.h>
#include <linux/printk.h>
#include <linux/scatterlist.h>
#include <os.h>

static int uml_report_free_pages(struct page_reporting_dev_info *prdev,
				 struct scatterlist *sgl, unsigned int nents)
{
	struct scatterlist *sg;
	unsigned int i;
	int err;

	for_each_sg(sgl, sg, nents, i) {
		err = os_drop_memory(page_address(sg_page(sg)), sg->length);
		if (err) {
			pr_warn_once("cannot return free memory to the host: %d\n",
				     err);
			return err;
		}
	}
	return 0;
}

static struct page_reporting_dev_info uml_page_reporting = {
	.report = uml_report_free_pages,
};

static int __init uml_page_reporting_init(void)
{
	int err;

	/*
	 * A returned page reads as zeros, which the allocator would take for
	 * corrupted poison when it hands the page out again.
	 */
	if (page_poisoning_enabled_static()) {
		pr_info("free page reporting is off, since page poisoning is on\n");
		return 0;
	}
	err = page_reporting_register(&uml_page_reporting);
	if (err)
		pr_err("cannot register free page reporting: %d\n", err);
	return 0;
}
device_initcall(uml_page_reporting_init);
