#include <linux/compiler.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/thread_info.h>
#include <linux/seccomp.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/uidgid.h>

#include "ksu.h"
#include "policy/allowlist.h"
#include "hook/setuid_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"
#include "infra/seccomp_cache.h"
#include "runtime/ksud_boot.h"
#include "selinux/selinux.h"
#include "supercall/supercall.h"
#include "hook/tp_marker.h"
#include "feature/kernel_umount.h"

int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid)
{
    // we rely on the fact that zygote always call setresuid(3) with same uids

    pr_debug("handle_setresuid from %d to %d\n", old_uid, new_uid);

    if (unlikely(old_uid == 0 && new_uid >= 10000 && !ksu_is_manager_appid_valid())) {
        static DEFINE_MUTEX(late_init_lock);
        static bool late_init_done = false;
        mutex_lock(&late_init_lock);
        if (!late_init_done && !ksu_is_manager_appid_valid()) {
            pr_info("ksu setresuid: lazy post-fs-data init\n");
            apply_kernelsu_rules();
            cache_sid();
            setup_ksu_cred();
            on_post_fs_data();
            late_init_done = true;
        }
        mutex_unlock(&late_init_lock);
    }

    if (unlikely(is_uid_manager(new_uid))) {
        spin_lock_irq(&current->sighand->siglock);
        ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
        ksu_set_task_tracepoint_flag(current);
        spin_unlock_irq(&current->sighand->siglock);

        pr_info("install fd for manager: %d\n", new_uid);
        ksu_install_fd();
        return 0;
    }

    if (ksu_is_allow_uid_for_current(new_uid)) {
        if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
            current->seccomp.filter) {
            spin_lock_irq(&current->sighand->siglock);
            ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
            spin_unlock_irq(&current->sighand->siglock);
        }
        ksu_set_task_tracepoint_flag(current);
    } else {
        ksu_clear_task_tracepoint_flag_if_needed(current);
    }

    // Handle kernel umount
    ksu_handle_umount(old_uid, new_uid);

    return 0;
}

void __init ksu_setuid_hook_init(void)
{
    ksu_kernel_umount_init();
}

void __exit ksu_setuid_hook_exit(void)
{
    pr_info("ksu_core_exit\n");
    ksu_kernel_umount_exit();
}
