// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	Userspace interface
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/netpoll.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>
#include <linux/slab.h>
#include <net/dsa.h>
#include <net/sock.h>
#include <linux/if_vlan.h>
#include <net/switchdev.h>
#include <net/net_namespace.h>
#include <linux/if_macvlan.h>

#include "br_private.h"

#if defined(CONFIG_TP_IMAGE) && defined(CONFIG_NET_DSA_MT7530)
#include "../dsa/dsa_priv.h"
#endif

#if defined(CONFIG_TP_IMAGE) || defined(CONFIG_TP_HYFI_BRIDGE)
/* Hook for external forwarding logic */
br_port_dev_get_hook_t __rcu *br_port_dev_get_hook __read_mostly;
EXPORT_SYMBOL_GPL(br_port_dev_get_hook);
#endif

#if defined(CONFIG_TP_IMAGE) && defined(CONFIG_NET_DSA_MT7530)

struct net_device * get_real_dsa_slave(struct net_device *dev)
{
	struct net_device *slave = NULL;
	
	if (dsa_slave_dev_check(dev))
		return dev;

	if (is_vlan_dev(dev))
	{
		slave = vlan_dev_real_dev(dev);
	}
#if defined(CONFIG_X_TP_VLAN)
	else if (netif_is_macvlan(dev))
	{
		slave = macvlan_dev_real_dev(dev);
	}
#endif
	else
	{
		return NULL;
	}

	if (dsa_slave_dev_check(slave))
	{
		return slave;
	}
	
	return NULL;
}

bool is_offlearning_dsa_slave_port(struct net_device * dev)
{
	struct net_device *slave = NULL;

	if (is_vlan_dev(dev))
	{
		slave = vlan_dev_real_dev(dev);
	}
#if defined(CONFIG_X_TP_VLAN)
	else if (netif_is_macvlan(dev))
	{
		slave = macvlan_dev_real_dev(dev);
	}
#endif

	if (!slave)
		return false;

	return (dsa_slave_dev_check(slave));
}

void port_set_sa_learning(struct dsa_port *dp, bool offlearning)
{
	struct dsa_switch *ds = dp->ds;

	if (ds->ops && ds->ops->port_sa_state_set)
		ds->ops->port_sa_state_set(ds, dp->index, offlearning);
}

void dsa_port_set_sa(struct net_device * dev, bool offlearning_attr, bool new_change_offlearning_port)
{
	struct net_device *slave = get_real_dsa_slave(dev);
	struct dsa_port *dp;
	struct dsa_slave_priv *p;
	int old;

	if (!slave)
		return;

	dp = dsa_slave_to_port(slave);

	old = dp->offlearning_vif_count;

	if (offlearning_attr)
	{
		dp->offlearning_vif_count += (new_change_offlearning_port ? 1 : 0);
	}
	else
	{
		dp->offlearning_vif_count -= (new_change_offlearning_port ? 1 : 0);
	}

	if (offlearning_attr)
	{
		if (0 == old)
			port_set_sa_learning(dp, 1);
	}
	else
	{
		if (0 == dp->offlearning_vif_count)
			port_set_sa_learning(dp, 0);
	}
}

void bridge_set_sa(struct net_bridge * br, bool offlearning)
{
	struct net_bridge_port *p;

	list_for_each_entry(p, &br->port_list, list) {
		dsa_port_set_sa(p->dev, offlearning, false);    /* 重置网桥上各端口的offlearning属性 */
	}
}

void bridge_port_set_sa(struct net_bridge *br, struct net_device * dev, bool join)
{
	struct net_device *slave;
	bool offlearning_port = is_offlearning_dsa_slave_port(dev);

	if (join)
	{
		if (offlearning_port)
		{
			if (0 == br->offlearning_port_count)
			{
				bridge_set_sa(br, true);
			}

			br->offlearning_port_count ++;
		}

		if (br->offlearning_port_count)
		{
			dsa_port_set_sa(dev, true, offlearning_port);
		}
	}
	else
	{
		if (offlearning_port)
		{
			br->offlearning_port_count --;

			if (0 == br->offlearning_port_count)
			{
				bridge_set_sa(br, false);
			}

			dsa_port_set_sa(dev, false, offlearning_port);
		}
		else
		{
			if (br->offlearning_port_count)
			{
				dsa_port_set_sa(dev, false, false);
			}
		}
	}
}
#endif

