// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung Exynos 9820 debug-snapshot log_kernel bridge for mainline Linux.
 *
 * Appends the mainline kernel printk console into the physical DRAM ring buffer
 * reserved by S-Boot (ess_setup=0xfd900000, log_kernel=0xfd910000, 2 MiB) so
 * downstream recovery kernels (e.g. OrangeFox) automatically expose the last
 * ~20 consecutive boots at /proc/last_kmsg across warm resets and panics.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#define DSS_HEADER_PADDR	0xfd900000UL
#define DSS_HEADER_SIZE		0x00010000UL
#define DSS_OFFSET_LAST_LOGBUF	0x200UL

#define DSS_LOG_KERNEL_PADDR	0xfd910000UL
#define DSS_LOG_KERNEL_SIZE	0x00200000UL
#define DSS_LOG_USABLE_SIZE	(DSS_LOG_KERNEL_SIZE - 8UL)

#define SEC_LKMSG_MAGICKEY	0x0000000a6c6c7546ULL

static void *dss_header_vaddr;
static char *dss_log_vaddr;
static size_t dss_log_off;
static DEFINE_RAW_SPINLOCK(dss_log_lock);

static void exynos_dss_append_locked(const char *s, size_t count)
{
	u32 paddr_cursor;

	if (count > DSS_LOG_USABLE_SIZE) {
		s += count - DSS_LOG_USABLE_SIZE;
		count = DSS_LOG_USABLE_SIZE;
	}

	if (dss_log_off + count > DSS_LOG_USABLE_SIZE) {
		*(u64 *)(dss_log_vaddr + DSS_LOG_USABLE_SIZE) = SEC_LKMSG_MAGICKEY;
		dss_log_off = 0;
	}

	memcpy(dss_log_vaddr + dss_log_off, s, count);
	dss_log_off += count;

	/* Ensure log bytes are visible in DRAM before updating cursor. */
	wmb();
	paddr_cursor = (u32)(DSS_LOG_KERNEL_PADDR + dss_log_off);
	*(u32 *)((char *)dss_header_vaddr + DSS_OFFSET_LAST_LOGBUF) = paddr_cursor;
	wmb();
}

static void exynos_dss_console_write(struct console *con, const char *s,
				     unsigned int count)
{
	unsigned long flags;

	if (!dss_log_vaddr || !dss_header_vaddr || !count)
		return;

	raw_spin_lock_irqsave(&dss_log_lock, flags);
	exynos_dss_append_locked(s, count);
	raw_spin_unlock_irqrestore(&dss_log_lock, flags);
}

static struct console exynos_dss_console = {
	.name	= "exynos_dss",
	.write	= exynos_dss_console_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

static int __init exynos_dss_last_kmsg_init(void)
{
	static const char banner[] = "\n===== MAINLINE 7.2 BOOT =====\n";
	unsigned long flags;
	u32 prev_cursor;

	dss_header_vaddr = memremap(DSS_HEADER_PADDR, DSS_HEADER_SIZE,
				    MEMREMAP_WC);
	if (!dss_header_vaddr) {
		pr_err("exynos_dss_last_kmsg: failed to map header 0x%lx\n",
		       DSS_HEADER_PADDR);
		return -ENOMEM;
	}

	dss_log_vaddr = memremap(DSS_LOG_KERNEL_PADDR, DSS_LOG_KERNEL_SIZE,
				 MEMREMAP_WC);
	if (!dss_log_vaddr) {
		pr_err("exynos_dss_last_kmsg: failed to map log_kernel 0x%lx\n",
		       DSS_LOG_KERNEL_PADDR);
		memunmap(dss_header_vaddr);
		dss_header_vaddr = NULL;
		return -ENOMEM;
	}

	/* Resume appending from S-Boot/previous-boot cursor if valid. */
	prev_cursor = *(u32 *)((char *)dss_header_vaddr + DSS_OFFSET_LAST_LOGBUF);
	if (prev_cursor >= DSS_LOG_KERNEL_PADDR &&
	    prev_cursor < DSS_LOG_KERNEL_PADDR + DSS_LOG_USABLE_SIZE) {
		dss_log_off = prev_cursor - DSS_LOG_KERNEL_PADDR;
	} else {
		memset(dss_log_vaddr, 0, DSS_LOG_KERNEL_SIZE);
		dss_log_off = 0;
		wmb();
		*(u32 *)((char *)dss_header_vaddr + DSS_OFFSET_LAST_LOGBUF) =
			(u32)DSS_LOG_KERNEL_PADDR;
		wmb();
	}

	raw_spin_lock_irqsave(&dss_log_lock, flags);
	exynos_dss_append_locked(banner, sizeof(banner) - 1);
	raw_spin_unlock_irqrestore(&dss_log_lock, flags);

	register_console(&exynos_dss_console);
	pr_info("exynos_dss_last_kmsg: active append-mode (log_kernel=0x%lx off=0x%zx cursor=0x%lx)\n",
		DSS_LOG_KERNEL_PADDR, dss_log_off,
		DSS_HEADER_PADDR + DSS_OFFSET_LAST_LOGBUF);
	return 0;
}
core_initcall(exynos_dss_last_kmsg_init);
