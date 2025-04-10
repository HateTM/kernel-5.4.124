/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author:
 *  Shaocheng.Wang <shaocheng.wang@mediatek.com>
 *  Chunfeng.Yun <chunfeng.yun@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/kobject.h>

#include "../core/usb.h"
#include "xhci-mtk.h"

static int t_test_j(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_k(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_se0(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_packet(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_suspend(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_resume(struct xhci_hcd_mtk *mtk, int argc, char **argv);
static int t_test_get_device_descriptor(struct xhci_hcd_mtk *mtk,
				int argc, char **argv);
static int t_test_enumerate_bus(struct xhci_hcd_mtk *mtk,
				int argc, char **argv);
static int t_power_u1u2(struct xhci_hcd_mtk *mtk, int argc, char **argv);

#define PORT_PLS_VALUE(p) ((p >> 5) & 0xf)

#define MAX_NAME_SIZE 32
#define MAX_ARG_SIZE 4

struct hqa_test_cmd {
	char name[MAX_NAME_SIZE];
	int (*cb_func)(struct xhci_hcd_mtk *mtk, int argc, char **argv);
	char *discription;
};


struct hqa_test_cmd xhci_mtk_hqa_cmds[] = {
	{"test.j", &t_test_j, "Test_J"},
	{"test.k", &t_test_k, "Test_K"},
	{"test.se0", &t_test_se0, "Test_SE0_NAK"},
	{"test.packet", &t_test_packet, "Test_PACKET"},
	{"test.suspend", &t_test_suspend, "Port Suspend"},
	{"test.resume", &t_test_resume, "Port Resume"},
	{"test.enumbus", &t_test_enumerate_bus, "Enumerate Bus"},
	{"test.getdesc", &t_test_get_device_descriptor,
				"Get Device Discriptor"},
	{"pm.u1u2", &t_power_u1u2, "Port U1,U2"},
	{"", NULL, ""},
};


int call_hqa_func(struct xhci_hcd_mtk *mtk, char *buf)
{
	struct hqa_test_cmd *hqa;
	struct usb_hcd *hcd = mtk->hcd;
	char *argv[MAX_ARG_SIZE];
	int argc;
	int i;

	argc = 0;
	do {
		argv[argc] = strsep(&buf, " ");
		xhci_err(hcd_to_xhci(hcd), "[%d] %s\r\n", argc, argv[argc]);
		argc++;
	} while (buf);

	for (i = 0; i < ARRAY_SIZE(xhci_mtk_hqa_cmds); i++) {
		hqa = &xhci_mtk_hqa_cmds[i];
		if ((!strcmp(hqa->name, argv[0])) && (hqa->cb_func != NULL))
			return hqa->cb_func(mtk, argc, argv);
	}

	return -1;
}

static int test_mode_enter(struct xhci_hcd_mtk *mtk,
				u32 port_id, u32 test_value)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 __iomem *addr;
	u32 temp;

	if (mtk->test_mode == 0) {
		xhci_stop(hcd);
		xhci_halt(xhci);
	}

	addr = &xhci->op_regs->port_power_base +
			NUM_PORT_REGS * ((port_id - 1) & 0xff);
	temp = readl(addr);
	temp &= ~(0xf << 28);
	temp |= (test_value << 28);
	writel(temp, addr);
	mtk->test_mode = 1;

	return 0;
}

static int test_mode_exit(struct xhci_hcd_mtk *mtk)
{
	/* struct usb_hcd *hcd = mtk->hcd; */
	/* struct xhci_hcd *xhci = hcd_to_xhci(hcd); */

	if (mtk->test_mode == 1) {
#if 0
		xhci_reset(xhci);
		/*reinitIP(&pdev->dev);*/

		if (!usb_hcd_is_primary_hcd(test_hcd))
			secondary_hcd = test_hcd;
		else
			secondary_hcd = xhci->shared_hcd;

		retval = xhci_init(test_hcd->primary_hcd);
		if (retval)
			return retval;

		retval = xhci_run(test_hcd->primary_hcd);
		if (!retval)
			retval = xhci_run(secondary_hcd);

		/*enableXhciAllPortPower(xhci);*/
#endif
		mtk->test_mode = 0;
	}
	return 0;
}

static int t_test_j(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	long port_id;
	u32 test_value;

	port_id = 2;
	test_value = 1;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);
	test_mode_enter(mtk, port_id, test_value);

	return 0;
}

static int t_test_k(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	long port_id;
	u32 test_value;

	port_id = 2;
	test_value = 2;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);
	test_mode_enter(mtk, port_id, test_value);

	return 0;
}

static int t_test_se0(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	long port_id;
	u32 test_value;

	port_id = 2;
	test_value = 3;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%ld\n", __func__, port_id);
	test_mode_enter(mtk, port_id, test_value);

	return 0;
}

static int t_test_packet(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	long port_id;
	u32 test_value;

	port_id = 2;
	test_value = 4;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%ld\n", __func__, port_id);
	test_mode_enter(mtk, port_id, test_value);

	return 0;
}

/* only for u3 ports, valid values are 1, 2, ...*/
static int t_power_u1u2(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 __iomem *addr;
	u32 temp;
	int port_id;
	int retval = 0;
	int u_num = 1;
	int u1_val = 1;
	int u2_val = 0;

	port_id = 1; /* first u3port by default */

	if (argc > 1 && kstrtoint(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	if (argc > 2 && kstrtoint(argv[2], 10, &u_num))
		xhci_err(xhci, "mu3h %s get u_num failed\n", __func__);

	if (argc > 3 && kstrtoint(argv[3], 10, &u1_val))
		xhci_err(xhci, "mu3h %s get u1_val failed\n", __func__);

	if (argc > 4 && kstrtoint(argv[4], 10, &u2_val))
		xhci_err(xhci, "mu3h %s get u2_val failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d, u_num%d, u1_val%d, u2_val%d\n",
		__func__, (int)port_id, u_num, u1_val, u2_val);

	if (mtk->test_mode == 1) {
		xhci_err(xhci, "please suspend port first\n");
		return -1;
	}

	xhci_err(xhci, "%s: stop port polling\n", __func__);
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	del_timer_sync(&hcd->rh_timer);
	clear_bit(HCD_FLAG_POLL_RH, &xhci->shared_hcd->flags);
	del_timer_sync(&xhci->shared_hcd->rh_timer);
	clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	clear_bit(HCD_FLAG_HW_ACCESSIBLE, &xhci->shared_hcd->flags);

	addr = &xhci->op_regs->port_power_base +
			NUM_PORT_REGS * ((port_id - 1) & 0xff);

	temp = readl(addr);
	if (u_num == 1) {
		temp &= ~PORT_U1_TIMEOUT_MASK;
		temp |= PORT_U1_TIMEOUT(u1_val);
	} else if (u_num == 2) {
		temp &= ~PORT_U2_TIMEOUT_MASK;
		temp |= PORT_U2_TIMEOUT(u2_val);
	} else if (u_num == 3) {
		temp &= ~(PORT_U1_TIMEOUT_MASK | PORT_U2_TIMEOUT_MASK);
		temp |= PORT_U1_TIMEOUT(u1_val) | PORT_U2_TIMEOUT(u2_val);
	}

	writel(temp, addr);

	return retval;
}


static int t_test_suspend(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 __iomem *addr;
	u32 temp;
	long port_id;

	port_id = 2;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);

	xhci_err(xhci, "%s: stop port polling\n", __func__);
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	del_timer_sync(&hcd->rh_timer);
	clear_bit(HCD_FLAG_POLL_RH, &xhci->shared_hcd->flags);
	del_timer_sync(&xhci->shared_hcd->rh_timer);
	clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	clear_bit(HCD_FLAG_HW_ACCESSIBLE, &xhci->shared_hcd->flags);

	temp = readl(&xhci->ir_set->irq_pending);
	writel(ER_IRQ_DISABLE(temp), &xhci->ir_set->irq_pending);

	if (mtk->test_mode == 1)
		test_mode_exit(mtk);

	/* set PLS = 3 */
	addr = &xhci->op_regs->port_status_base +
			NUM_PORT_REGS*((port_id - 1) & 0xff);

	temp = readl(addr);
	temp = xhci_port_state_to_neutral(temp);
	temp = (temp & ~(0xf << 5));
	temp = (temp | (3 << 5) | PORT_LINK_STROBE);
	writel(temp, addr);
	xhci_handshake(addr, (0xf << 5), (3 << 5), 30*1000);

	temp = readl(addr);
	if (PORT_PLS_VALUE(temp) != 3)
		xhci_err(xhci, "port not enter suspend state\n");
	else
		xhci_err(xhci, "port enter suspend state\n");

	return 0;
}

static int t_test_resume(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 __iomem *addr;
	u32 temp;
	long port_id;
	int retval = 0;

	port_id = 2;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);

	if (mtk->test_mode == 1) {
		xhci_err(xhci, "please suspend port first\n");
		return -1;
	}
	addr = &xhci->op_regs->port_status_base +
			NUM_PORT_REGS * ((port_id - 1) & 0xff);

	temp = readl(addr);
	if (PORT_PLS_VALUE(temp) != 3) {
		xhci_err(xhci, "port not in suspend state, please suspend port first\n");
		retval = -1;
	} else {
		temp = xhci_port_state_to_neutral(temp);
		temp = (temp & ~(0xf << 5));
		temp = (temp | (15 << 5) | PORT_LINK_STROBE);
		writel(temp, addr);
		mdelay(20);

		temp = readl(addr);
		temp = xhci_port_state_to_neutral(temp);
		temp = (temp & ~(0xf << 5));
		temp = (temp | PORT_LINK_STROBE);
		writel(temp, addr);

		xhci_handshake(addr, (0xf << 5), (0 << 5), 100*1000);
		temp = readl(addr);
		if (PORT_PLS_VALUE(temp) != 0) {
			xhci_err(xhci, "rusume fail,%x\n",
				PORT_PLS_VALUE(temp));
			retval = -1;
		} else {
			xhci_err(xhci, "port resume ok\n");
		}
	}

	return retval;
}

static int t_test_enumerate_bus(struct xhci_hcd_mtk *mtk, int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct usb_device *usb2_rh;
	struct usb_device *udev;
	long port_id;
	u32 retval;

	port_id = 2;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);

	if (mtk->test_mode == 1) {
		test_mode_exit(mtk);
		return 0;
	}

	usb2_rh = hcd->self.root_hub;
	udev = usb_hub_find_child(usb2_rh, port_id - 1);

	if (udev != NULL) {
		retval = usb_reset_device(udev);
		if (retval) {
			xhci_err(xhci, "ERROR: enumerate bus fail!\n");
			return -1;
		}
	} else {
		xhci_err(xhci, "ERROR: Device does not exist!\n");
		return -1;
	}

	return 0;
}
static int t_test_get_device_descriptor(struct xhci_hcd_mtk *mtk,
				int argc, char **argv)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct usb_device *usb2_rh;
	struct usb_device *udev;
	long port_id;
	u32 retval = 0;

	port_id = 2;

	if (argc > 1 && kstrtol(argv[1], 10, &port_id))
		xhci_err(xhci, "mu3h %s get port-id failed\n", __func__);

	xhci_err(xhci, "mu3h %s test port%d\n", __func__, (int)port_id);

	if (mtk->test_mode == 1) {
		test_mode_exit(mtk);
		msleep(2000);
	}

	usb2_rh = hcd->self.root_hub;

	udev = usb_hub_find_child(usb2_rh, port_id - 1);

	if (udev != NULL) {
		retval = usb_get_device_descriptor(udev, USB_DT_DEVICE_SIZE);
		if (retval != sizeof(udev->descriptor)) {
			xhci_err(xhci, "ERROR: get device descriptor fail!\n");
			return -1;
		}
	} else {
		xhci_err(xhci, "ERROR: Device does not exist!\n");
		return -1;
	}

	return 0;
}