/*
 * Determine initial path cost based on speed.
 * using recommendations from 802.1d standard
 *
 * Since driver might sleep need to not be holding any locks.
 */
static int port_cost(struct net_device *dev)
{
	struct ethtool_link_ksettings ecmd;

	if (!__ethtool_get_link_ksettings(dev, &ecmd)) {
		switch (ecmd.base.speed) {
		case SPEED_10000:
			return 2;
		case SPEED_1000:
			return 4;
		case SPEED_100:
			return 19;
		case SPEED_10:
			return 100;
		}
	}

	/* Old silly heuristics based on name */
	if (!strncmp(dev->name, "lec", 3))
		return 7;

	if (!strncmp(dev->name, "plip", 4))
		return 2500;

	return 100;	/* assume old 10Mbps */
}


/* Check for port carrier transitions. */
void br_port_carrier_check(struct net_bridge_port *p, bool *notified)
{
	struct net_device *dev = p->dev;
	struct net_bridge *br = p->br;

	if (!(p->flags & BR_ADMIN_COST) &&
	    netif_running(dev) && netif_oper_up(dev))
		p->path_cost = port_cost(dev);

	*notified = false;
	if (!netif_running(br->dev))
		return;

	spin_lock_bh(&br->lock);
	if (netif_running(dev) && netif_oper_up(dev)) {
		if (p->state == BR_STATE_DISABLED) {
			br_stp_enable_port(p);
			*notified = true;
		}
	} else {
		if (p->state != BR_STATE_DISABLED) {
			br_stp_disable_port(p);
			*notified = true;
		}
	}
	spin_unlock_bh(&br->lock);
}

static void br_port_set_promisc(struct net_bridge_port *p)
{
	int err = 0;

	if (br_promisc_port(p))
		return;

	err = dev_set_promiscuity(p->dev, 1);
	if (err)
		return;

	br_fdb_unsync_static(p->br, p);
	p->flags |= BR_PROMISC;
}

static void br_port_clear_promisc(struct net_bridge_port *p)
{
	int err;

	/* Check if the port is already non-promisc or if it doesn't
	 * support UNICAST filtering.  Without unicast filtering support
	 * we'll end up re-enabling promisc mode anyway, so just check for
	 * it here.
	 */
	if (!br_promisc_port(p) || !(p->dev->priv_flags & IFF_UNICAST_FLT))
		return;

	/* Since we'll be clearing the promisc mode, program the port
	 * first so that we don't have interruption in traffic.
	 */
	err = br_fdb_sync_static(p->br, p);
	if (err)
		return;

	dev_set_promiscuity(p->dev, -1);
	p->flags &= ~BR_PROMISC;
}

/* When a port is added or removed or when certain port flags
 * change, this function is called to automatically manage
 * promiscuity setting of all the bridge ports.  We are always called
 * under RTNL so can skip using rcu primitives.
 */
void br_manage_promisc(struct net_bridge *br)
{
	struct net_bridge_port *p;
	bool set_all = false;

	/* If vlan filtering is disabled or bridge interface is placed
	 * into promiscuous mode, place all ports in promiscuous mode.
	 */
	if ((br->dev->flags & IFF_PROMISC) || !br_vlan_enabled(br->dev))
		set_all = true;

	list_for_each_entry(p, &br->port_list, list) {
		if (set_all) {
			br_port_set_promisc(p);
		} else {
			/* If the number of auto-ports is <= 1, then all other
			 * ports will have their output configuration
			 * statically specified through fdbs.  Since ingress
			 * on the auto-port becomes forwarding/egress to other
			 * ports and egress configuration is statically known,
			 * we can say that ingress configuration of the
			 * auto-port is also statically known.
			 * This lets us disable promiscuous mode and write
			 * this config to hw.
			 */
			if (br->auto_cnt == 0 ||
			    (br->auto_cnt == 1 && br_auto_port(p)))
				br_port_clear_promisc(p);
			else
				br_port_set_promisc(p);
		}
	}
}

