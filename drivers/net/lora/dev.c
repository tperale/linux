// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2017-2018 Andreas Färber
 */

#include <linux/if_arp.h>
#include <linux/lora.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/lora/dev.h>
#include <linux/lora/skb.h>
#include <net/rtnetlink.h>

#define LORA_MTU 256 /* XXX */

struct sk_buff *alloc_lora_skb(struct net_device *dev, u8 **data)
{
	struct sk_buff *skb;

	skb = netdev_alloc_skb(dev, sizeof(struct lora_skb_priv) + LORA_MTU);
	if (unlikely(!skb))
		return NULL;

	skb->protocol = htons(ETH_P_LORA);
	skb->pkt_type = PACKET_BROADCAST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);

	lora_skb_reserve(skb);
	lora_skb_prv(skb)->ifindex = dev->ifindex;

	return skb;
}
EXPORT_SYMBOL_GPL(alloc_lora_skb);

int open_loradev(struct net_device *dev)
{
	if (!netif_carrier_ok(dev))
		netif_carrier_on(dev);

	return 0;
}
EXPORT_SYMBOL_GPL(open_loradev);

void close_loradev(struct net_device *dev)
{
}
EXPORT_SYMBOL_GPL(close_loradev);

static void lora_setup(struct net_device *dev)
{
	dev->type = ARPHRD_LORA;
	dev->mtu = LORA_MTU;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->tx_queue_len = 10;

	dev->flags = IFF_NOARP;
	dev->features = 0;
}

struct net_device *alloc_loradev(int sizeof_priv)
{
	struct net_device *dev;
	struct lora_dev_priv *priv;

	dev = alloc_netdev(sizeof_priv, "lora%d", NET_NAME_UNKNOWN, lora_setup);
	if (!dev)
		return NULL;

	priv = netdev_priv(dev);
	priv->dev = dev;

	return dev;
}
EXPORT_SYMBOL_GPL(alloc_loradev);

void free_loradev(struct net_device *dev)
{
	free_netdev(dev);
}
EXPORT_SYMBOL_GPL(free_loradev);

static void devm_free_loradev(struct device *dev, void *res)
{
	struct net_device **net = res;

	free_loradev(*net);
}

struct net_device *devm_alloc_loradev(struct device *dev, size_t priv)
{
	struct net_device **ptr;
	struct net_device *net;

	net = alloc_loradev(priv);
	if (!net)
		return NULL;

	ptr = devres_alloc(devm_free_loradev, sizeof(*ptr), GFP_KERNEL);
	if (!ptr) {
		free_loradev(net);
		return NULL;
	}

	*ptr = net;
	devres_add(dev, ptr);

	return net;
}
EXPORT_SYMBOL_GPL(devm_alloc_loradev);

static struct rtnl_link_ops lora_link_ops __read_mostly = {
	.kind = "lora",
	.setup = lora_setup,
};

int register_loradev(struct net_device *dev)
{
	dev->rtnl_link_ops = &lora_link_ops;
	return register_netdev(dev);
}
EXPORT_SYMBOL_GPL(register_loradev);

void unregister_loradev(struct net_device *dev)
{
	unregister_netdev(dev);
}
EXPORT_SYMBOL_GPL(unregister_loradev);

static int __init lora_dev_init(void)
{
	printk("lora-dev: init\n");

	return rtnl_link_register(&lora_link_ops);
}

static void __exit lora_dev_exit(void)
{
	printk("lora-dev: exit\n");

	rtnl_link_unregister(&lora_link_ops);
}

module_init(lora_dev_init);
module_exit(lora_dev_exit);

MODULE_DESCRIPTION("LoRa device driver interface");
MODULE_ALIAS_RTNL_LINK("lora");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andreas Färber");