static ssize_t mu3h_hqa_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int len = 0;
	int bufLen = PAGE_SIZE;
	struct hqa_test_cmd *hqa;
	int i;

	len += snprintf(buf+len, bufLen-len, "info:\n");
	len += snprintf(buf+len, bufLen-len,
			"\techo -n item port-id > hqa\n");
	len += snprintf(buf+len, bufLen-len,
			"\tport-id : based on number of usb3-port, e.g.\n");
	len += snprintf(buf+len, bufLen-len,
			"\t\txHCI with 1 u3p, 2 u2p: 1st u2p-id is 2(1+1), 2nd is 3\n");
	len += snprintf(buf+len, bufLen-len, "items:\n");

	for (i = 0; i < ARRAY_SIZE(xhci_mtk_hqa_cmds); i++) {
		hqa = &xhci_mtk_hqa_cmds[i];
		len += snprintf(buf+len, bufLen-len,
				"\t%s: %s\n", hqa->name, hqa->discription);
	}

	return len;
}

static ssize_t mu3h_hqa_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct xhci_hcd_mtk *mtk = dev_get_drvdata(dev);
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	int retval;

	retval = call_hqa_func(mtk, (char *)buf);
	if (retval < 0) {
		xhci_err(xhci, "mu3h cli fail\n");
		return -1;
	}

	return count;
}