int nbp_backup_change(struct net_bridge_port *p,
		      struct net_device *backup_dev)
{
	struct net_bridge_port *old_backup = rtnl_dereference(p->backup_port);
	struct net_bridge_port *backup_p = NULL;

	ASSERT_RTNL();

	if (backup_dev) {
		if (!netif_is_bridge_port(backup_dev))
			return -ENOENT;

		backup_p = br_port_get_rtnl(backup_dev);
		if (backup_p->br != p->br)
			return -EINVAL;
	}

	if (p == backup_p)
		return -EINVAL;

	if (old_backup == backup_p)
		return 0;

	/* if the backup link is already set, clear it */
	if (old_backup)
		old_backup->backup_redirected_cnt--;

	if (backup_p)
		backup_p->backup_redirected_cnt++;
	rcu_assign_pointer(p->backup_port, backup_p);

	return 0;
}

static void nbp_backup_clear(struct net_bridge_port *p)
{
	nbp_backup_change(p, NULL);
	if (p->backup_redirected_cnt) {
		struct net_bridge_port *cur_p;

		list_for_each_entry(cur_p, &p->br->port_list, list) {
			struct net_bridge_port *backup_p;

			backup_p = rtnl_dereference(cur_p->backup_port);
			if (backup_p == p)
				nbp_backup_change(cur_p, NULL);
		}
	}

	WARN_ON(rcu_access_pointer(p->backup_port) || p->backup_redirected_cnt);
}

static void nbp_update_port_count(struct net_bridge *br)
{
	struct net_bridge_port *p;
	u32 cnt = 0;

	list_for_each_entry(p, &br->port_list, list) {
		if (br_auto_port(p))
			cnt++;
	}
	if (br->auto_cnt != cnt) {
		br->auto_cnt = cnt;
		br_manage_promisc(br);
	}
}

static void nbp_delete_promisc(struct net_bridge_port *p)
{
	/* If port is currently promiscuous, unset promiscuity.
	 * Otherwise, it is a static port so remove all addresses
	 * from it.
	 */
	dev_set_allmulti(p->dev, -1);
	if (br_promisc_port(p))
		dev_set_promiscuity(p->dev, -1);
	else
		br_fdb_unsync_static(p->br, p);
}

static void release_nbp(struct kobject *kobj)
{
	struct net_bridge_port *p
		= container_of(kobj, struct net_bridge_port, kobj);
	kfree(p);
}

static void brport_get_ownership(struct kobject *kobj, kuid_t *uid, kgid_t *gid)
{
	struct net_bridge_port *p = kobj_to_brport(kobj);

	net_ns_get_ownership(dev_net(p->dev), uid, gid);
}

static struct kobj_type brport_ktype = {
#ifdef CONFIG_SYSFS
	.sysfs_ops = &brport_sysfs_ops,
#endif
	.release = release_nbp,
	.get_ownership = brport_get_ownership,
};

static void destroy_nbp(struct net_bridge_port *p)
{
	struct net_device *dev = p->dev;

	p->br = NULL;
	p->dev = NULL;
	dev_put(dev);

	kobject_put(&p->kobj);
}

static void destroy_nbp_rcu(struct rcu_head *head)
{
	struct net_bridge_port *p =
			container_of(head, struct net_bridge_port, rcu);
	destroy_nbp(p);
}

static unsigned get_max_headroom(struct net_bridge *br)
{
	unsigned max_headroom = 0;
	struct net_bridge_port *p;

	list_for_each_entry(p, &br->port_list, list) {
		unsigned dev_headroom = netdev_get_fwd_headroom(p->dev);

		if (dev_headroom > max_headroom)
			max_headroom = dev_headroom;
	}

	return max_headroom;
}

static void update_headroom(struct net_bridge *br, int new_hr)
{
	struct net_bridge_port *p;

	list_for_each_entry(p, &br->port_list, list)
		netdev_set_rx_headroom(p->dev, new_hr);

	br->dev->needed_headroom = new_hr;
}

/* Delete port(interface) from bridge is done in two steps.
 * via RCU. First step, marks device as down. That deletes
 * all the timers and stops new packets from flowing through.
 *
 * Final cleanup doesn't occur until after all CPU's finished
 * processing packets.
 *
 * Protected from multiple admin operations by RTNL mutex
 */
