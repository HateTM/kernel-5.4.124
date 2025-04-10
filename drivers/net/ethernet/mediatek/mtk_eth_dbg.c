﻿/*
 *   Copyright (C) 2018 MediaTek Inc.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/trace_seq.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/u64_stats_sync.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/of_mdio.h>

#include "mtk_eth_soc.h"
#include "mtk_eth_dbg.h"

u32 hw_lro_agg_num_cnt[MTK_HW_LRO_RING_NUM][MTK_HW_LRO_MAX_AGG_CNT + 1];
u32 hw_lro_agg_size_cnt[MTK_HW_LRO_RING_NUM][16];
u32 hw_lro_tot_agg_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_tot_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_agg_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_age_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_seq_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_timestamp_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 hw_lro_norule_flush_cnt[MTK_HW_LRO_RING_NUM];
u32 mtk_hwlro_stats_ebl;
static struct proc_dir_entry *proc_hw_lro_stats, *proc_hw_lro_auto_tlb;

#if defined(CONFIG_TP_IMAGE)
int phy_link_up_flag = 0;
int is_1g_wan = 0;
int wan_fc_status = 0;
/*0 means auto negotiation. 1 means just turn off rx. 2 means just turn off tx.3 means turn off all flow control.
You can view the meanings of other values in function gmac_fc_status_wirte.*/
int gmac_fc_status = 2;
/*The value can be 0 or 1, 1 means eth1, 0 means eth0. Gpy211 is currently connected to eth1.*/
int gmac_id = 1;
int wan_fc_port = 0;
int lan_fc_status = 0;
#endif

typedef int (*mtk_lro_dbg_func) (int par);

struct mtk_eth_debug {
        struct dentry *root;
};

struct mtk_eth *g_eth;

struct mtk_eth_debug eth_debug;

void mt7530_mdio_w32(struct mtk_eth *eth, u16 reg, u32 val)
{
	mutex_lock(&eth->mii_bus->mdio_lock);

	_mtk_mdio_write(eth, 0x1f, 0x1f, (reg >> 6) & 0x3ff);
	_mtk_mdio_write(eth, 0x1f, (reg >> 2) & 0xf,  val & 0xffff);
	_mtk_mdio_write(eth, 0x1f, 0x10, val >> 16);

	mutex_unlock(&eth->mii_bus->mdio_lock);
}

u32 mt7530_mdio_r32(struct mtk_eth *eth, u32 reg)
{
	u16 high, low;

	mutex_lock(&eth->mii_bus->mdio_lock);

	_mtk_mdio_write(eth, 0x1f, 0x1f, (reg >> 6) & 0x3ff);
	low = _mtk_mdio_read(eth, 0x1f, (reg >> 2) & 0xf);
	high = _mtk_mdio_read(eth, 0x1f, 0x10);

	mutex_unlock(&eth->mii_bus->mdio_lock);

	return (high << 16) | (low & 0xffff);
}

void mtk_switch_w32(struct mtk_eth *eth, u32 val, unsigned reg)
{
	mtk_w32(eth, val, reg + 0x10000);
}
EXPORT_SYMBOL(mtk_switch_w32);

u32 mtk_switch_r32(struct mtk_eth *eth, unsigned reg)
{
	return mtk_r32(eth, reg + 0x10000);
}
EXPORT_SYMBOL(mtk_switch_r32);

static int mtketh_debug_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	struct mtk_mac *mac = 0;
	int  i = 0;

	for (i = 0 ; i < MTK_MAX_DEVS ; i++) {
		if (!eth->mac[i] ||
		    of_phy_is_fixed_link(eth->mac[i]->of_node))
			continue;
		mac = eth->mac[i];
#if 0 //FIXME
		while (j < 30) {
			d =  _mtk_mdio_read(eth, mac->phy_dev->addr, j);

			seq_printf(m, "phy=%d, reg=0x%08x, data=0x%08x\n",
				   mac->phy_dev->addr, j, d);
			j++;
		}
#endif		
	}
	return 0;
}

static int mtketh_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtketh_debug_show, inode->i_private);
}

static const struct file_operations mtketh_debug_fops = {
	.open = mtketh_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mtketh_mt7530sw_debug_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	u32  offset, data;
	int i;
	struct mt7530_ranges {
		u32 start;
		u32 end;
	} ranges[] = {
		{0x0, 0xac},
		{0x1000, 0x10e0},
		{0x1100, 0x1140},
		{0x1200, 0x1240},
		{0x1300, 0x1340},
		{0x1400, 0x1440},
		{0x1500, 0x1540},
		{0x1600, 0x1640},
		{0x1800, 0x1848},
		{0x1900, 0x1948},
		{0x1a00, 0x1a48},
		{0x1b00, 0x1b48},
		{0x1c00, 0x1c48},
		{0x1d00, 0x1d48},
		{0x1e00, 0x1e48},
		{0x1f60, 0x1ffc},
		{0x2000, 0x212c},
		{0x2200, 0x222c},
		{0x2300, 0x232c},
		{0x2400, 0x242c},
		{0x2500, 0x252c},
		{0x2600, 0x262c},
		{0x3000, 0x3014},
		{0x30c0, 0x30f8},
		{0x3100, 0x3114},
		{0x3200, 0x3214},
		{0x3300, 0x3314},
		{0x3400, 0x3414},
		{0x3500, 0x3514},
		{0x3600, 0x3614},
		{0x4000, 0x40d4},
		{0x4100, 0x41d4},
		{0x4200, 0x42d4},
		{0x4300, 0x43d4},
		{0x4400, 0x44d4},
		{0x4500, 0x45d4},
		{0x4600, 0x46d4},
		{0x4f00, 0x461c},
		{0x7000, 0x7038},
		{0x7120, 0x7124},
		{0x7800, 0x7804},
		{0x7810, 0x7810},
		{0x7830, 0x7830},
		{0x7a00, 0x7a7c},
		{0x7b00, 0x7b04},
		{0x7e00, 0x7e04},
		{0x7ffc, 0x7ffc},
	};

	if (!mt7530_exist(eth))
		return -EOPNOTSUPP;

	if ((!eth->mac[0] || !of_phy_is_fixed_link(eth->mac[0]->of_node)) &&
	    (!eth->mac[1] || !of_phy_is_fixed_link(eth->mac[1]->of_node))) {
		seq_puts(m, "no switch found\n");
		return 0;
	}

	for (i = 0 ; i < ARRAY_SIZE(ranges) ; i++) {
		for (offset = ranges[i].start;
		     offset <= ranges[i].end; offset += 4) {
			data =  mt7530_mdio_r32(eth, offset);
			seq_printf(m, "mt7530 switch reg=0x%08x, data=0x%08x\n",
				   offset, data);
		}
	}

	return 0;
}

static int mtketh_debug_mt7530sw_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtketh_mt7530sw_debug_show, inode->i_private);
}

static const struct file_operations mtketh_debug_mt7530sw_fops = {
	.open = mtketh_debug_mt7530sw_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t mtketh_mt7530sw_debugfs_write(struct file *file,
					     const char __user *ptr,
					     size_t len, loff_t *off)
{
	struct mtk_eth *eth = file->private_data;
	char buf[32], *token, *p = buf;
	u32 reg, value, phy;
	int ret;

	if (!mt7530_exist(eth))
		return -EOPNOTSUPP;

	if (*off != 0)
		return 0;

	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;

	ret = strncpy_from_user(buf, ptr, len);
	if (ret < 0)
		return ret;
	buf[len] = '\0';

	token = strsep(&p, " ");
	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&phy))
		return -EINVAL;

	token = strsep(&p, " ");
	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&reg))
		return -EINVAL;

	token = strsep(&p, " ");
	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&value))
		return -EINVAL;

	pr_info("%s:phy=%d, reg=0x%x, val=0x%x\n", __func__,
		0x1f, reg, value);
	mt7530_mdio_w32(eth, reg, value);
	pr_info("%s:phy=%d, reg=0x%x, val=0x%x confirm..\n", __func__,
		0x1f, reg, mt7530_mdio_r32(eth, reg));

	return len;
}

static ssize_t mtketh_debugfs_write(struct file *file, const char __user *ptr,
				    size_t len, loff_t *off)
{
	struct mtk_eth *eth = file->private_data;
	char buf[32], *token, *p = buf;
	u32 reg, value, phy;
	int ret;

	if (*off != 0)
		return 0;

	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;

	ret = strncpy_from_user(buf, ptr, len);
	if (ret < 0)
		return ret;
	buf[len] = '\0';

	token = strsep(&p, " ");
	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&phy))
		return -EINVAL;

	token = strsep(&p, " ");

	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&reg))
		return -EINVAL;

	token = strsep(&p, " ");

	if (!token)
		return -EINVAL;
	if (kstrtoul(token, 16, (unsigned long *)&value))
		return -EINVAL;

	pr_info("%s:phy=%d, reg=0x%x, val=0x%x\n", __func__,
		phy, reg, value);

	_mtk_mdio_write(eth, phy,  reg, value);

	pr_info("%s:phy=%d, reg=0x%x, val=0x%x confirm..\n", __func__,
		phy, reg, _mtk_mdio_read(eth, phy, reg));

	return len;
}

