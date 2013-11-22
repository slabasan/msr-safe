/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2000-2008 H. Peter Anvin - All Rights Reserved
 *   Copyright 2009 Intel Corporation; author: H. Peter Anvin
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 *   USA; either version 2 of the License, or (at your option) any later
 *   version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * x86 MSR access device
 *
 * This device is accessed by lseek() to the appropriate register number
 * and then read/write in chunks of 8 bytes.  A larger size means multiple
 * reads or writes of the same register.
 *
 * This driver uses /dev/cpu/%d/msr_safe where %d is the minor number, and on
 * an SMP box will direct the access to CPU %d.
 */

/*Patki from Denver*/

#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>

#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/system.h>


#define _USE_ARCH_062D 1

#include "msr-supplemental.h"

static struct class *msr_class;
static int majordev;

struct msr_whitelist {
	u32	reg;
	u8	RWBit;
	u8	ReadOnStartBit;
	u8	WriteOnExitBit; 
	u64	Index;
};

#define MASK_ALL {0xFFFFFFFF, 0xFFFFFFFF}
#define MASK_NONE {0, 0}
#define MSR_LAST_ENTRY ~0
/*
 * Each MSR has an MSR Entry with its address, RWBit (1=RW, 0=RO), 
 * ReadOnStartBit(1 if you read on start), WriteOnExitBit and an Index.
 *
 *
 * Write Index = -1: Do Not Write (Read-Only; nothing needs to be reset)
 * Write Index = 0; Resets the value of the register to 0
 * Write Index = Default Value; others. 
 * */
#define MSR_ENTRY(reg, RWBit, ReadOnStartBit, WriteOnExit, Index) {reg, RWBit, ReadOnStartBit, WriteOnExitBit, Index }


static struct msr_whitelist whitelist[] = {
//	MSR_ENTRY(MSR_LAST_ENTRY, MASK_NONE, MASK_NONE),
	/*Patki*/
	MSR_ENTRY(SMSR_TIME_STAMP_COUNTER),
	MSR_ENTRY(SMSR_PLATFORM_ID),
	MSR_ENTRY(SMSR_PMC0),
	MSR_ENTRY(SMSR_PMC1),
	MSR_ENTRY(SMSR_PMC2),
	MSR_ENTRY(SMSR_PMC3),
	MSR_ENTRY(SMSR_PMC4),
	MSR_ENTRY(SMSR_PMC5),
	MSR_ENTRY(SMSR_PMC6),
	MSR_ENTRY(SMSR_PMC7),
	MSR_ENTRY(SMSR_MPERF),
	MSR_ENTRY(SMSR_APERF),
	MSR_ENTRY(SMSR_PERFEVTSEL0),
	MSR_ENTRY(SMSR_PERFEVTSEL1),
	MSR_ENTRY(SMSR_PERFEVTSEL2),
	MSR_ENTRY(SMSR_PERFEVTSEL3),
	MSR_ENTRY(SMSR_PERFEVTSEL4),
	MSR_ENTRY(SMSR_PERFEVTSEL5),
	MSR_ENTRY(SMSR_PERFEVTSEL6),
	MSR_ENTRY(SMSR_PERFEVTSEL7),
	MSR_ENTRY(SMSR_PERF_STATUS),
	MSR_ENTRY(SMSR_PERF_CTL),
	MSR_ENTRY(SMSR_CLOCK_MODULATION),
	MSR_ENTRY(SMSR_THERM_STATUS),
	MSR_ENTRY(SMSR_MISC_ENABLE),
	MSR_ENTRY(SMSR_OFFCORE_RSP_0),
	MSR_ENTRY(SMSR_OFFCORE_RSP_1),
	MSR_ENTRY(SMSR_MISC_PWR_MGMT),
	MSR_ENTRY(SMSR_ENERGY_PERF_BIAS),
	MSR_ENTRY(SMSR_PACKAGE_THERM_STATUS),
	MSR_ENTRY(SMSR_POWER_CTL),
	MSR_ENTRY(SMSR_FIXED_CTR0),
	MSR_ENTRY(SMSR_FIXED_CTR1),
	MSR_ENTRY(SMSR_FIXED_CTR2),
	MSR_ENTRY(SMSR_PERF_CAPABILITIES),
	MSR_ENTRY(SMSR_FIXED_CTR_CTRL),
	MSR_ENTRY(SMSR_PERF_GLOBAL_STATUS),
	MSR_ENTRY(SMSR_PERF_GLOBAL_CTRL),
	MSR_ENTRY(SMSR_PERF_GLOBAL_OVF_CTRL),
	MSR_ENTRY(SMSR_PEBS_ENABLE),
	MSR_ENTRY(SMSR_PEBS_LD_LAT),
	MSR_ENTRY(SMSR_RAPL_POWER_UNIT),
	MSR_ENTRY(SMSR_PKG_POWER_LIMIT),
	MSR_ENTRY(SMSR_PKG_ENERGY_STATUS),
	MSR_ENTRY(SMSR_PKG_POWER_INFO),
	MSR_ENTRY(SMSR_PP0_POWER_LIMIT),
	MSR_ENTRY(SMSR_PP0_ENERGY_STATUS),
	MSR_ENTRY(SMSR_TSC_DEADLINE),
	MSR_ENTRY(SMSR_TURBO_RATIO_LIMIT),
	MSR_ENTRY(SMSR_MSR_PEBS_NUM_ALT),
	MSR_ENTRY(SMSR_MSR_PKG_PERF_STATUS), 
	MSR_ENTRY(SMSR_DRAM_POWER_LIMIT), 
	MSR_ENTRY(SMSR_DRAM_ENERGY_STATUS),
	MSR_ENTRY(SMSR_DRAM_PERF_STATUS),
	MSR_ENTRY(SMSR_DRAM_POWER_INFO)
};