static DEVICE_ATTR(hqa, 0664, mu3h_hqa_show, mu3h_hqa_store);

#define REGS_LIMIT_XHCI 0x1000
#define REGS_LIMIT_MU3D 0x3000
#define REGS_LIMIT_IPPC 0x100
#define REGS_LIMIT_PHYS 0x4000

#define REGS_XHCI_OFFSET 0x0000
#define REGS_MU3D_OFFSET 0x1000
#define REGS_IPPC_OFFSET 0x3e00
#define REGS_PHY_OFFSET  0xA20000

static ssize_t ssusb_reg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = -EINVAL;

	ret = sprintf(buf, "SSUSB register operation interface help info.\n"
		"  rx - read xhci reg: offset [len]\n"
		"  rm - read mu3d reg: offset [len]\n"
		"  ri - read ippc reg: offset [len]\n"
		"  rp - read phy reg: offset [len]\n"
		"  wx - write xhci reg: offset value\n"
		"  wm - write mu3d reg: offset value\n"
		"  wi - write ippc reg: offset value\n"
		"  wp - write phy reg: offset value\n"
		"  sx - set xhci mac reg bits: offset bit_start mask value\n"
		"  sm - set mu3d mac reg bits: offset bit_start mask value\n"
		"  si - set ippc reg bits: offset bit_start mask value\n"
		"  sp - set phy reg bits: offset bit_start mask value\n"
		"  px - print xhci mac reg bits: offset bit_start mask\n"
		"  pm - print mu3d mac reg bits: offset bit_start mask\n"
		"  pi - print ippc reg bits: offset bit_start mask\n"
		"  pp - print phy reg bits: offset bit_start mask\n"
		"  NOTE: numbers should be HEX, except bit_star(DEC)\n");

	return ret;
}

