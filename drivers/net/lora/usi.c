// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USI WM-SG-SM-42
 *
 * Copyright (c) 2017-2018 Andreas Färber
 */

#include <linux/lora.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/lora/dev.h>

struct usi_device {
	struct serdev_device *serdev;

	int mode;

	char rx_buf[4096];
	int rx_len;

	struct completion prompt_recv_comp;
	struct completion tx_event_recv_comp;
};

static bool usi_cmd_ok(const char *resp)
{
	int len = strlen(resp);

	return (len == 4 && !strcmp(resp, "OK\r\n")) ||
	       (len >= 6 && !strcmp(resp + len - 6, "\r\nOK\r\n"));
}

static int usi_send_command(struct usi_device *usidev, const char *cmd, char **data, unsigned long timeout)
{
	struct serdev_device *sdev = usidev->serdev;
	const char cr = '\r';
	int cmd_len, resp_len;
	char *resp;

	cmd_len = strlen(cmd);
	serdev_device_write_buf(sdev, cmd, cmd_len);
	serdev_device_write_buf(sdev, &cr, 1);

	timeout = wait_for_completion_timeout(&usidev->prompt_recv_comp, timeout);
	if (!timeout)
		return -ETIMEDOUT;

	resp = usidev->rx_buf;
	resp_len = usidev->rx_len;
	if (!strncmp(resp, cmd, cmd_len) && resp[cmd_len] == '\r') {
		dev_dbg(&sdev->dev, "Skipping echo\n");
		resp += cmd_len + 1;
		resp_len -= cmd_len + 1;
	}
	dev_dbg(&sdev->dev, "Received: '%s'\n", resp);
	if (data)
		*data = kstrdup(resp, GFP_KERNEL);

	usidev->rx_len = 0;
	reinit_completion(&usidev->prompt_recv_comp);

	return 0;
}

static int usi_simple_cmd(struct usi_device *usidev, const char *cmd, unsigned long timeout)
{
	char *resp;
	int ret;

	ret = usi_send_command(usidev, cmd, &resp, timeout);
	if (ret)
		return ret;

	if (strcmp(resp, "OK\r\n") == 0) {
		kfree(resp);
		return 0;
	}

	kfree(resp);

	return -EINVAL;
}

static int usi_cmd_reset(struct usi_device *usidev)
{
	int ret;

	ret = usi_send_command(usidev, "ATZ", NULL, HZ);
	if (ret)
		return ret;

	mdelay(1000);

	return 0;
}

static int usi_cmd_read_reg(struct usi_device *usidev, u8 addr, u8 *val)
{
	char *cmd;
	char *resp;
	char *sz;
	int ret;

	cmd = kasprintf(GFP_KERNEL, "AT+RREG=0x%02x", (int)addr);
	if (!cmd)
		return -ENOMEM;

	ret = usi_send_command(usidev, cmd, &resp, HZ);
	if (ret) {
		kfree(cmd);
		return ret;
	}
	if (!usi_cmd_ok(resp)) {
		kfree(resp);
		kfree(cmd);
		return -EINVAL;
	}
	resp[strlen(resp) - 6] = '\0';
	sz = resp;
	if (strstarts(sz, "+Reg="))
		sz += 5;
	if (strncasecmp(sz, cmd + 8, 4) == 0 && strstarts(sz + 4, ", "))
		sz += 6;

	kfree(cmd);

	dev_dbg(&usidev->serdev->dev, "Parsing '%s'\n", sz);
	ret = kstrtou8(sz, 0, val);
	if (ret) {
		kfree(resp);
		return ret;
	}

	kfree(resp);

	return 0;
}