static void del_nbp(struct net_bridge_port *p)
{
	struct net_bridge *br = p->br;
	struct net_device *dev = p->dev;

	sysfs_remove_link(br->ifobj, p->dev->name);

	nbp_delete_promisc(p);

	spin_lock_bh(&br->lock);
	br_stp_disable_port(p);
	spin_unlock_bh(&br->lock);

	br_ifinfo_notify(RTM_DELLINK, NULL, p);

	list_del_rcu(&p->list);
	if (netdev_get_fwd_headroom(dev) == br->dev->needed_headroom)
		update_headroom(br, get_max_headroom(br));
	netdev_reset_rx_headroom(dev);

	nbp_vlan_flush(p);
	br_fdb_delete_by_port(br, p, 0, 1);
	switchdev_deferred_process();
	nbp_backup_clear(p);

	nbp_update_port_count(br);

	netdev_upper_dev_unlink(dev, br->dev);

	dev->priv_flags &= ~IFF_BRIDGE_PORT;

	netdev_rx_handler_unregister(dev);

	br_multicast_del_port(p);

	kobject_uevent(&p->kobj, KOBJ_REMOVE);
	kobject_del(&p->kobj);

	br_netpoll_disable(p);

	call_rcu(&p->rcu, destroy_nbp_rcu);
	
#if defined(CONFIG_TP_IMAGE) && defined(CONFIG_NET_DSA_MT7530)
	bridge_port_set_sa(br, dev, false);
#endif
	
}

/* Delete bridge device */
void br_dev_delete(struct net_device *dev, struct list_head *head)
{
	struct net_bridge *br = netdev_priv(dev);
	struct net_bridge_port *p, *n;

	list_for_each_entry_safe(p, n, &br->port_list, list) {
		del_nbp(p);
	}

	br_recalculate_neigh_suppress_enabled(br);

	br_fdb_delete_by_port(br, NULL, 0, 1);

	cancel_delayed_work_sync(&br->gc_work);

	br_sysfs_delbr(br->dev);
	unregister_netdevice_queue(br->dev, head);
}

/* find an available port number */
static int find_portno(struct net_bridge *br)
{
	int index;
	struct net_bridge_port *p;
	unsigned long *inuse;

	inuse = bitmap_zalloc(BR_MAX_PORTS, GFP_KERNEL);
	if (!inuse)
		return -ENOMEM;

	set_bit(0, inuse);	/* zero is reserved */
	list_for_each_entry(p, &br->port_list, list) {
		set_bit(p->port_no, inuse);
	}
	index = find_first_zero_bit(inuse, BR_MAX_PORTS);
	bitmap_free(inuse);

	return (index >= BR_MAX_PORTS) ? -EXFULL : index;
}

/* called with RTNL but without bridge lock */
static struct net_bridge_port *new_nbp(struct net_bridge *br,
				       struct net_device *dev)
{
	struct net_bridge_port *p;
	int index, err;

	index = find_portno(br);
	if (index < 0)
		return ERR_PTR(index);

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return ERR_PTR(-ENOMEM);

	p->br = br;
	dev_hold(dev);
	p->dev = dev;
	p->path_cost = port_cost(dev);
	p->priority = 0x8000 >> BR_PORT_BITS;
	p->port_no = index;
	p->flags = BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	br_init_port(p);
	br_set_state(p, BR_STATE_DISABLED);
#if defined(CONFIG_TP_IMAGE) && defined(CONFIG_BRIDGE_VLAN_TP)
	p->vlan_id = 0;
#endif	
	br_stp_port_timer_init(p);
	err = br_multicast_add_port(p);
	if (err) {
		dev_put(dev);
		kfree(p);
		p = ERR_PTR(err);
	}

	return p;
}

int br_add_bridge(struct net *net, const char *name)
{
	struct net_device *dev;
	int res;

	dev = alloc_netdev(sizeof(struct net_bridge), name, NET_NAME_UNKNOWN,
			   br_dev_setup);

	if (!dev)
		return -ENOMEM;

	dev_net_set(dev, net);
	dev->rtnl_link_ops = &br_link_ops;

	res = register_netdev(dev);
	if (res)
		free_netdev(dev);
	return res;
}

