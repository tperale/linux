// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Semtech SX1280/SX1281 LoRa transceiver
 *
 * Copyright (c) 2018 Andreas Färber
 *
 * Based on sx1276.c:
 * Copyright (c) 2016-2018 Andreas Färber
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/lora.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/lora/dev.h>
#include <linux/spi/spi.h>

#define SX128X_CMD_GET_SILICON_VERSION		0x14
#define SX128X_CMD_WRITE_REGISTER		0x18
#define SX128X_CMD_READ_REGISTER		0x19
#define SX128X_CMD_SET_STANDBY			0x80
#define SX128X_CMD_SET_PACKET_TYPE		0x8a
#define SX128X_CMD_SET_TX_PARAMS		0x8e
#define SX128X_CMD_SET_REGULATOR_MODE		0x96
#define SX128X_CMD_GET_STATUS			0xc0

#define SX128X_STATUS_COMMAND_MASK			GENMASK(4, 2)
#define SX128X_STATUS_COMMAND_TIMEOUT			(0x3 << 2)
#define SX128X_STATUS_COMMAND_PROCESSING_ERROR		(0x4 << 2)
#define SX128X_STATUS_COMMAND_FAILURE_TO_EXECUTE	(0x5 << 2)

#define SX128X_STATUS_MODE_MASK				GENMASK(7, 5)
#define SX128X_STATUS_MODE_STDBY_RC			(0x2 << 5)
#define SX128X_STATUS_MODE_STDBY_XOSC			(0x3 << 5)

#define SX128X_STANDBY_CONFIG_STDBY_RC		0
#define SX128X_STANDBY_CONFIG_STDBY_XOSC	1

#define SX128X_PACKET_TYPE_GFSK		0x00
#define SX128X_PACKET_TYPE_LORA		0x01

#define SX128X_RADIO_RAMP_20_US		0xe0

#define SX128X_REGULATOR_MODE_LDO	0
#define SX128X_REGULATOR_MODE_DCDC	1

struct sx128x_device;

struct sx128x_ops {
	int (*send_command)(struct sx128x_device *sxdev, u8 opcode, u8 argc, const u8 *argv, u8 *buf, size_t buf_len);
	int (*send_addr_command)(struct sx128x_device *sxdev, u8 opcode, u16 addr, u8 argc, const u8 *argv, u8 *buf, size_t buf_len);
};

struct sx128x_device {
	struct device *dev;
	struct gpio_desc *rst;
	struct gpio_desc *busy_gpio;

	const struct sx128x_ops *cmd_ops;

	struct net_device *netdev;
};

struct sx128x_priv {
	struct sx128x_device *sxdev;
};

static int sx128x_get_status(struct sx128x_device *sxdev, u8 *val)
{
	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_GET_STATUS, 0, NULL, val, 1);
}

static int sx128x_write_regs(struct sx128x_device *sxdev, u16 addr, const u8 *val, size_t len)
{
	return sxdev->cmd_ops->send_addr_command(sxdev, SX128X_CMD_WRITE_REGISTER, addr, len, val, NULL, 0);
}

static inline int sx128x_write_reg(struct sx128x_device *sxdev, u16 addr, u8 val)
{
	return sx128x_write_regs(sxdev, addr, &val, 1);
}

static int sx128x_read_regs(struct sx128x_device *sxdev, u16 addr, u8 *val, size_t len)
{
	return sxdev->cmd_ops->send_addr_command(sxdev, SX128X_CMD_READ_REGISTER, addr, 0, NULL, val, len);
}

static inline int sx128x_read_reg(struct sx128x_device *sxdev, u16 addr, u8 *val)
{
	return sx128x_read_regs(sxdev, addr, val, 1);
}

static int sx128x_set_standby(struct sx128x_device *sxdev, u8 val)
{
	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_SET_STANDBY, 1, &val, NULL, 0);
}