static ssize_t mtketh_debugfs_reset(struct file *file, const char __user *ptr,
				    size_t len, loff_t *off)
{
	struct mtk_eth *eth = file->private_data;

	schedule_work(&eth->pending_work);
	return len;
}

static const struct file_operations fops_reg_w = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mtketh_debugfs_write,
	.llseek = noop_llseek,
};

static const struct file_operations fops_eth_reset = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mtketh_debugfs_reset,
	.llseek = noop_llseek,
};

static const struct file_operations fops_mt7530sw_reg_w = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = mtketh_mt7530sw_debugfs_write,
	.llseek = noop_llseek,
};

void mtketh_debugfs_exit(struct mtk_eth *eth)
{
	debugfs_remove_recursive(eth_debug.root);
}

int mtketh_debugfs_init(struct mtk_eth *eth)
{
	int ret = 0;

	eth_debug.root = debugfs_create_dir("mtketh", NULL);
	if (!eth_debug.root) {
		dev_notice(eth->dev, "%s:err at %d\n", __func__, __LINE__);
		ret = -ENOMEM;
	}

	debugfs_create_file("phy_regs", S_IRUGO,
			    eth_debug.root, eth, &mtketh_debug_fops);
	debugfs_create_file("phy_reg_w", S_IFREG | S_IWUSR,
			    eth_debug.root, eth,  &fops_reg_w);
	debugfs_create_file("reset", S_IFREG | S_IWUSR,
			    eth_debug.root, eth,  &fops_eth_reset);
	if (mt7530_exist(eth)) {
		debugfs_create_file("mt7530sw_regs", S_IRUGO,
				    eth_debug.root, eth,
				    &mtketh_debug_mt7530sw_fops);
		debugfs_create_file("mt7530sw_reg_w", S_IFREG | S_IWUSR,
				    eth_debug.root, eth,
				    &fops_mt7530sw_reg_w);
	}
	return ret;
}

void mii_mgr_read_combine(struct mtk_eth *eth, u32 phy_addr, u32 phy_register,
			  u32 *read_data)
{
	if (mt7530_exist(eth) && phy_addr == 31)
		*read_data = mt7530_mdio_r32(eth, phy_register);

	else
	{
		mutex_lock(&eth->mii_bus->mdio_lock);
		*read_data = _mtk_mdio_read(eth, phy_addr, phy_register);
		mutex_unlock(&eth->mii_bus->mdio_lock);
	}
}

void mii_mgr_write_combine(struct mtk_eth *eth, u16 phy_addr, u16 phy_register,
			   u32 write_data)
{
	if (mt7530_exist(eth) && phy_addr == 31)
		mt7530_mdio_w32(eth, phy_register, write_data);

	else
	{
		mutex_lock(&eth->mii_bus->mdio_lock);
		_mtk_mdio_write(eth, phy_addr, phy_register, write_data);
		mutex_unlock(&eth->mii_bus->mdio_lock);
	}
}

static void mii_mgr_read_cl45(struct mtk_eth *eth, u16 port, u16 devad, u16 reg, u16 *data)
{
	mtk_cl45_ind_read(eth, port, devad, reg, data);
}

static void mii_mgr_write_cl45(struct mtk_eth *eth, u16 port, u16 devad, u16 reg, u16 data)
{
	mtk_cl45_ind_write(eth, port, devad, reg, data);
}

int mtk_do_priv_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_mii_ioctl_data mii;
	struct mtk_esw_reg reg;
	u16 val;

	switch (cmd) {
	case MTKETH_MII_READ:
		if (copy_from_user(&mii, ifr->ifr_data, sizeof(mii)))
			goto err_copy;
		mii_mgr_read_combine(eth, mii.phy_id, mii.reg_num,
				     &mii.val_out);
		if (copy_to_user(ifr->ifr_data, &mii, sizeof(mii)))
			goto err_copy;

		return 0;
	case MTKETH_MII_WRITE:
		if (copy_from_user(&mii, ifr->ifr_data, sizeof(mii)))
			goto err_copy;
		mii_mgr_write_combine(eth, mii.phy_id, mii.reg_num,
				      mii.val_in);
		return 0;
	case MTKETH_MII_READ_CL45:
		if (copy_from_user(&mii, ifr->ifr_data, sizeof(mii)))
			goto err_copy;
		mii_mgr_read_cl45(eth,
				  mdio_phy_id_prtad(mii.phy_id),
				  mdio_phy_id_devad(mii.phy_id),
				  mii.reg_num,
				  &val);
		mii.val_out = val;
		if (copy_to_user(ifr->ifr_data, &mii, sizeof(mii)))
			goto err_copy;

		return 0;
	case MTKETH_MII_WRITE_CL45:
		if (copy_from_user(&mii, ifr->ifr_data, sizeof(mii)))
			goto err_copy;
		val = mii.val_in;
		mii_mgr_write_cl45(eth,
				  mdio_phy_id_prtad(mii.phy_id),
				  mdio_phy_id_devad(mii.phy_id),
				  mii.reg_num,
				  val);
		return 0;
	case MTKETH_ESW_REG_READ:
		if (!mt7530_exist(eth))
			return -EOPNOTSUPP;
		if (copy_from_user(&reg, ifr->ifr_data, sizeof(reg)))
			goto err_copy;
		if (reg.off > REG_ESW_MAX)
			return -EINVAL;
		reg.val = mtk_switch_r32(eth, reg.off);

		if (copy_to_user(ifr->ifr_data, &reg, sizeof(reg)))
			goto err_copy;

		return 0;
	case MTKETH_ESW_REG_WRITE:
		if (!mt7530_exist(eth))
			return -EOPNOTSUPP;
		if (copy_from_user(&reg, ifr->ifr_data, sizeof(reg)))
			goto err_copy;
		if (reg.off > REG_ESW_MAX)
			return -EINVAL;
		mtk_switch_w32(eth, reg.val, reg.off);

		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
err_copy:
	return -EFAULT;
}