/* base address: return value; limit is put into @limit */
static void __iomem *get_reg_base_limit(struct xhci_hcd_mtk *mtk,
		const char *buf, u32 *limit)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	void __iomem *base;
	u32 len = 0;

	switch (buf[1]) {
	case 'x':
		base = mtk->ip_base + REGS_XHCI_OFFSET;
		len = REGS_LIMIT_XHCI;
		xhci_info(xhci, "xhci's reg:\n");
		break;
	case 'm':
		base = mtk->ip_base + REGS_MU3D_OFFSET;
		len = REGS_LIMIT_MU3D;
		xhci_info(xhci, "mu3d's reg:\n");
		break;
	case 'i':
		base = mtk->ip_base + REGS_IPPC_OFFSET;
		len = REGS_LIMIT_IPPC;
		xhci_info(xhci, "ippc's reg:\n");
		break;
	case 'p':
		base = mtk->ip_base + REGS_PHY_OFFSET;
		len = REGS_LIMIT_PHYS;
		xhci_info(xhci, "phy's reg:\n");
		break;
	default:
		base = NULL;
	}

	*limit = len;

	return base;
}

static void ssusb_write_reg(struct xhci_hcd_mtk *mtk, const char *buf)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	void __iomem *base;
	u32 offset = 0;
	u32 value = 0;
	u32 old_val = 0;
	u32 limit = 0;
	u32 param;

	param = sscanf(buf, "%*s 0x%x 0x%x", &offset, &value);
	xhci_info(xhci, "params-%d (offset: %#x, value: %#x)\n",
				param, offset, value);

	base = get_reg_base_limit(mtk, buf, &limit);
	if (!base || (param != 2)) {
		xhci_err(xhci, "params are invalid!\n");
		return;
	}

	offset &= ~0x3;  /* 4-bytes align */
	if (offset >= limit) {
		xhci_err(xhci, "reg's offset overrun!\n");
		return;
	}
	old_val = readl(base + offset);
	writel(value, base + offset);
	xhci_info(xhci, "0x%8.8x : 0x%8.8x --> 0x%8.8x\n", offset, old_val,
		readl(base + offset));
}