static int sx128x_set_packet_type(struct sx128x_device *sxdev, u8 val)
{
	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_SET_PACKET_TYPE, 1, &val, NULL, 0);
}

static int sx128x_set_tx_params(struct sx128x_device *sxdev, u8 power, u8 ramp_time)
{
	u8 buf[2];

	buf[0] = power;
	buf[1] = ramp_time;

	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_SET_TX_PARAMS, 2, buf, NULL, 0);
}

static int sx128x_set_regulator_mode(struct sx128x_device *sxdev, u8 val)
{
	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_SET_REGULATOR_MODE, 1, &val, NULL, 0);
}

static int sx128x_get_silicon_version(struct sx128x_device *sxdev, u8 *val)
{
	return sxdev->cmd_ops->send_command(sxdev, SX128X_CMD_GET_SILICON_VERSION, 0, NULL, val, 1);
}

static void sx128x_reset(struct sx128x_device *sxdev)
{
	gpiod_set_value_cansleep(sxdev->rst, 0);
	msleep(50);
	gpiod_set_value_cansleep(sxdev->rst, 1);
	msleep(20);
}

static netdev_tx_t sx128x_loradev_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	//struct sx128x_priv *priv = netdev_priv(netdev);

	netdev_dbg(netdev, "%s\n", __func__);

	if (skb->protocol != htons(ETH_P_LORA) &&
	    skb->protocol != htons(ETH_P_FLRC)) {
		kfree_skb(skb);
		netdev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	netif_stop_queue(netdev);
	/* TODO */

	return NETDEV_TX_OK;
}

static int sx128x_loradev_open(struct net_device *netdev)
{
	//struct sx128x_priv *priv = netdev_priv(netdev);
	int ret;

	netdev_dbg(netdev, "%s\n", __func__);

	ret = open_loradev(netdev);
	if (ret)
		return ret;

	/* TODO */

	netif_wake_queue(netdev);

	return 0;
}

static int sx128x_loradev_stop(struct net_device *netdev)
{
	//struct sx128x_priv *priv = netdev_priv(netdev);

	netdev_dbg(netdev, "%s\n", __func__);

	close_loradev(netdev);

	/* TODO */

	return 0;
}

static const struct net_device_ops sx128x_netdev_ops =  {
	.ndo_open = sx128x_loradev_open,
	.ndo_stop = sx128x_loradev_stop,
	.ndo_start_xmit = sx128x_loradev_start_xmit,
};