int esw_cnt_read(struct seq_file *seq, void *v)
{
	unsigned int pkt_cnt = 0;
	int i = 0;
	struct mtk_eth *eth = g_eth;
	unsigned int mib_base = MTK_GDM1_TX_GBCNT;

	seq_puts(seq, "\n		  <<CPU>>\n");
	seq_puts(seq, "		    |\n");
	seq_puts(seq, "+-----------------------------------------------+\n");
	seq_puts(seq, "|		  <<PSE>>		        |\n");
	seq_puts(seq, "+-----------------------------------------------+\n");
	seq_puts(seq, "		   |\n");
	seq_puts(seq, "+-----------------------------------------------+\n");
	seq_puts(seq, "|		  <<GDMA>>		        |\n");
	seq_printf(seq, "| GDMA1_RX_GBCNT  : %010u (Rx Good Bytes)	|\n",
		   mtk_r32(eth, mib_base));
	seq_printf(seq, "| GDMA1_RX_GPCNT  : %010u (Rx Good Pkts)	|\n",
		   mtk_r32(eth, mib_base+0x08));
	seq_printf(seq, "| GDMA1_RX_OERCNT : %010u (overflow error)	|\n",
		   mtk_r32(eth, mib_base+0x10));
	seq_printf(seq, "| GDMA1_RX_FERCNT : %010u (FCS error)	|\n",
		   mtk_r32(eth, mib_base+0x14));
	seq_printf(seq, "| GDMA1_RX_SERCNT : %010u (too short)	|\n",
		   mtk_r32(eth, mib_base+0x18));
	seq_printf(seq, "| GDMA1_RX_LERCNT : %010u (too long)	|\n",
		   mtk_r32(eth, mib_base+0x1C));
	seq_printf(seq, "| GDMA1_RX_CERCNT : %010u (checksum error)	|\n",
		   mtk_r32(eth, mib_base+0x20));
	seq_printf(seq, "| GDMA1_RX_FCCNT  : %010u (flow control)	|\n",
		   mtk_r32(eth, mib_base+0x24));
	seq_printf(seq, "| GDMA1_TX_SKIPCNT: %010u (about count)	|\n",
		   mtk_r32(eth, mib_base+0x28));
	seq_printf(seq, "| GDMA1_TX_COLCNT : %010u (collision count)	|\n",
		   mtk_r32(eth, mib_base+0x2C));
	seq_printf(seq, "| GDMA1_TX_GBCNT  : %010u (Tx Good Bytes)	|\n",
		   mtk_r32(eth, mib_base+0x30));
	seq_printf(seq, "| GDMA1_TX_GPCNT  : %010u (Tx Good Pkts)	|\n",
		   mtk_r32(eth, mib_base+0x38));
	seq_puts(seq, "|						|\n");
	seq_printf(seq, "| GDMA2_RX_GBCNT  : %010u (Rx Good Bytes)	|\n",
		   mtk_r32(eth, mib_base+0x40));
	seq_printf(seq, "| GDMA2_RX_GPCNT  : %010u (Rx Good Pkts)	|\n",
		   mtk_r32(eth, mib_base+0x48));
	seq_printf(seq, "| GDMA2_RX_OERCNT : %010u (overflow error)	|\n",
		   mtk_r32(eth, mib_base+0x50));
	seq_printf(seq, "| GDMA2_RX_FERCNT : %010u (FCS error)	|\n",
		   mtk_r32(eth, mib_base+0x54));
	seq_printf(seq, "| GDMA2_RX_SERCNT : %010u (too short)	|\n",
		   mtk_r32(eth, mib_base+0x58));
	seq_printf(seq, "| GDMA2_RX_LERCNT : %010u (too long)	|\n",
		   mtk_r32(eth, mib_base+0x5C));
	seq_printf(seq, "| GDMA2_RX_CERCNT : %010u (checksum error)	|\n",
		   mtk_r32(eth, mib_base+0x60));
	seq_printf(seq, "| GDMA2_RX_FCCNT  : %010u (flow control)	|\n",
		   mtk_r32(eth, mib_base+0x64));
	seq_printf(seq, "| GDMA2_TX_SKIPCNT: %010u (skip)		|\n",
		   mtk_r32(eth, mib_base+0x68));
	seq_printf(seq, "| GDMA2_TX_COLCNT : %010u (collision)	|\n",
		   mtk_r32(eth, mib_base+0x6C));
	seq_printf(seq, "| GDMA2_TX_GBCNT  : %010u (Tx Good Bytes)	|\n",
		   mtk_r32(eth, mib_base+0x70));
	seq_printf(seq, "| GDMA2_TX_GPCNT  : %010u (Tx Good Pkts)	|\n",
		   mtk_r32(eth, mib_base+0x78));
	seq_puts(seq, "+-----------------------------------------------+\n");

	if (!mt7530_exist(eth))
		return 0;

#define DUMP_EACH_PORT(base)					\
	do { \
		for (i = 0; i < 7; i++) {				\
			pkt_cnt = mt7530_mdio_r32(eth, (base) + (i * 0x100));\
			seq_printf(seq, "%8u ", pkt_cnt);		\
		}							\
		seq_puts(seq, "\n"); \
	} while (0)

	seq_printf(seq, "===================== %8s %8s %8s %8s %8s %8s %8s\n",
		   "Port0", "Port1", "Port2", "Port3", "Port4", "Port5",
		   "Port6");
	seq_puts(seq, "Tx Drop Packet      :");
	DUMP_EACH_PORT(0x4000);
	seq_puts(seq, "Tx CRC Error        :");
	DUMP_EACH_PORT(0x4004);
	seq_puts(seq, "Tx Unicast Packet   :");
	DUMP_EACH_PORT(0x4008);
	seq_puts(seq, "Tx Multicast Packet :");
	DUMP_EACH_PORT(0x400C);
	seq_puts(seq, "Tx Broadcast Packet :");
	DUMP_EACH_PORT(0x4010);
	seq_puts(seq, "Tx Collision Event  :");
	DUMP_EACH_PORT(0x4014);
	seq_puts(seq, "Tx Pause Packet     :");
	DUMP_EACH_PORT(0x402C);
	seq_puts(seq, "Rx Drop Packet      :");
	DUMP_EACH_PORT(0x4060);
	seq_puts(seq, "Rx Filtering Packet :");
	DUMP_EACH_PORT(0x4064);
	seq_puts(seq, "Rx Unicast Packet   :");
	DUMP_EACH_PORT(0x4068);
	seq_puts(seq, "Rx Multicast Packet :");
	DUMP_EACH_PORT(0x406C);
	seq_puts(seq, "Rx Broadcast Packet :");
	DUMP_EACH_PORT(0x4070);
	seq_puts(seq, "Rx Alignment Error  :");
	DUMP_EACH_PORT(0x4074);
	seq_puts(seq, "Rx CRC Error	    :");
	DUMP_EACH_PORT(0x4078);
	seq_puts(seq, "Rx Undersize Error  :");
	DUMP_EACH_PORT(0x407C);
	seq_puts(seq, "Rx Fragment Error   :");
	DUMP_EACH_PORT(0x4080);
	seq_puts(seq, "Rx Oversize Error   :");
	DUMP_EACH_PORT(0x4084);
	seq_puts(seq, "Rx Jabber Error     :");
	DUMP_EACH_PORT(0x4088);
	seq_puts(seq, "Rx Pause Packet     :");
	DUMP_EACH_PORT(0x408C);
	mt7530_mdio_w32(eth, 0x4fe0, 0xf0);
	mt7530_mdio_w32(eth, 0x4fe0, 0x800000f0);

	seq_puts(seq, "\n");

	return 0;
}

static int switch_count_open(struct inode *inode, struct file *file)
{
	return single_open(file, esw_cnt_read, 0);
}

static const struct file_operations switch_count_fops = {
	.owner = THIS_MODULE,
	.open = switch_count_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release
};

static struct proc_dir_entry *proc_tx_ring, *proc_rx_ring;

int tx_ring_read(struct seq_file *seq, void *v)
{
	struct mtk_tx_ring *ring = &g_eth->tx_ring;
	struct mtk_tx_dma *tx_ring;
	int i = 0;

	tx_ring =
	    kmalloc(sizeof(struct mtk_tx_dma) * MTK_DMA_SIZE, GFP_KERNEL);
	if (!tx_ring) {
		seq_puts(seq, " allocate temp tx_ring fail.\n");
		return 0;
	}

	for (i = 0; i < MTK_DMA_SIZE; i++)
		tx_ring[i] = ring->dma[i];

	seq_printf(seq, "free count = %d\n", (int)atomic_read(&ring->free_count));
	seq_printf(seq, "cpu next free: %d\n", (int)(ring->next_free - ring->dma));
	seq_printf(seq, "cpu last free: %d\n", (int)(ring->last_free - ring->dma));
	for (i = 0; i < MTK_DMA_SIZE; i++) {
		dma_addr_t tmp = ring->phys + i * sizeof(*tx_ring);

		seq_printf(seq, "%d (%pad): %08x %08x %08x %08x", i, &tmp,
			   *(int *)&tx_ring[i].txd1, *(int *)&tx_ring[i].txd2,
			   *(int *)&tx_ring[i].txd3, *(int *)&tx_ring[i].txd4);
#if defined(CONFIG_MEDIATEK_NETSYS_V2)
		seq_printf(seq, " %08x %08x %08x %08x",
			   *(int *)&tx_ring[i].txd5, *(int *)&tx_ring[i].txd6,
			   *(int *)&tx_ring[i].txd7, *(int *)&tx_ring[i].txd8);
#endif
		seq_printf(seq, "\n");
	}

	kfree(tx_ring);
	return 0;
}

static int tx_ring_open(struct inode *inode, struct file *file)
{
	return single_open(file, tx_ring_read, NULL);
}

static const struct file_operations tx_ring_fops = {
	.owner = THIS_MODULE,
	.open = tx_ring_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release
};

int rx_ring_read(struct seq_file *seq, void *v)
{
	struct mtk_rx_ring *ring = &g_eth->rx_ring[0];
	struct mtk_rx_dma *rx_ring;

	int i = 0;

	rx_ring =
	    kmalloc(sizeof(struct mtk_rx_dma) * MTK_DMA_SIZE, GFP_KERNEL);
	if (!rx_ring) {
		seq_puts(seq, " allocate temp rx_ring fail.\n");
		return 0;
	}

	for (i = 0; i < MTK_DMA_SIZE; i++)
		rx_ring[i] = ring->dma[i];

	seq_printf(seq, "next to read: %d\n",
		   NEXT_DESP_IDX(ring->calc_idx, MTK_DMA_SIZE));
	for (i = 0; i < MTK_DMA_SIZE; i++) {
		seq_printf(seq, "%d: %08x %08x %08x %08x", i,
			   *(int *)&rx_ring[i].rxd1, *(int *)&rx_ring[i].rxd2,
			   *(int *)&rx_ring[i].rxd3, *(int *)&rx_ring[i].rxd4);
#if defined(CONFIG_MEDIATEK_NETSYS_V2)
		seq_printf(seq, " %08x %08x %08x %08x",
			   *(int *)&rx_ring[i].rxd5, *(int *)&rx_ring[i].rxd6,
			   *(int *)&rx_ring[i].rxd7, *(int *)&rx_ring[i].rxd8);
#endif
		seq_printf(seq, "\n");
	}

	kfree(rx_ring);
	return 0;
}

static int rx_ring_open(struct inode *inode, struct file *file)
{
	return single_open(file, rx_ring_read, NULL);
}

static const struct file_operations rx_ring_fops = {
	.owner = THIS_MODULE,
	.open = rx_ring_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release
};

static inline u32 mtk_dbg_r32(u32 reg)
{
	void __iomem *virt_reg;
	u32 val;

	virt_reg = ioremap(reg, 32);
	val = __raw_readl(virt_reg);
	iounmap(virt_reg);

	return val;
}

