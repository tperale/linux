/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LoRa config interface
 *
 * Copyright (c) 2019 Andreas Färber
 */
#ifndef __NET_CFGLORA_H
#define __NET_CFGLORA_H

struct lora_phy;

struct cfglora_ops {
	int (*get_freq)(struct lora_phy *phy, u32 *val);
};

struct lora_phy {
	struct device *dev;
	struct net_device *netdev;

	/* keep this last */
	u8 priv[0] __aligned(NETDEV_ALIGN);
};

struct lora_phy *lora_phy_new(const struct cfglora_ops *ops, size_t priv_size);
struct lora_phy *devm_lora_phy_new(struct device *dev, const struct cfglora_ops *ops, size_t priv_size);
int lora_phy_register(struct lora_phy *phy);
void lora_phy_unregister(struct lora_phy *phy);
void lora_phy_free(struct lora_phy *phy);

#endif