int br_del_bridge(struct net *net, const char *name)
{
	struct net_device *dev;
	int ret = 0;

	rtnl_lock();
	dev = __dev_get_by_name(net, name);
	if (dev == NULL)
		ret =  -ENXIO; 	/* Could not find device */

	else if (!(dev->priv_flags & IFF_EBRIDGE)) {
		/* Attempt to delete non bridge device! */
		ret = -EPERM;
	}

	else if (dev->flags & IFF_UP) {
		/* Not shutdown yet. */
		ret = -EBUSY;
	}

	else
		br_dev_delete(dev, NULL);

	rtnl_unlock();
	return ret;
}

/* MTU of the bridge pseudo-device: ETH_DATA_LEN or the minimum of the ports */
static int br_mtu_min(const struct net_bridge *br)
{
	const struct net_bridge_port *p;
	int ret_mtu = 0;

	list_for_each_entry(p, &br->port_list, list)
		if (!ret_mtu || ret_mtu > p->dev->mtu)
			ret_mtu = p->dev->mtu;

	return ret_mtu ? ret_mtu : ETH_DATA_LEN;
}

void br_mtu_auto_adjust(struct net_bridge *br)
{
	ASSERT_RTNL();

	/* if the bridge MTU was manually configured don't mess with it */
	if (br_opt_get(br, BROPT_MTU_SET_BY_USER))
		return;

	/* change to the minimum MTU and clear the flag which was set by
	 * the bridge ndo_change_mtu callback
	 */
	dev_set_mtu(br->dev, br_mtu_min(br));
	br_opt_toggle(br, BROPT_MTU_SET_BY_USER, false);
}

static void br_set_gso_limits(struct net_bridge *br)
{
	unsigned int gso_max_size = GSO_MAX_SIZE;
	u16 gso_max_segs = GSO_MAX_SEGS;
	const struct net_bridge_port *p;

	list_for_each_entry(p, &br->port_list, list) {
		gso_max_size = min(gso_max_size, p->dev->gso_max_size);
		gso_max_segs = min(gso_max_segs, p->dev->gso_max_segs);
	}
	br->dev->gso_max_size = gso_max_size;
	br->dev->gso_max_segs = gso_max_segs;
}

/*
 * Recomputes features using slave's features
 */
netdev_features_t br_features_recompute(struct net_bridge *br,
	netdev_features_t features)
{
	struct net_bridge_port *p;
	netdev_features_t mask;

	if (list_empty(&br->port_list))
		return features;

	mask = features;
	features &= ~NETIF_F_ONE_FOR_ALL;

	list_for_each_entry(p, &br->port_list, list) {
		features = netdev_increment_features(features,
						     p->dev->features, mask);
	}
	features = netdev_add_tso_features(features, mask);

	return features;
}

/* called with RTNL */
int br_add_if(struct net_bridge *br, struct net_device *dev,
	      struct netlink_ext_ack *extack)
{
	struct net_bridge_port *p;
	int err = 0;
	unsigned br_hr, dev_hr;
	bool changed_addr;

	/* Don't allow bridging non-ethernet like devices, or DSA-enabled
	 * master network devices since the bridge layer rx_handler prevents
	 * the DSA fake ethertype handler to be invoked, so we do not strip off
	 * the DSA switch tag protocol header and the bridge layer just return
	 * RX_HANDLER_CONSUMED, stopping RX processing for these frames.
	 */
	if ((dev->flags & IFF_LOOPBACK) ||
	    dev->type != ARPHRD_ETHER || dev->addr_len != ETH_ALEN ||
	    !is_valid_ether_addr(dev->dev_addr) ||
	    netdev_uses_dsa(dev))
		return -EINVAL;

	/* No bridging of bridges */
	if (dev->netdev_ops->ndo_start_xmit == br_dev_xmit) {
		NL_SET_ERR_MSG(extack,
			       "Can not enslave a bridge to a bridge");
		return -ELOOP;
	}

	/* Device has master upper dev */
	if (netdev_master_upper_dev_get(dev))
		return -EBUSY;

	/* No bridging devices that dislike that (e.g. wireless) */
	if (dev->priv_flags & IFF_DONT_BRIDGE) {
		NL_SET_ERR_MSG(extack,
			       "Device does not allow enslaving to a bridge");
		return -EOPNOTSUPP;
	}