int dbg_regs_read(struct seq_file *seq, void *v)
{
	struct mtk_eth *eth = g_eth;

	seq_puts(seq, "   <<DEBUG REG DUMP>>\n");

	seq_printf(seq, "| FE_INT_STA	: %08x |\n",
		   mtk_r32(eth, MTK_INT_STATUS));
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2))
		seq_printf(seq, "| FE_INT_STA2	: %08x |\n",
			   mtk_r32(eth, MTK_INT_STATUS2));

	seq_printf(seq, "| PSE_FQFC_CFG	: %08x |\n",
		   mtk_r32(eth, MTK_PSE_FQFC_CFG));
	seq_printf(seq, "| PSE_IQ_STA1	: %08x |\n",
		   mtk_r32(eth, MTK_PSE_IQ_STA(0)));
	seq_printf(seq, "| PSE_IQ_STA2	: %08x |\n",
		   mtk_r32(eth, MTK_PSE_IQ_STA(1)));

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2)) {
		seq_printf(seq, "| PSE_IQ_STA3	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_IQ_STA(2)));
		seq_printf(seq, "| PSE_IQ_STA4	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_IQ_STA(3)));
		seq_printf(seq, "| PSE_IQ_STA5	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_IQ_STA(4)));
	}

	seq_printf(seq, "| PSE_OQ_STA1	: %08x |\n",
		   mtk_r32(eth, MTK_PSE_OQ_STA(0)));
	seq_printf(seq, "| PSE_OQ_STA2	: %08x |\n",
		   mtk_r32(eth, MTK_PSE_OQ_STA(1)));

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2)) {
		seq_printf(seq, "| PSE_OQ_STA3	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_OQ_STA(2)));
		seq_printf(seq, "| PSE_OQ_STA4	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_OQ_STA(3)));
		seq_printf(seq, "| PSE_OQ_STA5	: %08x |\n",
			   mtk_r32(eth, MTK_PSE_OQ_STA(4)));
	}

	seq_printf(seq, "| PDMA_CRX_IDX	: %08x |\n",
		   mtk_r32(eth, MTK_PRX_CRX_IDX0));
	seq_printf(seq, "| PDMA_DRX_IDX	: %08x |\n",
		   mtk_r32(eth, MTK_PRX_DRX_IDX0));
	seq_printf(seq, "| QDMA_CTX_IDX	: %08x |\n",
		   mtk_r32(eth, MTK_QTX_CTX_PTR));
	seq_printf(seq, "| QDMA_DTX_IDX	: %08x |\n",
		   mtk_r32(eth, MTK_QTX_DTX_PTR));
	seq_printf(seq, "| QDMA_FQ_CNT	: %08x |\n",
		   mtk_r32(eth, MTK_QDMA_FQ_CNT));
	seq_printf(seq, "| FE_PSE_FREE	: %08x |\n",
		   mtk_r32(eth, MTK_FE_PSE_FREE));
	seq_printf(seq, "| FE_DROP_FQ	: %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_FQ));
	seq_printf(seq, "| FE_DROP_FC	: %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_FC));
	seq_printf(seq, "| FE_DROP_PPE	: %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_PPE));
	seq_printf(seq, "| GDM1_IG_CTRL	: %08x |\n",
		   mtk_r32(eth, MTK_GDMA_FWD_CFG(0)));
	seq_printf(seq, "| GDM2_IG_CTRL	: %08x |\n",
		   mtk_r32(eth, MTK_GDMA_FWD_CFG(1)));
	seq_printf(seq, "| MAC_P1_MCR	: %08x |\n",
		   mtk_r32(eth, MTK_MAC_MCR(0)));
	seq_printf(seq, "| MAC_P2_MCR	: %08x |\n",
		   mtk_r32(eth, MTK_MAC_MCR(1)));
	seq_printf(seq, "| MAC_P1_FSM	: %08x |\n",
		   mtk_r32(eth, MTK_MAC_FSM(0)));
	seq_printf(seq, "| MAC_P2_FSM	: %08x |\n",
		   mtk_r32(eth, MTK_MAC_FSM(1)));

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2)) {
		seq_printf(seq, "| FE_CDM1_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_CDM1_FSM));
		seq_printf(seq, "| FE_CDM2_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_CDM2_FSM));
		seq_printf(seq, "| FE_CDM3_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_CDM3_FSM));
		seq_printf(seq, "| FE_CDM4_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_CDM4_FSM));
		seq_printf(seq, "| FE_GDM1_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_GDM1_FSM));
		seq_printf(seq, "| FE_GDM2_FSM	: %08x |\n",
			   mtk_r32(eth, MTK_FE_GDM2_FSM));
		seq_printf(seq, "| SGMII_EFUSE	: %08x |\n",
			   mtk_dbg_r32(MTK_SGMII_EFUSE));
		seq_printf(seq, "| SGMII0_RX_CNT : %08x |\n",
			   mtk_dbg_r32(MTK_SGMII_FALSE_CARRIER_CNT(0)));
		seq_printf(seq, "| SGMII1_RX_CNT : %08x |\n",
			   mtk_dbg_r32(MTK_SGMII_FALSE_CARRIER_CNT(1)));
		seq_printf(seq, "| WED_RTQM_GLO	: %08x |\n",
			   mtk_dbg_r32(MTK_WED_RTQM_GLO_CFG));
	}

	mtk_w32(eth, 0xffffffff, MTK_INT_STATUS);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2))
		mtk_w32(eth, 0xffffffff, MTK_INT_STATUS2);

	return 0;
}

static int dbg_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_regs_read, 0);
}

static const struct file_operations dbg_regs_fops = {
	.owner = THIS_MODULE,
	.open = dbg_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release
};

void hw_lro_stats_update(u32 ring_no, struct mtk_rx_dma *rxd)
{
	u32 idx, agg_cnt, agg_size;

#if defined(CONFIG_MEDIATEK_NETSYS_V2)
	idx = ring_no - 4;
	agg_cnt = RX_DMA_GET_AGG_CNT_V2(rxd->rxd6);
#else
	idx = ring_no - 1;
	agg_cnt = RX_DMA_GET_AGG_CNT(rxd->rxd2);
#endif

	agg_size = RX_DMA_GET_PLEN0(rxd->rxd2);

	hw_lro_agg_size_cnt[idx][agg_size / 5000]++;
	hw_lro_agg_num_cnt[idx][agg_cnt]++;
	hw_lro_tot_flush_cnt[idx]++;
	hw_lro_tot_agg_cnt[idx] += agg_cnt;
}

void hw_lro_flush_stats_update(u32 ring_no, struct mtk_rx_dma *rxd)
{
	u32 idx, flush_reason;

#if defined(CONFIG_MEDIATEK_NETSYS_V2)
	idx = ring_no - 4;
	flush_reason = RX_DMA_GET_FLUSH_RSN_V2(rxd->rxd6);
#else
	idx = ring_no - 1;
	flush_reason = RX_DMA_GET_REV(rxd->rxd2);
#endif

	if ((flush_reason & 0x7) == MTK_HW_LRO_AGG_FLUSH)
		hw_lro_agg_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_AGE_FLUSH)
		hw_lro_age_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_NOT_IN_SEQ_FLUSH)
		hw_lro_seq_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_TIMESTAMP_FLUSH)
		hw_lro_timestamp_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_NON_RULE_FLUSH)
		hw_lro_norule_flush_cnt[idx]++;
}

ssize_t hw_lro_stats_write(struct file *file, const char __user *buffer,
			   size_t count, loff_t *data)
{
	memset(hw_lro_agg_num_cnt, 0, sizeof(hw_lro_agg_num_cnt));
	memset(hw_lro_agg_size_cnt, 0, sizeof(hw_lro_agg_size_cnt));
	memset(hw_lro_tot_agg_cnt, 0, sizeof(hw_lro_tot_agg_cnt));
	memset(hw_lro_tot_flush_cnt, 0, sizeof(hw_lro_tot_flush_cnt));
	memset(hw_lro_agg_flush_cnt, 0, sizeof(hw_lro_agg_flush_cnt));
	memset(hw_lro_age_flush_cnt, 0, sizeof(hw_lro_age_flush_cnt));
	memset(hw_lro_seq_flush_cnt, 0, sizeof(hw_lro_seq_flush_cnt));
	memset(hw_lro_timestamp_flush_cnt, 0,
	       sizeof(hw_lro_timestamp_flush_cnt));
	memset(hw_lro_norule_flush_cnt, 0, sizeof(hw_lro_norule_flush_cnt));

	pr_info("clear hw lro cnt table\n");

	return count;
}