static int usi_receive_buf(struct serdev_device *sdev, const u8 *data, size_t count)
{
	struct usi_device *usidev = serdev_device_get_drvdata(sdev);
	size_t i = 0;
	int len = 0;

	dev_dbg(&sdev->dev, "Receive (%d)\n", (int)count);

	for (i = 0; i < count; i++) {
		dev_dbg(&sdev->dev, "Receive: 0x%02x\n", (int)data[i]);
	}
	i = 0;

	if (completion_done(&usidev->prompt_recv_comp) ||
	    completion_done(&usidev->tx_event_recv_comp)) {
		dev_info(&sdev->dev, "RX waiting on completion\n");
		return 0;
	}
	if (usidev->rx_len == sizeof(usidev->rx_buf) - 1) {
		dev_warn(&sdev->dev, "RX buffer full\n");
		return 0;
	}

	i = min(count, sizeof(usidev->rx_buf) - 1 - usidev->rx_len);
	if (i > 0) {
		memcpy(&usidev->rx_buf[usidev->rx_len], data, i);
		usidev->rx_len += i;
		len += i;
	}
	if (usidev->rx_len >= 3 && strncmp(&usidev->rx_buf[usidev->rx_len - 3], "\r# ", 3) == 0) {
		usidev->rx_len -= 3;
		usidev->rx_buf[usidev->rx_len] = '\0';
		complete(&usidev->prompt_recv_comp);
	} else if (usidev->rx_len > 7 && strstarts(usidev->rx_buf, "+RCV") &&
			strncmp(&usidev->rx_buf[usidev->rx_len - 2], "\r\n", 2) == 0) {
		usidev->rx_buf[usidev->rx_len - 2] = '\0';
		dev_info(&sdev->dev, "RCV event: '%s'\n", usidev->rx_buf + 4);
		usidev->rx_len = 0;
	} else if (usidev->rx_len > 6 && strstarts(usidev->rx_buf, "+TX: ") &&
			strncmp(&usidev->rx_buf[usidev->rx_len - 2], "\r\n", 2) == 0) {
		usidev->rx_buf[usidev->rx_len - 2] = '\0';
		dev_info(&sdev->dev, "TX event: '%s'\n", usidev->rx_buf + 5);
		complete(&usidev->tx_event_recv_comp);
	}

	return len;
}

static const struct serdev_device_ops usi_serdev_client_ops = {
	.receive_buf = usi_receive_buf,
};