static struct msr_whitelist *get_whitelist_entry(u64 reg)
{
	struct msr_whitelist *entry;

	for (entry = whitelist; entry->reg != MSR_LAST_ENTRY; entry++)
		if (entry->reg == reg)
			return entry;

	return NULL;
}


static loff_t msr_seek(struct file *file, loff_t offset, int orig)
{
	loff_t ret;
	struct inode *inode = file->f_mapping->host;

	mutex_lock(&inode->i_mutex);
	switch (orig) {
	case 0:
		file->f_pos = offset;
		ret = file->f_pos;
		break;
	case 1:
		file->f_pos += offset;
		ret = file->f_pos;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&inode->i_mutex);
	return ret;
}

static ssize_t msr_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	u32 __user *tmp = (u32 __user *) buf;
	u32 data[2];
	u32 reg = *ppos;
	int cpu = iminor(file->f_path.dentry->d_inode);
	int err = 0;
	ssize_t bytes = 0;
	struct msr_whitelist *wlp;
	u32 read_mask[] = {0, 0};

	if (count % 8)
		return -EINVAL;	/* Invalid chunk size */

	wlp = get_whitelist_entry(reg);
	if (wlp) {
		read_mask[0] = wlp->read_mask[0];
		read_mask[1] = wlp->read_mask[1];
	}

	/*Patki*/		
	read_mask[0] = read_mask[1] = ~0;

	if (!read_mask[0] && !read_mask[1])
		return -EINVAL;

	for (; count; count -= 8) {
		err = rdmsr_safe_on_cpu(cpu, reg, &data[0], &data[1]);
		if (err)
			break;
		data[0] &= read_mask[0];
		data[1] &= read_mask[1];
		if (copy_to_user(tmp, &data, 8)) {
			err = -EFAULT;
			break;
		}
		tmp += 2;
		bytes += 8;
	}

	return bytes ? bytes : err;
}

static ssize_t msr_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	const u32 __user *tmp = (const u32 __user *)buf;
	u32 data[2];
	u32 reg = *ppos;
	int cpu = iminor(file->f_path.dentry->d_inode);
	int err = 0;
	ssize_t bytes = 0;
	struct msr_whitelist *wlp;
	u32 write_mask[] = { 0, 0};
	u32 orig_data[2];

	if (count % 8)
		return -EINVAL;	/* Invalid chunk size */

	wlp = get_whitelist_entry(reg);
	if (wlp) {
		write_mask[0] = wlp->write_mask[0];
		write_mask[1] = wlp->write_mask[1];
	}

	/*Patki*/
	write_mask[0] = write_mask[1] = ~0;

	if (!write_mask[0] && !write_mask[1])
		return -EINVAL;

	for (; count; count -= 8) {
		if (copy_from_user(&data, tmp, 8)) {
			err = -EFAULT;
			break;
		}
		if (~write_mask[0] || ~write_mask[1]) {
			err = rdmsr_safe_on_cpu(cpu, reg, &orig_data[0],
						&orig_data[1]);
			if (err)
				break;
			data[0] = (orig_data[0] & ~write_mask[0]) |
				  (data[0] & write_mask[0]);
			data[1] = (orig_data[1] & ~write_mask[1]) |
				  (data[1] & write_mask[1]);
		}
		err = wrmsr_safe_on_cpu(cpu, reg, data[0], data[1]);
		if (err)
			break;
		tmp += 2;
		bytes += 8;
	}

	return bytes ? bytes : err;
}