int hw_lro_stats_read_v1(struct seq_file *seq, void *v)
{
	int i;

	seq_puts(seq, "HW LRO statistic dump:\n");

	/* Agg number count */
	seq_puts(seq, "Cnt:   RING1 | RING2 | RING3 | Total\n");
	for (i = 0; i <= MTK_HW_LRO_MAX_AGG_CNT; i++) {
		seq_printf(seq, " %d :      %d        %d        %d        %d\n",
			   i, hw_lro_agg_num_cnt[0][i],
			   hw_lro_agg_num_cnt[1][i], hw_lro_agg_num_cnt[2][i],
			   hw_lro_agg_num_cnt[0][i] + hw_lro_agg_num_cnt[1][i] +
			   hw_lro_agg_num_cnt[2][i]);
	}

	/* Total agg count */
	seq_puts(seq, "Total agg:   RING1 | RING2 | RING3 | Total\n");
	seq_printf(seq, "                %d      %d      %d      %d\n",
		   hw_lro_tot_agg_cnt[0], hw_lro_tot_agg_cnt[1],
		   hw_lro_tot_agg_cnt[2],
		   hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		   hw_lro_tot_agg_cnt[2]);

	/* Total flush count */
	seq_puts(seq, "Total flush:   RING1 | RING2 | RING3 | Total\n");
	seq_printf(seq, "                %d      %d      %d      %d\n",
		   hw_lro_tot_flush_cnt[0], hw_lro_tot_flush_cnt[1],
		   hw_lro_tot_flush_cnt[2],
		   hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		   hw_lro_tot_flush_cnt[2]);

	/* Avg agg count */
	seq_puts(seq, "Avg agg:   RING1 | RING2 | RING3 | Total\n");
	seq_printf(seq, "                %d      %d      %d      %d\n",
		   (hw_lro_tot_flush_cnt[0]) ?
		    hw_lro_tot_agg_cnt[0] / hw_lro_tot_flush_cnt[0] : 0,
		   (hw_lro_tot_flush_cnt[1]) ?
		    hw_lro_tot_agg_cnt[1] / hw_lro_tot_flush_cnt[1] : 0,
		   (hw_lro_tot_flush_cnt[2]) ?
		    hw_lro_tot_agg_cnt[2] / hw_lro_tot_flush_cnt[2] : 0,
		   (hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		    hw_lro_tot_flush_cnt[2]) ?
		    ((hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		      hw_lro_tot_agg_cnt[2]) / (hw_lro_tot_flush_cnt[0] +
		      hw_lro_tot_flush_cnt[1] + hw_lro_tot_flush_cnt[2])) : 0);

	/*  Statistics of aggregation size counts */
	seq_puts(seq, "HW LRO flush pkt len:\n");
	seq_puts(seq, " Length  | RING1  | RING2  | RING3  | Total\n");
	for (i = 0; i < 15; i++) {
		seq_printf(seq, "%d~%d: %d      %d      %d      %d\n", i * 5000,
			   (i + 1) * 5000, hw_lro_agg_size_cnt[0][i],
			   hw_lro_agg_size_cnt[1][i], hw_lro_agg_size_cnt[2][i],
			   hw_lro_agg_size_cnt[0][i] +
			   hw_lro_agg_size_cnt[1][i] +
			   hw_lro_agg_size_cnt[2][i]);
	}

	seq_puts(seq, "Flush reason:   RING1 | RING2 | RING3 | Total\n");
	seq_printf(seq, "AGG timeout:      %d      %d      %d      %d\n",
		   hw_lro_agg_flush_cnt[0], hw_lro_agg_flush_cnt[1],
		   hw_lro_agg_flush_cnt[2],
		   (hw_lro_agg_flush_cnt[0] + hw_lro_agg_flush_cnt[1] +
		    hw_lro_agg_flush_cnt[2]));

	seq_printf(seq, "AGE timeout:      %d      %d      %d      %d\n",
		   hw_lro_age_flush_cnt[0], hw_lro_age_flush_cnt[1],
		   hw_lro_age_flush_cnt[2],
		   (hw_lro_age_flush_cnt[0] + hw_lro_age_flush_cnt[1] +
		    hw_lro_age_flush_cnt[2]));

	seq_printf(seq, "Not in-sequence:  %d      %d      %d      %d\n",
		   hw_lro_seq_flush_cnt[0], hw_lro_seq_flush_cnt[1],
		   hw_lro_seq_flush_cnt[2],
		   (hw_lro_seq_flush_cnt[0] + hw_lro_seq_flush_cnt[1] +
		    hw_lro_seq_flush_cnt[2]));

	seq_printf(seq, "Timestamp:        %d      %d      %d      %d\n",
		   hw_lro_timestamp_flush_cnt[0],
		   hw_lro_timestamp_flush_cnt[1],
		   hw_lro_timestamp_flush_cnt[2],
		   (hw_lro_timestamp_flush_cnt[0] +
		    hw_lro_timestamp_flush_cnt[1] +
		    hw_lro_timestamp_flush_cnt[2]));

	seq_printf(seq, "No LRO rule:      %d      %d      %d      %d\n",
		   hw_lro_norule_flush_cnt[0],
		   hw_lro_norule_flush_cnt[1],
		   hw_lro_norule_flush_cnt[2],
		   (hw_lro_norule_flush_cnt[0] +
		    hw_lro_norule_flush_cnt[1] +
		    hw_lro_norule_flush_cnt[2]));

	return 0;
}

int hw_lro_stats_read_v2(struct seq_file *seq, void *v)
{
	int i;

	seq_puts(seq, "HW LRO statistic dump:\n");

	/* Agg number count */
	seq_puts(seq, "Cnt:   RING4 | RING5 | RING6 | RING7 Total\n");
	for (i = 0; i <= MTK_HW_LRO_MAX_AGG_CNT; i++) {
		seq_printf(seq,
			   " %d :      %d        %d        %d        %d        %d\n",
			   i, hw_lro_agg_num_cnt[0][i], hw_lro_agg_num_cnt[1][i],
			   hw_lro_agg_num_cnt[2][i], hw_lro_agg_num_cnt[3][i],
			   hw_lro_agg_num_cnt[0][i] + hw_lro_agg_num_cnt[1][i] +
			   hw_lro_agg_num_cnt[2][i] + hw_lro_agg_num_cnt[3][i]);
	}

	/* Total agg count */
	seq_puts(seq, "Total agg:   RING4 | RING5 | RING6 | RING7 Total\n");
	seq_printf(seq, "                %d      %d      %d      %d      %d\n",
		   hw_lro_tot_agg_cnt[0], hw_lro_tot_agg_cnt[1],
		   hw_lro_tot_agg_cnt[2], hw_lro_tot_agg_cnt[3],
		   hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		   hw_lro_tot_agg_cnt[2] + hw_lro_tot_agg_cnt[3]);

	/* Total flush count */
	seq_puts(seq, "Total flush:   RING4 | RING5 | RING6 | RING7 Total\n");
	seq_printf(seq, "                %d      %d      %d      %d      %d\n",
		   hw_lro_tot_flush_cnt[0], hw_lro_tot_flush_cnt[1],
		   hw_lro_tot_flush_cnt[2], hw_lro_tot_flush_cnt[3],
		   hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		   hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3]);

	/* Avg agg count */
	seq_puts(seq, "Avg agg:   RING4 | RING5 | RING6 | RING7 Total\n");
	seq_printf(seq, "                %d      %d      %d      %d      %d\n",
		   (hw_lro_tot_flush_cnt[0]) ?
		    hw_lro_tot_agg_cnt[0] / hw_lro_tot_flush_cnt[0] : 0,
		   (hw_lro_tot_flush_cnt[1]) ?
		    hw_lro_tot_agg_cnt[1] / hw_lro_tot_flush_cnt[1] : 0,
		   (hw_lro_tot_flush_cnt[2]) ?
		    hw_lro_tot_agg_cnt[2] / hw_lro_tot_flush_cnt[2] : 0,
		   (hw_lro_tot_flush_cnt[3]) ?
                    hw_lro_tot_agg_cnt[3] / hw_lro_tot_flush_cnt[3] : 0,
		   (hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		    hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3]) ?
		    ((hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		      hw_lro_tot_agg_cnt[2] + hw_lro_tot_agg_cnt[3]) /
		     (hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		      hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3])) : 0);

	/*  Statistics of aggregation size counts */
	seq_puts(seq, "HW LRO flush pkt len:\n");
	seq_puts(seq, " Length  | RING4  | RING5  | RING6  | RING7 Total\n");
	for (i = 0; i < 15; i++) {
		seq_printf(seq, "%d~%d: %d      %d      %d      %d      %d\n",
			   i * 5000, (i + 1) * 5000,
			   hw_lro_agg_size_cnt[0][i], hw_lro_agg_size_cnt[1][i],
			   hw_lro_agg_size_cnt[2][i], hw_lro_agg_size_cnt[3][i],
			   hw_lro_agg_size_cnt[0][i] +
			   hw_lro_agg_size_cnt[1][i] +
			   hw_lro_agg_size_cnt[2][i] +
			   hw_lro_agg_size_cnt[3][i]);
	}

	seq_puts(seq, "Flush reason:   RING4 | RING5 | RING6 | RING7 Total\n");
	seq_printf(seq, "AGG timeout:      %d      %d      %d      %d      %d\n",
		   hw_lro_agg_flush_cnt[0], hw_lro_agg_flush_cnt[1],
		   hw_lro_agg_flush_cnt[2], hw_lro_agg_flush_cnt[3],
		   (hw_lro_agg_flush_cnt[0] + hw_lro_agg_flush_cnt[1] +
		    hw_lro_agg_flush_cnt[2] + hw_lro_agg_flush_cnt[3]));

	seq_printf(seq, "AGE timeout:      %d      %d      %d      %d      %d\n",
		   hw_lro_age_flush_cnt[0], hw_lro_age_flush_cnt[1],
		   hw_lro_age_flush_cnt[2], hw_lro_age_flush_cnt[3],
		   (hw_lro_age_flush_cnt[0] + hw_lro_age_flush_cnt[1] +
		    hw_lro_age_flush_cnt[2] + hw_lro_age_flush_cnt[3]));

	seq_printf(seq, "Not in-sequence:  %d      %d      %d      %d      %d\n",
		   hw_lro_seq_flush_cnt[0], hw_lro_seq_flush_cnt[1],
		   hw_lro_seq_flush_cnt[2], hw_lro_seq_flush_cnt[3],
		   (hw_lro_seq_flush_cnt[0] + hw_lro_seq_flush_cnt[1] +
		    hw_lro_seq_flush_cnt[2] + hw_lro_seq_flush_cnt[3]));

	seq_printf(seq, "Timestamp:        %d      %d      %d      %d      %d\n",
		   hw_lro_timestamp_flush_cnt[0],
		   hw_lro_timestamp_flush_cnt[1],
		   hw_lro_timestamp_flush_cnt[2],
		   hw_lro_timestamp_flush_cnt[3],
		   (hw_lro_timestamp_flush_cnt[0] +
		    hw_lro_timestamp_flush_cnt[1] +
		    hw_lro_timestamp_flush_cnt[2] +
		    hw_lro_timestamp_flush_cnt[3]));

	seq_printf(seq, "No LRO rule:      %d      %d      %d      %d      %d\n",
		   hw_lro_norule_flush_cnt[0],
		   hw_lro_norule_flush_cnt[1],
		   hw_lro_norule_flush_cnt[2],
		   hw_lro_norule_flush_cnt[3],
		   (hw_lro_norule_flush_cnt[0] +
		    hw_lro_norule_flush_cnt[1] +
		    hw_lro_norule_flush_cnt[2] +
		    hw_lro_norule_flush_cnt[3]));

	return 0;
}

