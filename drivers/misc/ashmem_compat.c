// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal /dev/ashmem compatibility driver for Android userspace on Linux 7.2+
 * Backed by shmem_file_setup() so legacy Android API<=28 vendor images work
 * without requiring sys.use_memfd.
 */

#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define ASHMEM_NAME_LEN		256
#define ASHMEM_NAME_PREFIX	"dev/ashmem/"
#define ASHMEM_NAME_PREFIX_LEN	(sizeof(ASHMEM_NAME_PREFIX) - 1)
#define ASHMEM_FULL_NAME_LEN	(ASHMEM_NAME_LEN + ASHMEM_NAME_PREFIX_LEN)

#define __ASHMEMIOC		0x77
#define ASHMEM_NOT_PURGED	0
#define ASHMEM_IS_PINNED	1

struct ashmem_area {
	char name[ASHMEM_FULL_NAME_LEN];
	struct file *file;
	size_t size;
	unsigned long prot_mask;
	struct mutex lock;
};

static int ashmem_open(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma;

	asma = kzalloc(sizeof(*asma), GFP_KERNEL);
	if (!asma)
		return -ENOMEM;

	memcpy(asma->name, ASHMEM_NAME_PREFIX, ASHMEM_NAME_PREFIX_LEN);
	asma->prot_mask = PROT_READ | PROT_WRITE | PROT_EXEC;
	mutex_init(&asma->lock);
	file->private_data = asma;
	return 0;
}

static int ashmem_release(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma = file->private_data;

	if (asma) {
		if (asma->file)
			fput(asma->file);
		kfree(asma);
	}
	return 0;
}

static int ashmem_ensure_file(struct ashmem_area *asma)
{
	struct file *vmfile;
	const char *name;

	if (asma->file)
		return 0;
	if (!asma->size)
		return -EINVAL;

	name = asma->name[ASHMEM_NAME_PREFIX_LEN] ? asma->name : "dev/ashmem";
	vmfile = shmem_file_setup(name, asma->size, mk_vma_flags(VMA_NORESERVE_BIT));
	if (IS_ERR(vmfile))
		return PTR_ERR(vmfile);

	vmfile->f_mode |= FMODE_LSEEK;
	asma->file = vmfile;
	return 0;
}

static int ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ashmem_area *asma = file->private_data;
	int ret;

	mutex_lock(&asma->lock);
	ret = ashmem_ensure_file(asma);
	if (ret) {
		mutex_unlock(&asma->lock);
		return ret;
	}

	if ((vma->vm_flags & VM_WRITE) && !(asma->prot_mask & PROT_WRITE)) {
		mutex_unlock(&asma->lock);
		return -EPERM;
	}
	if (!(asma->prot_mask & PROT_WRITE))
		vm_flags_clear(vma, VM_MAYWRITE);
	if (!(asma->prot_mask & PROT_EXEC))
		vm_flags_clear(vma, VM_MAYEXEC);

	vma_set_file(vma, asma->file);
	{
		struct vm_area_desc desc = {
			.file = asma->file,
			.vma_flags = vma->flags,
			.page_prot = vma->vm_page_prot,
		};

		ret = asma->file->f_op->mmap_prepare(&desc);
		if (!ret)
			vma->vm_ops = desc.vm_ops;
	}
	mutex_unlock(&asma->lock);

	return ret;
}

static ssize_t ashmem_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct ashmem_area *asma = iocb->ki_filp->private_data;
	ssize_t ret;

	mutex_lock(&asma->lock);
	if (!asma->file) {
		mutex_unlock(&asma->lock);
		return 0;
	}
	get_file(asma->file);
	mutex_unlock(&asma->lock);

	ret = vfs_iter_read(asma->file, iter, &iocb->ki_pos, 0);
	fput(asma->file);
	return ret;
}