static int sx128x_probe(struct sx128x_device *sxdev)
{
	struct device *dev = sxdev->dev;
	struct net_device *netdev;
	struct sx128x_priv *priv;
	u8 val, status;
	int ret;

	sxdev->rst = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sxdev->rst)) {
		ret = PTR_ERR(sxdev->rst);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to obtain reset GPIO (%d)\n", ret);
		return ret;
	}

	sxdev->busy_gpio = devm_gpiod_get_optional(dev, "busy", GPIOD_IN);
	if (IS_ERR(sxdev->busy_gpio)) {
		ret = PTR_ERR(sxdev->busy_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to obtain reset GPIO (%d)\n", ret);
		return ret;
	}

	sx128x_reset(sxdev);

	ret = sx128x_get_status(sxdev, &status);
	if (ret) {
		dev_err(dev, "GetStatus failed (%d)\n", ret);
		return ret;
	}

	if ((status & SX128X_STATUS_MODE_MASK) != SX128X_STATUS_MODE_STDBY_RC) {
		ret = sx128x_set_standby(sxdev, SX128X_STANDBY_CONFIG_STDBY_RC);
		if (ret) {
			dev_err(dev, "SetStandby STDBY_RC failed (%d)\n", ret);
			return ret;
		}
	}

	ret = sx128x_set_regulator_mode(sxdev, SX128X_REGULATOR_MODE_LDO);
	if (ret) {
		dev_err(dev, "SetRegulatorMode LDO failed (%d)\n", ret);
		return ret;
	}

	ret = sx128x_set_tx_params(sxdev, 31, SX128X_RADIO_RAMP_20_US);
	if (ret) {
		dev_err(dev, "SetTxParams failed (%d)\n", ret);
		return ret;
	}

	ret = sx128x_get_silicon_version(sxdev, &val);
	if (ret) {
		dev_err(dev, "GetSiliconVersion failed (%d)\n", ret);
		return ret;
	}
	dev_info(dev, "silicon version: 0x%02x\n", (unsigned int)val);

	ret = sx128x_set_packet_type(sxdev, SX128X_PACKET_TYPE_LORA);
	if (ret) {
		dev_err(dev, "SetPacketType LORA failed (%d)\n", ret);
		return ret;
	}

	ret = sx128x_read_reg(sxdev, 0x925, &val);
	if (ret) {
		dev_err(dev, "ReadRegister failed (%d)\n", ret);
		return ret;
	}
	dev_info(dev, "ReadRegister 0x925: 0x%02x\n", (unsigned int)val);

	netdev = devm_alloc_loradev(dev, sizeof(*priv));
	if (!netdev)
		return -ENOMEM;

	netdev->netdev_ops = &sx128x_netdev_ops;

	priv = netdev_priv(netdev);
	priv->sxdev = sxdev;

	sxdev->netdev = netdev;
	SET_NETDEV_DEV(netdev, dev);

	ret = register_loradev(netdev);
	if (ret) {
		dev_err(dev, "registering loradev failed (%d)\n", ret);
		return ret;
	}

	dev_info(dev, "probed\n");

	return 0;
}