int hw_lro_stats_read_wrapper(struct seq_file *seq, void *v)
{
	struct mtk_eth *eth = g_eth;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_NETSYS_V2))
		hw_lro_stats_read_v2(seq, v);
	else
		hw_lro_stats_read_v1(seq, v);

	return 0;
}

static int hw_lro_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, hw_lro_stats_read_wrapper, NULL);
}

static const struct file_operations hw_lro_stats_fops = {
	.owner = THIS_MODULE,
	.open = hw_lro_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = hw_lro_stats_write,
	.release = single_release
};

int hwlro_agg_cnt_ctrl(int cnt)
{
	int i;

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++)
		SET_PDMA_RXRING_MAX_AGG_CNT(g_eth, i, cnt);

	return 0;
}

int hwlro_agg_time_ctrl(int time)
{
	int i;

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++)
		SET_PDMA_RXRING_AGG_TIME(g_eth, i, time);

	return 0;
}

int hwlro_age_time_ctrl(int time)
{
	int i;

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++)
		SET_PDMA_RXRING_AGE_TIME(g_eth, i, time);

	return 0;
}

int hwlro_threshold_ctrl(int bandwidth)
{
	SET_PDMA_LRO_BW_THRESHOLD(g_eth, bandwidth);

	return 0;
}

int hwlro_ring_enable_ctrl(int enable)
{
	int i;

	pr_info("[%s] %s HW LRO rings\n", __func__, (enable) ? "Enable" : "Disable");

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++)
		SET_PDMA_RXRING_VALID(g_eth, i, enable);

	return 0;
}

int hwlro_stats_enable_ctrl(int enable)
{
	pr_info("[%s] %s HW LRO statistics\n", __func__, (enable) ? "Enable" : "Disable");
	mtk_hwlro_stats_ebl = enable;

	return 0;
}

static const mtk_lro_dbg_func lro_dbg_func[] = {
	[0] = hwlro_agg_cnt_ctrl,
	[1] = hwlro_agg_time_ctrl,
	[2] = hwlro_age_time_ctrl,
	[3] = hwlro_threshold_ctrl,
	[4] = hwlro_ring_enable_ctrl,
	[5] = hwlro_stats_enable_ctrl,
};

ssize_t hw_lro_auto_tlb_write(struct file *file, const char __user *buffer,
			      size_t count, loff_t *data)
{
	char buf[32];
	char *p_buf;
	char *p_token = NULL;
	char *p_delimiter = " \t";
	long x = 0, y = 0;
	int len = count;
	int ret;

	if (len >= sizeof(buf)) {
		pr_info("Input handling fail!\n");
		len = sizeof(buf) - 1;
		return -1;
	}

	if (copy_from_user(buf, buffer, len))
		return -EFAULT;

	buf[len] = '\0';

	p_buf = buf;
	p_token = strsep(&p_buf, p_delimiter);
	if (!p_token)
		x = 0;
	else
		ret = kstrtol(p_token, 10, &x);

	p_token = strsep(&p_buf, "\t\n ");
	if (p_token)
		ret = kstrtol(p_token, 10, &y);

	if (lro_dbg_func[x] && (ARRAY_SIZE(lro_dbg_func) > x))
		(*lro_dbg_func[x]) (y);

	return count;
}

void hw_lro_auto_tlb_dump_v1(struct seq_file *seq, u32 index)
{
	int i;
	struct mtk_lro_alt_v1 alt;
	__be32 addr;
	u32 tlb_info[9];
	u32 dw_len, cnt, priority;
	u32 entry;

	if (index > 4)
		index = index - 1;
	entry = (index * 9) + 1;

	/* read valid entries of the auto-learn table */
	mtk_w32(g_eth, entry, MTK_FE_ALT_CF8);

	for (i = 0; i < 9; i++)
		tlb_info[i] = mtk_r32(g_eth, MTK_FE_ALT_SEQ_CFC);

	memcpy(&alt, tlb_info, sizeof(struct mtk_lro_alt_v1));

	dw_len = alt.alt_info7.dw_len;
	cnt = alt.alt_info6.cnt;

	if (mtk_r32(g_eth, MTK_PDMA_LRO_CTRL_DW0) & MTK_LRO_ALT_PKT_CNT_MODE)
		priority = cnt;		/* packet count */
	else
		priority = dw_len;	/* byte count */

	/* dump valid entries of the auto-learn table */
	if (index >= 4)
		seq_printf(seq, "\n===== TABLE Entry: %d (Act) =====\n", index);
	else
		seq_printf(seq, "\n===== TABLE Entry: %d (LRU) =====\n", index);

	if (alt.alt_info8.ipv4) {
		addr = htonl(alt.alt_info1.sip0);
		seq_printf(seq, "SIP = %pI4 (IPv4)\n", &addr);
	} else {
		seq_printf(seq, "SIP = %08X:%08X:%08X:%08X (IPv6)\n",
			   alt.alt_info4.sip3, alt.alt_info3.sip2,
			   alt.alt_info2.sip1, alt.alt_info1.sip0);
	}

	seq_printf(seq, "DIP_ID = %d\n", alt.alt_info8.dip_id);
	seq_printf(seq, "TCP SPORT = %d | TCP DPORT = %d\n",
		   alt.alt_info0.stp, alt.alt_info0.dtp);
	seq_printf(seq, "VLAN_VID_VLD = %d\n", alt.alt_info6.vlan_vid_vld);
	seq_printf(seq, "VLAN1 = %d | VLAN2 = %d | VLAN3 = %d | VLAN4 =%d\n",
		   (alt.alt_info5.vlan_vid0 & 0xfff),
		   ((alt.alt_info5.vlan_vid0 >> 12) & 0xfff),
		   ((alt.alt_info6.vlan_vid1 << 8) |
		   ((alt.alt_info5.vlan_vid0 >> 24) & 0xfff)),
		   ((alt.alt_info6.vlan_vid1 >> 4) & 0xfff));
	seq_printf(seq, "TPUT = %d | FREQ = %d\n", dw_len, cnt);
	seq_printf(seq, "PRIORITY = %d\n", priority);
}