static int msr_open(struct inode *inode, struct file *file)
{
	unsigned int cpu;
	struct cpuinfo_x86 *c;
	int ret = 0;

	lock_kernel();
	cpu = iminor(file->f_path.dentry->d_inode);

	if (cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		ret = -ENXIO;	/* No such CPU */
		goto out;
	}
	c = &cpu_data(cpu);
	if (!cpu_has(c, X86_FEATURE_MSR))
		ret = -EIO;	/* MSR not supported */
out:
	unlock_kernel();
	return ret;
}

/*
 * File operations we support
 */
static const struct file_operations msr_fops = {
	.owner = THIS_MODULE,
	.llseek = msr_seek,
	.read = msr_read,
	.write = msr_write,
	.open = msr_open,
};

static int __cpuinit msr_device_create(int cpu)
{
	struct device *dev;

	dev = device_create(msr_class, NULL, MKDEV(majordev, cpu), NULL,
			    "msr_safe%d", cpu);
	return IS_ERR(dev) ? PTR_ERR(dev) : 0;
}

static void msr_device_destroy(int cpu)
{
	device_destroy(msr_class, MKDEV(majordev, cpu));
}

static int __cpuinit msr_class_cpu_callback(struct notifier_block *nfb,
				unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	int err = 0;

	switch (action) {
	case CPU_UP_PREPARE:
		err = msr_device_create(cpu);
		break;
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
	case CPU_DEAD:
		msr_device_destroy(cpu);
		break;
	}
	return notifier_from_errno(err);
}

static struct notifier_block __refdata msr_class_cpu_notifier = {
	.notifier_call = msr_class_cpu_callback,
};

static char *msr_devnode(struct device *dev, mode_t *mode)
{
	return kasprintf(GFP_KERNEL, "cpu/%u/msr_safe", MINOR(dev->devt));
}

static int __init msr_init(void)
{
	int i, err = 0;
	i = 0;

	majordev = __register_chrdev(0, 0, NR_CPUS, "cpu/msr_safe", &msr_fops);
	if (majordev < 0) {
		printk(KERN_ERR "msr_safe: unable to register device number\n");
		err = -EBUSY;
		goto out;
	}
	msr_class = class_create(THIS_MODULE, "msr_safe");
	if (IS_ERR(msr_class)) {
		err = PTR_ERR(msr_class);
		goto out_chrdev;
	}
	msr_class->devnode = msr_devnode;
	for_each_online_cpu(i) {
		err = msr_device_create(i);
		if (err != 0)
			goto out_class;
	}
	register_hotcpu_notifier(&msr_class_cpu_notifier);

	err = 0;
	goto out;

out_class:
	i = 0;
	for_each_online_cpu(i)
		msr_device_destroy(i);
	class_destroy(msr_class);
out_chrdev:
	__unregister_chrdev(majordev, 0, NR_CPUS, "cpu/msr");
out:
	return err;
}

static void __exit msr_exit(void)
{
	int cpu = 0;
	for_each_online_cpu(cpu)
		msr_device_destroy(cpu);
	class_destroy(msr_class);
	__unregister_chrdev(majordev, 0, NR_CPUS, "cpu/msr");
	unregister_hotcpu_notifier(&msr_class_cpu_notifier);
}

module_init(msr_init);
module_exit(msr_exit)

MODULE_AUTHOR("Barry Rountree <rountree@llnl.gov>");
MODULE_DESCRIPTION("x86 sanitized MSR driver");
MODULE_LICENSE("GPL");