static int usi_probe(struct serdev_device *sdev)
{
	struct usi_device *usidev;
	//unsigned long timeout;
	char *resp;
	u8 val;
	int ret;

	dev_info(&sdev->dev, "Probing");

	usidev = devm_kzalloc(&sdev->dev, sizeof(struct usi_device), GFP_KERNEL);
	if (!usidev)
		return -ENOMEM;

	usidev->serdev = sdev;
	usidev->mode = -1;
	init_completion(&usidev->prompt_recv_comp);
	init_completion(&usidev->tx_event_recv_comp);
	serdev_device_set_drvdata(sdev, usidev);

	ret = serdev_device_open(sdev);
	if (ret) {
		dev_err(&sdev->dev, "Failed to open (%d)", ret);
		return ret;
	}

	serdev_device_set_baudrate(sdev, 115200);
	serdev_device_set_flow_control(sdev, false);
	serdev_device_set_client_ops(sdev, &usi_serdev_client_ops);

	ret = usi_cmd_reset(usidev);
	if (ret)
		dev_warn(&sdev->dev, "Reset failed\n");

	ret = usi_send_command(usidev, "ATE=0", NULL, HZ);
	if (ret)
		dev_warn(&sdev->dev, "ATE failed\n");

	/* Dropped in firmware 2.8 */
	ret = usi_send_command(usidev, "ATI", &resp, HZ);
	if (!ret) {
		if (usi_cmd_ok(resp)) {
			resp[strlen(resp) - 6] = '\0';
			dev_info(&sdev->dev, "Firmware '%s'\n", resp);
		}
		kfree(resp);
	}

	ret = usi_send_command(usidev, "AT+DEFMODE", &resp, HZ);
	if (ret) {
		dev_err(&sdev->dev, "Checking DEFMODE failed (%d)\n", ret);
		serdev_device_close(sdev);
		return ret;
	}
	if (usi_cmd_ok(resp)) {
		resp[strlen(resp) - 6] = '\0';
		dev_info(&sdev->dev, "Default mode '%s'\n", resp);
		if (!strcmp(resp, "MFG_WAN_MODE"))
			usidev->mode = 6;
		else if (!strcmp(resp, "MFG_TEST_IDLE"))
			usidev->mode = 0;
		else if (!strcmp(resp, "MFG_TX_TONE"))
			usidev->mode = 1;
		else if (!strcmp(resp, "MFG_TX_PACKET"))
			usidev->mode = 2;
		else if (!strcmp(resp, "MFG_ERROR_LESS_ARGUMENETS"))
			usidev->mode = 3;
		else if (!strcmp(resp, "MFG_TX_TEXT"))
			usidev->mode = 4;
		else if (!strcmp(resp, "MFG_TEST_STOP"))
			usidev->mode = 5;
	}
	kfree(resp);

	if (usidev->mode != 3) {
		ret = usi_simple_cmd(usidev, "AT+DEFMODE=3", HZ);
		if (ret) {
			dev_err(&sdev->dev, "Setting DEFMODE failed (%d)\n", ret);
			serdev_device_close(sdev);
			return ret;
		}

#if 1
		ret = usi_simple_cmd(usidev, "AT+WDCT", 5 * HZ);
		if (ret) {
			dev_err(&sdev->dev, "Writing DCT failed (%d)\n", ret);
			serdev_device_close(sdev);
			return ret;
		}

		ret = usi_cmd_reset(usidev);
		if (ret) {
			dev_err(&sdev->dev, "Reset failed\n");
			serdev_device_close(sdev);
			return ret;
		}

		ret = usi_send_command(usidev, "ATE=0", NULL, HZ);
		if (ret)
			dev_warn(&sdev->dev, "ATE failed\n");
#endif

		usidev->mode = -1;
		ret = usi_send_command(usidev, "AT+DEFMODE", &resp, HZ);
		if (ret) {
			dev_err(&sdev->dev, "Checking DEFMODE failed (%d)\n", ret);
			serdev_device_close(sdev);
			return ret;
		}
		if (usi_cmd_ok(resp)) {
			resp[strlen(resp) - 6] = '\0';
			dev_info(&sdev->dev, "Default mode '%s'\n", resp);
			if (!strcmp(resp, "MFG_WAN_MODE")) {
				usidev->mode = 6;
			}
		}
		kfree(resp);
	}

	ret = usi_send_command(usidev, "AT+VER", &resp, HZ);
	if (!ret) {
		if (usi_cmd_ok(resp)) {
			resp[strlen(resp) - 6] = '\0';
			dev_info(&sdev->dev, "LoRaWAN version '%s'\n",
				(strstarts(resp, "+VER=")) ? (resp + 5) : resp);
		}
		kfree(resp);
	}

	ret = usi_simple_cmd(usidev, "AT+RF=20,868000000,7,0,1,0,8,0,0,0", HZ);
	if (ret) {
		dev_err(&sdev->dev, "AT+RF failed (%d)\n", ret);
		serdev_device_close(sdev);
		return ret;
	}

	/*ret = usi_simple_cmd(usidev, "AT+TXT=1,deadbeef", 2 * HZ);
	if (ret) {
		dev_err(&sdev->dev, "TX failed (%d)\n", ret);
		serdev_device_close(sdev);
		return ret;
	}

	timeout = wait_for_completion_timeout(&usidev->tx_event_recv_comp, 5 * HZ);
	if (!timeout) {
		serdev_device_close(sdev);
		return -ETIMEDOUT;
	}
	usidev->rx_len = 0;
	reinit_completion(&usidev->tx_event_recv_comp);*/

	ret = usi_cmd_read_reg(usidev, 0x42, &val);
	if (!ret) {
		dev_info(&sdev->dev, "SX1272 VERSION 0x%02x\n", (int)val);
	}

	ret = usi_cmd_read_reg(usidev, 0x39, &val);
	if (!ret) {
		dev_info(&sdev->dev, "SX1272 SyncWord 0x%02x\n", (int)val);
	}

	ret = usi_cmd_read_reg(usidev, 0x01, &val);
	if (!ret) {
		dev_info(&sdev->dev, "SX1272 OpMode 0x%02x\n", (int)val);
	}

	dev_info(&sdev->dev, "Done.");

	return 0;
}

static void usi_remove(struct serdev_device *sdev)
{
	struct usi_device *usidev = serdev_device_get_drvdata(sdev);

	usi_send_command(usidev, "ATE=1\r", NULL, HZ);

	serdev_device_close(sdev);

	dev_info(&sdev->dev, "Removed\n");
}

static const struct of_device_id usi_of_match[] = {
	{ .compatible = "usi,wm-sg-sm-42" },
	{}
};
MODULE_DEVICE_TABLE(of, usi_of_match);

static struct serdev_device_driver usi_serdev_driver = {
	.probe = usi_probe,
	.remove = usi_remove,
	.driver = {
		.name = "usi",
		.of_match_table = usi_of_match,
	},
};

static int __init usi_init(void)
{
	int ret;

	ret = serdev_device_driver_register(&usi_serdev_driver);
	if (ret)
		return ret;

	return 0;
}

static void __exit usi_exit(void)
{
	serdev_device_driver_unregister(&usi_serdev_driver);
}

module_init(usi_init);
module_exit(usi_exit);

MODULE_DESCRIPTION("USI WM-SG-SM-42 serdev driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");