void hw_lro_auto_tlb_dump_v2(struct seq_file *seq, u32 index)
{
	int i;
	struct mtk_lro_alt_v2 alt;
	u32 score = 0, ipv4 = 0;
	u32 ipv6[4] = { 0 };
	u32 tlb_info[12];

	/* read valid entries of the auto-learn table */
	mtk_w32(g_eth, index << MTK_LRO_ALT_INDEX_OFFSET, MTK_LRO_ALT_DBG);

	for (i = 0; i < 11; i++)
		tlb_info[i] = mtk_r32(g_eth, MTK_LRO_ALT_DBG_DATA);

	memcpy(&alt, tlb_info, sizeof(struct mtk_lro_alt_v2));

	if (mtk_r32(g_eth, MTK_PDMA_LRO_CTRL_DW0) & MTK_LRO_ALT_PKT_CNT_MODE)
		score = 1;	/* packet count */
	else
		score = 0;	/* byte count */

	/* dump valid entries of the auto-learn table */
	if (alt.alt_info0.valid) {
		if (index < 5)
			seq_printf(seq,
				   "\n===== TABLE Entry: %d (onging) =====\n",
				   index);
		else
			seq_printf(seq,
				   "\n===== TABLE Entry: %d (candidate) =====\n",
				   index);

		if (alt.alt_info1.v4_valid) {
			ipv4 = (alt.alt_info4.sip0_h << 23) |
				alt.alt_info5.sip0_l;
			seq_printf(seq, "SIP = 0x%x: (IPv4)\n", ipv4);

			ipv4 = (alt.alt_info8.dip0_h << 23) |
				alt.alt_info9.dip0_l;
			seq_printf(seq, "DIP = 0x%x: (IPv4)\n", ipv4);
		} else if (alt.alt_info1.v6_valid) {
			ipv6[3] = (alt.alt_info1.sip3_h << 23) |
				   (alt.alt_info2.sip3_l << 9);
			ipv6[2] = (alt.alt_info2.sip2_h << 23) |
				   (alt.alt_info3.sip2_l << 9);
			ipv6[1] = (alt.alt_info3.sip1_h << 23) |
				   (alt.alt_info4.sip1_l << 9);
			ipv6[0] = (alt.alt_info4.sip0_h << 23) |
				   (alt.alt_info5.sip0_l << 9);
			seq_printf(seq, "SIP = 0x%x:0x%x:0x%x:0x%x (IPv6)\n",
				   ipv6[3], ipv6[2], ipv6[1], ipv6[0]);

			ipv6[3] = (alt.alt_info5.dip3_h << 23) |
				   (alt.alt_info6.dip3_l << 9);
			ipv6[2] = (alt.alt_info6.dip2_h << 23) |
				   (alt.alt_info7.dip2_l << 9);
			ipv6[1] = (alt.alt_info7.dip1_h << 23) |
				   (alt.alt_info8.dip1_l << 9);
			ipv6[0] = (alt.alt_info8.dip0_h << 23) |
				   (alt.alt_info9.dip0_l << 9);
			seq_printf(seq, "DIP = 0x%x:0x%x:0x%x:0x%x (IPv6)\n",
				   ipv6[3], ipv6[2], ipv6[1], ipv6[0]);
		}

		seq_printf(seq, "TCP SPORT = %d | TCP DPORT = %d\n",
			   (alt.alt_info9.sp_h << 7) | (alt.alt_info10.sp_l),
			   alt.alt_info10.dp);
	}
}

int hw_lro_auto_tlb_read(struct seq_file *seq, void *v)
{
	int i;
	u32 reg_val;
	u32 reg_op1, reg_op2, reg_op3, reg_op4;
	u32 agg_cnt, agg_time, age_time;

	seq_puts(seq, "Usage of /proc/mtketh/hw_lro_auto_tlb:\n");
	seq_puts(seq, "echo [function] [setting] > /proc/mtketh/hw_lro_auto_tlb\n");
	seq_puts(seq, "Functions:\n");
	seq_puts(seq, "[0] = hwlro_agg_cnt_ctrl\n");
	seq_puts(seq, "[1] = hwlro_agg_time_ctrl\n");
	seq_puts(seq, "[2] = hwlro_age_time_ctrl\n");
	seq_puts(seq, "[3] = hwlro_threshold_ctrl\n");
	seq_puts(seq, "[4] = hwlro_ring_enable_ctrl\n");
	seq_puts(seq, "[5] = hwlro_stats_enable_ctrl\n\n");

	if (MTK_HAS_CAPS(g_eth->soc->caps, MTK_NETSYS_V2)) {
		for (i = 1; i <= 8; i++)
			hw_lro_auto_tlb_dump_v2(seq, i);
	} else {
		/* Read valid entries of the auto-learn table */
		mtk_w32(g_eth, 0, MTK_FE_ALT_CF8);
		reg_val = mtk_r32(g_eth, MTK_FE_ALT_SEQ_CFC);

		seq_printf(seq,
			   "HW LRO Auto-learn Table: (MTK_FE_ALT_SEQ_CFC=0x%x)\n",
			   reg_val);

		for (i = 7; i >= 0; i--) {
			if (reg_val & (1 << i))
				hw_lro_auto_tlb_dump_v1(seq, i);
		}
	}

	/* Read the agg_time/age_time/agg_cnt of LRO rings */
	seq_puts(seq, "\nHW LRO Ring Settings\n");

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++) {
		reg_op1 = mtk_r32(g_eth, MTK_LRO_CTRL_DW1_CFG(i));
		reg_op2 = mtk_r32(g_eth, MTK_LRO_CTRL_DW2_CFG(i));
		reg_op3 = mtk_r32(g_eth, MTK_LRO_CTRL_DW3_CFG(i));
		reg_op4 = mtk_r32(g_eth, MTK_PDMA_LRO_CTRL_DW2);

		agg_cnt =
		    ((reg_op3 & 0x3) << 6) |
		    ((reg_op2 >> MTK_LRO_RING_AGG_CNT_L_OFFSET) & 0x3f);
		agg_time = (reg_op2 >> MTK_LRO_RING_AGG_TIME_OFFSET) & 0xffff;
		age_time =
		    ((reg_op2 & 0x3f) << 10) |
		    ((reg_op1 >> MTK_LRO_RING_AGE_TIME_L_OFFSET) & 0x3ff);
		seq_printf(seq,
			   "Ring[%d]: MAX_AGG_CNT=%d, AGG_TIME=%d, AGE_TIME=%d, Threshold=%d\n",
			   (MTK_HAS_CAPS(g_eth->soc->caps, MTK_NETSYS_V2))? i+3 : i,
			   agg_cnt, agg_time, age_time, reg_op4);
	}

	seq_puts(seq, "\n");

	return 0;
}

static int hw_lro_auto_tlb_open(struct inode *inode, struct file *file)
{
	return single_open(file, hw_lro_auto_tlb_read, NULL);
}

static const struct file_operations hw_lro_auto_tlb_fops = {
	.owner = THIS_MODULE,
	.open = hw_lro_auto_tlb_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = hw_lro_auto_tlb_write,
	.release = single_release
};
	
#if defined(CONFIG_TP_IMAGE)
static int phy_link_up_read(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%d\n", phy_link_up_flag);
	return 0;
}

static int phy_link_up_open(struct inode *inode, struct file *file)
{
	return single_open(file, phy_link_up_read, NULL);
}


static ssize_t phy_link_up_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
{
	char buf[32];	
	int flag = 0;
	int ret = 0;

	if (count > 32)
		count = 32;
	memset(buf, 0, 32);
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	/* determine interface name */

	ret = sscanf(buf, "%d", &flag);
	if(ret == 0)
	{
		printk("PhyLinkUpWrite error\n");
		return -1;
	}
	phy_link_up_flag = flag;
    return count;
}

static const struct file_operations phy_link_up_fops = {
	.owner = THIS_MODULE,
	.open = phy_link_up_open,
	.read = seq_read,
	.write = phy_link_up_write
};

static int wan_fc_status_read(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Current switch wan enable %d, flow control status %d, port %d\n", is_1g_wan, wan_fc_status, wan_fc_port);
	return 0;
}

static int wan_fc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, wan_fc_status_read, NULL);
}

static ssize_t wan_fc_status_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
{
	char buf[32];
	char *p_buf;
	int len = count;
	int arg0 = 0, arg1 = 0, arg2 = 0;
	char *p_token = NULL;
	char *p_delimiter = " \t";
	int ret;

	if (len >= sizeof(buf)) {
		pr_info("input handling fail!\n");
		len = sizeof(buf) - 1;
		return -1;
	}

	if (copy_from_user(buf, buffer, len))
		return -EFAULT;

	buf[len] = '\0';

	p_buf = buf;
	p_token = strsep(&p_buf, p_delimiter);
	if (!p_token)
		arg0 = 0;
	else
		ret = kstrtoint(p_token, 10, &arg0);

	if(arg0 == 0){
		is_1g_wan = arg0;
		wan_fc_status = arg1;
		wan_fc_port = arg2;
		return len;
	}
		
	p_token = strsep(&p_buf, p_delimiter);
	if (!p_token)
		arg1 = 0;
	else
		ret = kstrtoint(p_token, 10, &arg1);

	switch (arg1) {
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
		p_token = strsep(&p_buf, p_delimiter);
		if (!p_token)
			arg2 = 0;
		else
			ret = kstrtoint(p_token, 10, &arg2);
		break;
	default:
		arg1 = 0;
		arg2 = 0;
		pr_info("Currently, only 7531 switch is supported and only one port can be used as WAN.\n");
		pr_info("Usage:echo [enable] [status] [port] > /proc/mtketh/wan_fc_status \n\n");
		pr_info("enable:\n");
		pr_info("0		1g phy port not as wan port\n");
		pr_info("1		1g phy port as wan port\n");
		pr_info("status:\n");
		pr_info("0		Negotiate with the link partner\n");
		pr_info("1		disable tx flow control, enable rx flow control\n");
		pr_info("2		enable tx flow control, disable rx flow control\n");
		pr_info("3		disable trx flow control\n");
		pr_info("4		enable trx flow control\n");
		pr_info("port:\n");
		pr_info("0~4	depends on the actual port in use\n");
		break;
	}

	is_1g_wan = arg0;
	wan_fc_status = arg1;
	wan_fc_port = arg2;

	return len;
}

static const struct file_operations wan_fc_status_fops = {
	.owner = THIS_MODULE,
	.open = wan_fc_status_open,
	.read = seq_read,
	.write = wan_fc_status_write
};
	
static int lan_fc_status_read(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Current switch lan flow control status %d\n", lan_fc_status);
	return 0;
}