static int sx128x_remove(struct sx128x_device *sxdev)
{
	unregister_loradev(sxdev->netdev);

	dev_info(sxdev->dev, "removed\n");

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sx128x_dt_ids[] = {
	{ .compatible = "semtech,sx1280" },
	{}
};
MODULE_DEVICE_TABLE(of, sx128x_dt_ids);
#endif

static inline int sx128x_status_to_errno(struct sx128x_device *sxdev, u8 status)
{
	dev_dbg(sxdev->dev, "%s: 0x%02x\n", __func__, (unsigned int)status);

	switch (status & GENMASK(4, 2)) {
	case SX128X_STATUS_COMMAND_TIMEOUT:
		return -ETIMEDOUT;
	case SX128X_STATUS_COMMAND_PROCESSING_ERROR:
		return -EINVAL;
	case SX128X_STATUS_COMMAND_FAILURE_TO_EXECUTE:
		return -EOPNOTSUPP;
	default:
		return 0;
	}
}

static inline int sx128x_busy_check_pre(struct sx128x_device *sxdev)
{
	int ret;

	if (!sxdev->busy_gpio)
		return 0;

	ret = gpiod_get_value_cansleep(sxdev->busy_gpio);
	if (ret < 0) {
		dev_err(sxdev->dev, "reading Busy GPIO failed (%d)\n", ret);
		return ret;
	}
	if (ret > 0) {
		dev_warn(sxdev->dev, "chip is busy!\n");
		return -EBUSY;
	}
	return 0;
}

static inline int sx128x_busy_wait_post(struct sx128x_device *sxdev)
{
	int ret, i;

	if (!sxdev->busy_gpio)
		return 0;

	for (i = 10; i > 0; i--) {
		ret = gpiod_get_value_cansleep(sxdev->busy_gpio);
		if (ret == 0)
			return 0;
		else if (ret < 0) {
			dev_err(sxdev->dev, "reading Busy GPIO failed (%d)\n", ret);
			return ret;
		}
	}
	dev_dbg(sxdev->dev, "still busy\n");
	return 0;
}

#ifdef CONFIG_SPI
static int sx128x_spi_send_command(struct sx128x_device *sxdev, u8 opcode, u8 argc, const u8 *argv, u8 *buf, size_t buf_len)
{
	struct spi_device *spi = to_spi_device(sxdev->dev);
	u8 status;
	struct spi_transfer xfers[] = {
		{
			.tx_buf = &opcode,
			.len = 1,
		},
		{
			.tx_buf = argv,
			.len = (argc > 0) ? argc - 1 : 0,
		},
		{
			.tx_buf = argv ? argv + (argc - 1) : NULL,
			.rx_buf = &status,
			.len = 1,
		},
		{
			.rx_buf = buf,
			.len = (opcode == SX128X_CMD_GET_STATUS) ? 0 : buf_len,
		},
	};
	int ret;

	ret = sx128x_busy_check_pre(sxdev);
	if (ret)
		return ret;

	ret = spi_sync_transfer(spi, xfers, ARRAY_SIZE(xfers));
	if (ret)
		return ret;

	if (buf && opcode == SX128X_CMD_GET_STATUS)
		*buf = status;

	sx128x_busy_wait_post(sxdev);

	return sx128x_status_to_errno(sxdev, status);
}

static int sx128x_spi_send_addr_command(struct sx128x_device *sxdev, u8 opcode, u16 addr, u8 argc, const u8 *argv, u8 *buf, size_t buf_len)
{
	struct spi_device *spi = to_spi_device(sxdev->dev);
	u8 addr_buf[2];
	u8 status;
	struct spi_transfer xfers[] = {
		{
			.tx_buf = &opcode,
			.len = 1,
		},
		{
			.tx_buf = addr_buf,
			.len = 2,
		},
		{
			.tx_buf = argv,
			.len = (argc > 0) ? argc - 1 : 0,
		},
		{
			.tx_buf = argv ? argv + (argc - 1) : NULL,
			.rx_buf = &status,
			.len = 1,
		},
		{
			.rx_buf = buf,
			.len = buf_len,
		},
	};
	int ret;

	addr_buf[0] = addr >> 8;
	addr_buf[1] = addr;

	ret = sx128x_busy_check_pre(sxdev);
	if (ret)
		return ret;

	ret = spi_sync_transfer(spi, xfers, ARRAY_SIZE(xfers));
	if (ret)
		return ret;

	sx128x_busy_wait_post(sxdev);

	return sx128x_status_to_errno(sxdev, status);
}

static const struct sx128x_ops sx128x_spi_cmd_ops = {
	.send_command = sx128x_spi_send_command,
	.send_addr_command = sx128x_spi_send_addr_command,
};

static int sx128x_spi_probe(struct spi_device *spi)
{
	struct sx128x_device *sxdev;

	sxdev = devm_kzalloc(&spi->dev, sizeof(*sxdev), GFP_KERNEL);
	if (!sxdev)
		return -ENOMEM;

	sxdev->dev = &spi->dev;
	sxdev->cmd_ops = &sx128x_spi_cmd_ops;

	spi_set_drvdata(spi, sxdev);

	spi->bits_per_word = 8;
	spi_setup(spi);

	return sx128x_probe(sxdev);
}

static int sx128x_spi_remove(struct spi_device *spi)
{
	struct sx128x_device *sxdev = spi_get_drvdata(spi);

	return sx128x_remove(sxdev);
}

static struct spi_driver sx128x_spi_driver = {
	.driver = {
		.name = "sx128x-spi",
		.of_match_table = of_match_ptr(sx128x_dt_ids),
	},
	.probe = sx128x_spi_probe,
	.remove = sx128x_spi_remove,
};
#endif

static int __init sx128x_init(void)
{
	int ret = 0;

#ifdef CONFIG_SPI
	ret = spi_register_driver(&sx128x_spi_driver);
	if (ret)
		return ret;
#endif
	return ret;
}

static void __exit sx128x_exit(void)
{
#ifdef CONFIG_SPI
	spi_unregister_driver(&sx128x_spi_driver);
#endif
}

module_init(sx128x_init);
module_exit(sx128x_exit);

MODULE_DESCRIPTION("SX1280 SPI driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");