static void read_single_reg(struct xhci_hcd_mtk *mtk,
			void __iomem *base, u32 offset, u32 limit)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 value;

	offset &= ~0x3;  /* 4-bytes align */
	if (offset >= limit) {
		xhci_err(xhci, "reg's offset overrun!\n");
		return;
	}
	value = readl(base + offset);
	xhci_err(xhci, "0x%8.8x : 0x%8.8x\n", offset, value);
}

static void read_multi_regs(struct xhci_hcd_mtk *mtk,
	void __iomem *base, u32 offset, u32 len, u32 limit)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	int i;

	/* at least 4 ints */
	offset &= ~0xF;
	len = (len + 0x3) & ~0x3;

	if (offset + len > limit) {
		xhci_err(xhci, "reg's offset overrun!\n");
		return;
	}

	len >>= 2;
	xhci_info(xhci, "read regs [%#x, %#x)\n", offset, offset + (len << 4));
	for (i = 0; i < len; i++) {
		xhci_err(xhci, "0x%8.8x : 0x%8.8x 0x%8.8x 0x%8.8x 0x%8.8x\n",
			offset, readl(base + offset),
			readl(base + offset + 0x4),
			readl(base + offset + 0x8),
			readl(base + offset + 0xc));
		offset += 0x10;
	}
}

static void ssusb_read_regs(struct xhci_hcd_mtk *mtk, const char *buf)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	void __iomem *base;
	u32 offset = 0;
	u32 len = 0;
	u32 limit = 0;
	u32 param;

	param = sscanf(buf, "%*s 0x%x 0x%x", &offset, &len);
	xhci_info(xhci, "params-%d (offset: %#x, len: %#x)\n",
				param, offset, len);

	base = get_reg_base_limit(mtk, buf, &limit);
	if (!base || !param) {
		xhci_err(xhci, "params are invalid!\n");
		return;
	}

	if (param == 1)
		read_single_reg(mtk, base, offset, limit);
	else
		read_multi_regs(mtk, base, offset, len, limit);
}