	p = new_nbp(br, dev);
	if (IS_ERR(p))
		return PTR_ERR(p);

	call_netdevice_notifiers(NETDEV_JOIN, dev);

	err = dev_set_allmulti(dev, 1);
	if (err) {
		kfree(p);	/* kobject not yet init'd, manually free */
		goto err1;
	}

	err = kobject_init_and_add(&p->kobj, &brport_ktype, &(dev->dev.kobj),
				   SYSFS_BRIDGE_PORT_ATTR);
	if (err)
		goto err2;

	err = br_sysfs_addif(p);
	if (err)
		goto err2;

	err = br_netpoll_enable(p);
	if (err)
		goto err3;

	err = netdev_rx_handler_register(dev, br_handle_frame, p);
	if (err)
		goto err4;

	dev->priv_flags |= IFF_BRIDGE_PORT;

	err = netdev_master_upper_dev_link(dev, br->dev, NULL, NULL, extack);
	if (err)
		goto err5;

	err = nbp_switchdev_mark_set(p);
	if (err)
		goto err6;

	dev_disable_lro(dev);

#if defined(CONFIG_TP_IMAGE) && defined(CONFIG_NET_DSA_MT7530)
	bridge_port_set_sa(br, dev, true);
#endif

	list_add_rcu(&p->list, &br->port_list);

	nbp_update_port_count(br);

	netdev_update_features(br->dev);

	br_hr = br->dev->needed_headroom;
	dev_hr = netdev_get_fwd_headroom(dev);
	if (br_hr < dev_hr)
		update_headroom(br, dev_hr);
	else
		netdev_set_rx_headroom(dev, br_hr);

	if (br_fdb_insert(br, p, dev->dev_addr, 0))
		netdev_err(dev, "failed insert local address bridge forwarding table\n");

	if (br->dev->addr_assign_type != NET_ADDR_SET) {
		/* Ask for permission to use this MAC address now, even if we
		 * don't end up choosing it below.
		 */
		err = dev_pre_changeaddr_notify(br->dev, dev->dev_addr, extack);
		if (err)
			goto err7;
	}

	err = nbp_vlan_init(p, extack);
	if (err) {
		netdev_err(dev, "failed to initialize vlan filtering on this port\n");
		goto err7;
	}

	spin_lock_bh(&br->lock);
	changed_addr = br_stp_recalculate_bridge_id(br);

	if (netif_running(dev) && netif_oper_up(dev) &&
	    (br->dev->flags & IFF_UP))
		br_stp_enable_port(p);
	spin_unlock_bh(&br->lock);

	br_ifinfo_notify(RTM_NEWLINK, NULL, p);

	if (changed_addr)
		call_netdevice_notifiers(NETDEV_CHANGEADDR, br->dev);

	br_mtu_auto_adjust(br);
	br_set_gso_limits(br);

	kobject_uevent(&p->kobj, KOBJ_ADD);
#ifdef CONFIG_TP_HYFI_BRIDGE
	call_netdevice_notifiers(NETDEV_BR_JOIN, dev);
#endif
	return 0;

err7:
	list_del_rcu(&p->list);
	br_fdb_delete_by_port(br, p, 0, 1);
	nbp_update_port_count(br);
err6:
	netdev_upper_dev_unlink(dev, br->dev);
err5:
	dev->priv_flags &= ~IFF_BRIDGE_PORT;
	netdev_rx_handler_unregister(dev);
err4:
	br_netpoll_disable(p);
err3:
	sysfs_remove_link(br->ifobj, p->dev->name);
err2:
	kobject_put(&p->kobj);
	dev_set_allmulti(dev, -1);
err1:
	dev_put(dev);
	return err;
}