static int lan_fc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, lan_fc_status_read, NULL);
}

/* 
 * fn       static ssize_t lan_fc_status_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
 * brief    get lan flow control status from /proc/mtketh/lan_fc_status
 * note     After the WAN port is identified, other ports are considered to be LAN ports. 
 *          Therefore, this function does not obtain the ports of the LAN port     
 */
static ssize_t lan_fc_status_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
{
	char buf[32];	
	int arg = 0;
	int ret = 0;

	if (count > 32)
		count = 32;
	
	memset(buf, 0, 32);
	
	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	ret = sscanf(buf, "%d", &arg);
	
	if(ret == 0)
	{
		pr_notice("lan_fc_status_write error\n");
		return -1;
	}
	
	switch (arg) {
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
		break;
	default:
		arg = 0;
		pr_info("Currently, only 7531 switch is supported.\n");
		pr_info("Usage:echo [status] > /proc/mtketh/lan_fc_status \n\n");
		pr_info("status:\n");
		pr_info("0		Negotiate with the link partner\n");
		pr_info("1		disable tx flow control, enable rx flow control\n");
		pr_info("2		enable tx flow control, disable rx flow control\n");
		pr_info("3		disable trx flow control\n");
		pr_info("4		enable trx flow control\n");
		break;
	}
	
	lan_fc_status = arg;
	return count;
}

static const struct file_operations lan_fc_status_fops = {
	.owner = THIS_MODULE,
	.open = lan_fc_status_open,
	.read = seq_read,
	.write = lan_fc_status_write
};

static int gmac_fc_status_read(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Current gmac eth%d flow control status %d\n", gmac_id, gmac_fc_status);
	return 0;
}

static int gmac_fc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, gmac_fc_status_read, NULL);
}

/* 
 * fn       static ssize_t gmac_fc_status_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
 * brief    get gmac flow control status from /proc/mtketh/gmac_fc_status
 * note     current 2.5G wan gpy211 connect to eth1, we set gmc_id default value is 1 and gmac_fc_status is 3
 */
static ssize_t gmac_fc_status_write(struct file *file, const char __user *buffer, 
		      size_t count, loff_t *data)
{
	char buf[32];
	char *p_buf;
	int len = count;
	int arg0 = 0, arg1 = 0;
	char *p_token = NULL;
	char *p_delimiter = " \t";
	int ret;

	if (len >= sizeof(buf)) {
		pr_info("input handling fail!\n");
		len = sizeof(buf) - 1;
		return -1;
	}

	if (copy_from_user(buf, buffer, len))
		return -EFAULT;

	buf[len] = '\0';

	p_buf = buf;
	p_token = strsep(&p_buf, p_delimiter);
	if (!p_token)
		arg0 = 1;//If get value error, flow control for eth1 is set by default
	else
		ret = kstrtoint(p_token, 10, &arg0);

	p_token = strsep(&p_buf, p_delimiter);
	if (!p_token)
		arg1 = 0;//If get value error, flow control negotiate with the link partner
	else
		ret = kstrtoint(p_token, 10, &arg1);

	switch (arg1) {
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
		break;
	default:
		arg1 = 0;
		pr_info("Set flow control for GMAC, also need to down up interface(eth0 or eth1) to take effect\n");
		pr_info("Usage:echo [gmac_id] [status] > /proc/mtketh/gmac_fc_status \n\n");
		pr_info("gmac_id:\n");
		pr_info("0		\n");
		pr_info("1		\n");
		pr_info("status:\n");
		pr_info("0		Negotiate with the link partner\n");
		pr_info("1		enable tx flow control, disable rx flow control\n");
		pr_info("2		disable tx flow control, enable rx flow control\n");
		pr_info("3		disable trx flow control\n");
		pr_info("4		disable trx flow control\n");
		break;
	}

	gmac_id = arg0;
	gmac_fc_status = arg1;

	return len;
}

static const struct file_operations gmac_fc_status_fops = {
	.owner = THIS_MODULE,
	.open = gmac_fc_status_open,
	.read = seq_read,
	.write = gmac_fc_status_write
};

static struct proc_dir_entry *proc_gmac_fc_status;
static struct proc_dir_entry *proc_phy_link_up;
static struct proc_dir_entry *proc_wan_fc_status;
static struct proc_dir_entry *proc_lan_fc_status;
#endif

struct proc_dir_entry *proc_reg_dir;
static struct proc_dir_entry *proc_esw_cnt, *proc_dbg_regs;

int debug_proc_init(struct mtk_eth *eth)
{
	g_eth = eth;

	if (!proc_reg_dir)
		proc_reg_dir = proc_mkdir(PROCREG_DIR, NULL);

	proc_tx_ring =
	    proc_create(PROCREG_TXRING, 0, proc_reg_dir, &tx_ring_fops);
	if (!proc_tx_ring)
		pr_notice("!! FAIL to create %s PROC !!\n", PROCREG_TXRING);

	proc_rx_ring =
	    proc_create(PROCREG_RXRING, 0, proc_reg_dir, &rx_ring_fops);
	if (!proc_rx_ring)
		pr_notice("!! FAIL to create %s PROC !!\n", PROCREG_RXRING);

	proc_esw_cnt =
	    proc_create(PROCREG_ESW_CNT, 0, proc_reg_dir, &switch_count_fops);
	if (!proc_esw_cnt)
		pr_notice("!! FAIL to create %s PROC !!\n", PROCREG_ESW_CNT);

	proc_dbg_regs =
	    proc_create(PROCREG_DBG_REGS, 0, proc_reg_dir, &dbg_regs_fops);
	if (!proc_dbg_regs)
		pr_notice("!! FAIL to create %s PROC !!\n", PROCREG_DBG_REGS);

#if defined(CONFIG_TP_IMAGE)
	proc_phy_link_up =
	    proc_create(PROCREG_PHY_LINK_UP, 0, proc_reg_dir, &phy_link_up_fops);
	if (!proc_phy_link_up)
		pr_notice("!! FAIL to create %s PROC !!\n", PROCREG_PHY_LINK_UP);
	
	proc_wan_fc_status =
	    proc_create(PROC_WAN_FC_STS, 0, proc_reg_dir, &wan_fc_status_fops);
	if (!proc_wan_fc_status)
		pr_notice("!! FAIL to create %s PROC !!\n", PROC_WAN_FC_STS);

	proc_lan_fc_status =
	    proc_create(PROC_LAN_FC_STS, 0, proc_reg_dir, &lan_fc_status_fops);
	if (!proc_lan_fc_status)
		pr_notice("!! FAIL to create %s PROC !!\n", PROC_LAN_FC_STS);

	proc_gmac_fc_status =
	    proc_create(PROC_GMAC_FC_STS, 0, proc_reg_dir, &gmac_fc_status_fops);
	if (!proc_gmac_fc_status)
		pr_notice("!! FAIL to create %s PROC !!\n", PROC_GMAC_FC_STS);
#endif

	if (g_eth->hwlro) {
		proc_hw_lro_stats =
			proc_create(PROCREG_HW_LRO_STATS, 0, proc_reg_dir,
				    &hw_lro_stats_fops);
		if (!proc_hw_lro_stats)
			pr_info("!! FAIL to create %s PROC !!\n", PROCREG_HW_LRO_STATS);

		proc_hw_lro_auto_tlb =
			proc_create(PROCREG_HW_LRO_AUTO_TLB, 0, proc_reg_dir,
				    &hw_lro_auto_tlb_fops);
		if (!proc_hw_lro_auto_tlb)
			pr_info("!! FAIL to create %s PROC !!\n",
				PROCREG_HW_LRO_AUTO_TLB);
	}

	return 0;
}

void debug_proc_exit(void)
{
	if (proc_tx_ring)
		remove_proc_entry(PROCREG_TXRING, proc_reg_dir);
	if (proc_rx_ring)
		remove_proc_entry(PROCREG_RXRING, proc_reg_dir);

	if (proc_esw_cnt)
		remove_proc_entry(PROCREG_ESW_CNT, proc_reg_dir);

	if (proc_reg_dir)
		remove_proc_entry(PROCREG_DIR, 0);

	if (proc_dbg_regs)
		remove_proc_entry(PROCREG_DBG_REGS, proc_reg_dir);

#if defined(CONFIG_TP_IMAGE)
	if (proc_phy_link_up)
		remove_proc_entry(PROCREG_PHY_LINK_UP, proc_reg_dir);
	
	if (proc_wan_fc_status)
		remove_proc_entry(PROC_WAN_FC_STS, proc_reg_dir);

	if (proc_lan_fc_status)
		remove_proc_entry(PROC_LAN_FC_STS, proc_reg_dir);

	if (proc_gmac_fc_status)
		remove_proc_entry(PROC_GMAC_FC_STS, proc_reg_dir);
#endif

	if (g_eth->hwlro) {
		if (proc_hw_lro_stats)
			remove_proc_entry(PROCREG_HW_LRO_STATS, proc_reg_dir);

		if (proc_hw_lro_auto_tlb)
			remove_proc_entry(PROCREG_HW_LRO_AUTO_TLB, proc_reg_dir);
	}
}