static void ssusb_set_reg_bits(struct xhci_hcd_mtk *mtk, const char *buf)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	void __iomem *base;
	u32 offset = 0;
	u32 bit_start = 0;
	u32 mask = 0;
	u32 value = 0;
	u32 old_val = 0;
	u32 new_val = 0;
	u32 limit = 0;
	u32 param;

	param = sscanf(buf, "%*s 0x%x %d 0x%x 0x%x",
			&offset, &bit_start, &mask, &value);
	xhci_info(xhci, "params-%d (offset:%#x,bit_start:%d,mask:%#x,value:%#x)\n",
		param, offset, bit_start, mask, value);

	base = get_reg_base_limit(mtk, buf, &limit);
	if (!base || (param != 4) || (bit_start > 32)) {
		xhci_err(xhci, "params are invalid!\n");
		return;
	}

	offset &= ~0x3;  /* 4-bytes align */
	if (offset >= limit) {
		xhci_err(xhci, "reg's offset overrun!\n");
		return;
	}
	old_val = readl(base + offset);
	new_val = old_val;
	new_val &= ~(mask << bit_start);
	new_val |= (value << bit_start);
	writel(new_val, base + offset);
	xhci_info(xhci, "0x%8.8x : 0x%8.8x --> 0x%8.8x\n", offset, old_val,
		readl(base + offset));
}


static void ssusb_print_reg_bits(struct xhci_hcd_mtk *mtk, const char *buf)
{
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	void __iomem *base;
	u32 offset = 0;
	u32 bit_start = 0;
	u32 mask = 0;
	u32 old_val = 0;
	u32 new_val = 0;
	u32 limit = 0;
	u32 param;

	param = sscanf(buf, "%*s 0x%x %d 0x%x", &offset, &bit_start, &mask);
	xhci_info(xhci, "params-%d (offset: %#x, bit_start: %d, mask: %#x)\n",
		param, offset, bit_start, mask);

	base = get_reg_base_limit(mtk, buf, &limit);
	if (!base || (param != 3) || (bit_start > 32)) {
		xhci_err(xhci, "params are invalid!\n");
		return;
	}

	offset &= ~0x3;  /* 4-bytes align */
	if (offset >= limit) {
		xhci_err(xhci, "reg's offset overrun!\n");
		return;
	}

	old_val = readl(base + offset);
	new_val = old_val;
	new_val >>= bit_start;
	new_val &= mask;
	xhci_info(xhci, "0x%8.8x : 0x%8.8x (0x%x)\n", offset, old_val, new_val);
}


static ssize_t
ssusb_reg_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t n)
{
	struct xhci_hcd_mtk *mtk = dev_get_drvdata(dev);
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	xhci_info(xhci, " cmd: %s\n", buf);

	switch (buf[0]) {
	case 'w':
		ssusb_write_reg(mtk, buf);
		break;
	case 'r':
		ssusb_read_regs(mtk, buf);
		break;
	case 's':
		ssusb_set_reg_bits(mtk, buf);
		break;
	case 'p':
		ssusb_print_reg_bits(mtk, buf);
		break;
	default:
		xhci_err(xhci, "No such cmd\n");
	}

	return n;
}

static DEVICE_ATTR(reg, 0664, ssusb_reg_show, ssusb_reg_store);

static struct device_attribute *mu3h_hqa_attr_list[] = {
	&dev_attr_hqa,
	&dev_attr_reg,
};

static void ssusb_remap_ip_regs(struct device *dev)
{
	struct xhci_hcd_mtk *mtk = dev_get_drvdata(dev);
	struct usb_hcd *hcd = mtk->hcd;
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	mtk->ip_base = ioremap(hcd->rsrc_start, 0xb00000);//0xa30000
	if (!mtk->ip_base)
		xhci_err(xhci, "could not ioremap regs\n");
}

int mu3h_hqa_create_attr(struct device *dev)
{
	int idx, err = 0;
	int num = ARRAY_SIZE(mu3h_hqa_attr_list);

	if (dev == NULL)
		return -EINVAL;

	ssusb_remap_ip_regs(dev);

	for (idx = 0; idx < num; idx++) {
		err = device_create_file(dev, mu3h_hqa_attr_list[idx]);
		if (err)
			break;
	}

	return err;
}

void mu3h_hqa_remove_attr(struct device *dev)
{
	int idx;
	int num = ARRAY_SIZE(mu3h_hqa_attr_list);

	for (idx = 0; idx < num; idx++)
		device_remove_file(dev, mu3h_hqa_attr_list[idx]);
}