/* called with RTNL */
int br_del_if(struct net_bridge *br, struct net_device *dev)
{
	struct net_bridge_port *p;
	bool changed_addr;

	p = br_port_get_rtnl(dev);
	if (!p || p->br != br)
		return -EINVAL;
#ifdef CONFIG_TP_HYFI_BRIDGE
	call_netdevice_notifiers(NETDEV_BR_LEAVE, dev);
#endif
	/* Since more than one interface can be attached to a bridge,
	 * there still maybe an alternate path for netconsole to use;
	 * therefore there is no reason for a NETDEV_RELEASE event.
	 */
	del_nbp(p);

	br_mtu_auto_adjust(br);
	br_set_gso_limits(br);

	spin_lock_bh(&br->lock);
	changed_addr = br_stp_recalculate_bridge_id(br);
	spin_unlock_bh(&br->lock);

	if (changed_addr)
		call_netdevice_notifiers(NETDEV_CHANGEADDR, br->dev);

	netdev_update_features(br->dev);

	return 0;
}

void br_port_flags_change(struct net_bridge_port *p, unsigned long mask)
{
	struct net_bridge *br = p->br;

	if (mask & BR_AUTO_MASK)
		nbp_update_port_count(br);

	if (mask & BR_NEIGH_SUPPRESS)
		br_recalculate_neigh_suppress_enabled(br);
}

bool br_port_flag_is_set(const struct net_device *dev, unsigned long flag)
{
	struct net_bridge_port *p;

	p = br_port_get_rtnl_rcu(dev);
	if (!p)
		return false;

	return p->flags & flag;
}
EXPORT_SYMBOL_GPL(br_port_flag_is_set);

#if defined(CONFIG_TP_IMAGE) || defined(CONFIG_TP_HYFI_BRIDGE)
/* br_port_dev_get()
 *      If a skb is provided, and the br_port_dev_get_hook_t hook exists,
 *      use that to try and determine the egress port for that skb.
 *      If not, or no egress port could be determined, use the given addr
 *      to identify the port to which it is reachable,
 *	returing a reference to the net device associated with that port.
 *
 * NOTE: Return NULL if given dev is not a bridge or the mac has no
 * associated port.
 */
struct net_device *br_port_dev_get(struct net_device *dev, unsigned char *addr,
				   struct sk_buff *skb,
				   unsigned int cookie)
{
	struct net_bridge_fdb_entry *fdbe;
	struct net_bridge *br;
	struct net_device *netdev = NULL;

	/* Is this a bridge? */
	if (!(dev->priv_flags & IFF_EBRIDGE))
		return NULL;

	rcu_read_lock();

	/* If the hook exists and the skb isn't NULL, try and get the port */
	if (skb) {
		br_port_dev_get_hook_t *port_dev_get_hook;

		port_dev_get_hook = rcu_dereference(br_port_dev_get_hook);
		if (port_dev_get_hook) {
			struct net_bridge_port *pdst =
				__br_get(port_dev_get_hook, NULL, dev, skb,
					 addr, cookie);
			if (pdst) {
				dev_hold(pdst->dev);
				netdev = pdst->dev;
				goto out;
			}
		}
	}

	/* Either there is no hook, or can't
	 * determine the port to use - fall back to using FDB
	 */

	br = netdev_priv(dev);

	/* Lookup the fdb entry and get reference to the port dev */
	fdbe = br_fdb_find_rcu(br, addr, 0);
	if (fdbe && fdbe->dst) {
		netdev = fdbe->dst->dev; /* port device */
		dev_hold(netdev);
	}
out:
	rcu_read_unlock();
	return netdev;
}
EXPORT_SYMBOL_GPL(br_port_dev_get);

/* Update bridge statistics for bridge packets processed by offload engines */
void br_dev_update_stats(struct net_device *dev,
			 struct rtnl_link_stats64 *nlstats)
{
	struct net_bridge *br;
	struct pcpu_sw_netstats *stats;

	/* Is this a bridge? */
	if (!(dev->priv_flags & IFF_EBRIDGE))
		return;

	br = netdev_priv(dev);
	stats = per_cpu_ptr(br->stats, 0);

	u64_stats_update_begin(&stats->syncp);
	stats->rx_packets += nlstats->rx_packets;
	stats->rx_bytes += nlstats->rx_bytes;
	stats->tx_packets += nlstats->tx_packets;
	stats->tx_bytes += nlstats->tx_bytes;
	u64_stats_update_end(&stats->syncp);
}
EXPORT_SYMBOL_GPL(br_dev_update_stats);
#endif /* CONFIG_TP_IMAGE */