static loff_t ashmem_llseek(struct file *file, loff_t offset, int origin)
{
	struct ashmem_area *asma = file->private_data;
	loff_t ret;

	mutex_lock(&asma->lock);
	if (!asma->file) {
		mutex_unlock(&asma->lock);
		return -EINVAL;
	}
	ret = vfs_llseek(asma->file, offset, origin);
	file->f_pos = asma->file->f_pos;
	mutex_unlock(&asma->lock);
	return ret;
}

static long ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ashmem_area *asma = file->private_data;
	long ret = -ENOTTY;

	if (_IOC_TYPE(cmd) != __ASHMEMIOC)
		return -ENOTTY;

	switch (_IOC_NR(cmd)) {
	case 1: { /* ASHMEM_SET_NAME */
		char local_name[ASHMEM_NAME_LEN];

		if (copy_from_user(local_name, (void __user *)arg, ASHMEM_NAME_LEN))
			return -EFAULT;
		local_name[ASHMEM_NAME_LEN - 1] = '\0';
		mutex_lock(&asma->lock);
		if (asma->file) {
			ret = -EINVAL;
		} else {
			strscpy(asma->name + ASHMEM_NAME_PREFIX_LEN, local_name,
				ASHMEM_NAME_LEN);
			ret = 0;
		}
		mutex_unlock(&asma->lock);
		return ret;
	}
	case 2: { /* ASHMEM_GET_NAME */
		char local_name[ASHMEM_NAME_LEN] = { 0 };

		mutex_lock(&asma->lock);
		strscpy(local_name, asma->name + ASHMEM_NAME_PREFIX_LEN,
			ASHMEM_NAME_LEN);
		mutex_unlock(&asma->lock);
		if (copy_to_user((void __user *)arg, local_name, ASHMEM_NAME_LEN))
			return -EFAULT;
		return 0;
	}
	case 3: /* ASHMEM_SET_SIZE */
		mutex_lock(&asma->lock);
		if (asma->file) {
			ret = -EINVAL;
		} else {
			asma->size = arg;
			ret = 0;
		}
		mutex_unlock(&asma->lock);
		return ret;
	case 4: /* ASHMEM_GET_SIZE */
		return asma->size;
	case 5: /* ASHMEM_SET_PROT_MASK */
		mutex_lock(&asma->lock);
		if ((asma->prot_mask & arg) != arg) {
			ret = -EINVAL;
		} else {
			asma->prot_mask = arg;
			ret = 0;
		}
		mutex_unlock(&asma->lock);
		return ret;
	case 6: /* ASHMEM_GET_PROT_MASK */
		return asma->prot_mask;
	case 7: /* ASHMEM_PIN */
		return ASHMEM_NOT_PURGED;
	case 8: /* ASHMEM_UNPIN */
		return 0;
	case 9: /* ASHMEM_GET_PIN_STATUS */
		return ASHMEM_IS_PINNED;
	case 10: /* ASHMEM_PURGE_ALL_CACHES */
		return 0;
	case 11: { /* ASHMEM_GET_FILE_ID */
		unsigned long ino;

		mutex_lock(&asma->lock);
		ino = asma->file ? file_inode(asma->file)->i_ino : file_inode(file)->i_ino;
		mutex_unlock(&asma->lock);
		if (copy_to_user((void __user *)arg, &ino, sizeof(ino)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ashmem_fops = {
	.owner		= THIS_MODULE,
	.open		= ashmem_open,
	.release	= ashmem_release,
	.read_iter	= ashmem_read_iter,
	.llseek		= ashmem_llseek,
	.mmap		= ashmem_mmap,
	.unlocked_ioctl	= ashmem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ashmem_ioctl,
#endif
};

static struct miscdevice ashmem_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ashmem",
	.fops	= &ashmem_fops,
	.mode	= 0666,
};

static int __init ashmem_compat_init(void)
{
	int ret = misc_register(&ashmem_misc);

	if (ret)
		pr_err("ashmem_compat: failed to register /dev/ashmem (%d)\n", ret);
	else
		pr_info("ashmem_compat: registered /dev/ashmem\n");
	return ret;
}
device_initcall(ashmem_compat_init);
