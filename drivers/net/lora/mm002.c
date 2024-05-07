// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nemeus MM002
 *
 * Copyright (c) 2017-2018 Andreas Färber
 */

#include <linux/delay.h>
#include <linux/lora.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/lora/dev.h>

struct mm002_device {
	struct serdev_device *serdev;

	char rx_buf[4096];
	int rx_len;

	struct completion resp_recv_comp;
};

static int mm002_send_command(struct mm002_device *mmdev, const char *cmd, char **data, unsigned long timeout)
{
	struct serdev_device *sdev = mmdev->serdev;
	const char *crlf = "\r\n";
	char *resp;

	serdev_device_write_buf(sdev, cmd, strlen(cmd));
	serdev_device_write_buf(sdev, crlf, 2);

	timeout = wait_for_completion_timeout(&mmdev->resp_recv_comp, timeout);
	if (!timeout)
		return -ETIMEDOUT;

	resp = mmdev->rx_buf;
	dev_dbg(&sdev->dev, "Received: '%s'\n", resp);
	if (data)
		*data = kstrdup(resp, GFP_KERNEL);

	mmdev->rx_len = 0;
	reinit_completion(&mmdev->resp_recv_comp);

	return 0;
}

/*static int mm002_simple_cmd(struct mm002_device *mmdev, const char *cmd, unsigned long timeout)
{
	char *resp;
	int ret;

	ret = mm002_send_command(mmdev, cmd, &resp, timeout);
	if (ret)
		return ret;

	if (strncmp(resp, "OK", 2) == 0) {
		kfree(resp);
		return 0;
	}

	kfree(resp);

	return -EINVAL;
}*/

static int mm002_get_version(struct mm002_device *mmdev, char **version, unsigned long timeout)
{
	char *resp;
	int ret, len;

	ret = mm002_send_command(mmdev, "AT+DEBUG=MVER", &resp, timeout);
	if (ret)
		return ret;

	len = strlen(resp);
	if (len < 4 || strcmp(resp + len - 4, "\r\nOK") != 0) {
		kfree(resp);
		return -EINVAL;
	}

	if (strncmp(resp, "+DEBUG: MVER,", 13) == 0) {
		*version = kstrndup(resp + 13, len - 13 - 4, GFP_KERNEL);
		kfree(resp);
		return 0;
	}

	kfree(resp);

	return -EINVAL;
}

static void mm002_handle_reset(struct mm002_device *mmdev, const char *version)
{
	dev_warn(&mmdev->serdev->dev, "reset (%s)\n", version);
}

static int mm002_receive_buf(struct serdev_device *sdev, const u8 *data, size_t count)
{
	struct mm002_device *mmdev = serdev_device_get_drvdata(sdev);
	size_t i = 0;
	int len = 0;

	dev_dbg(&sdev->dev, "Receive (%d)\n", (int)count);

	for (i = 0; i < count; i++) {
		dev_dbg(&sdev->dev, "Receive: 0x%02x\n", (int)data[i]);
	}

	if (completion_done(&mmdev->resp_recv_comp)) {
		dev_info(&sdev->dev, "RX waiting on completion\n");
		return 0;
	}
	if (mmdev->rx_len == sizeof(mmdev->rx_buf) - 1) {
		dev_warn(&sdev->dev, "RX buffer full\n");
		return 0;
	}

	i = min(count, sizeof(mmdev->rx_buf) - 1 - mmdev->rx_len);
	if (i > 0) {
		memcpy(&mmdev->rx_buf[mmdev->rx_len], data, i);
		mmdev->rx_len += i;
		len += i;
	}
	if (mmdev->rx_len > 2 && strncmp(&mmdev->rx_buf[mmdev->rx_len - 2], "\r\n", 2) == 0) {
		if (mmdev->rx_len == 3 && mmdev->rx_buf[0] == '\0') {
			memmove(mmdev->rx_buf, mmdev->rx_buf + 1, mmdev->rx_len - 1);
			mmdev->rx_len--;
			dev_dbg(&sdev->dev, "dropped leading nul char, assuming from reset\n");
		}
	}
	if (mmdev->rx_len > 16 && mmdev->rx_buf[mmdev->rx_len - 1] == '\n' &&
	    strstarts(mmdev->rx_buf, "\r\n+DEBUG: START,")) {
		mmdev->rx_buf[mmdev->rx_len - 1] = '\0';
		mm002_handle_reset(mmdev, mmdev->rx_buf + 16);
		mmdev->rx_len = 0;
	}
	if ((mmdev->rx_len >= 6 && strncmp(&mmdev->rx_buf[mmdev->rx_len - 6], "\r\nOK\r\n", 6) == 0) ||
	    (mmdev->rx_len >= 9 && strncmp(&mmdev->rx_buf[mmdev->rx_len - 9], "\r\nERROR\r\n", 9) == 0)) {
		mmdev->rx_len -= 2;
		if (strncmp(mmdev->rx_buf, "\r\n", 2) == 0) {
			memmove(mmdev->rx_buf, mmdev->rx_buf + 2, mmdev->rx_len - 2);
			mmdev->rx_len -= 2;
		} else
			dev_warn(&sdev->dev, "response unexpectedly does not start with CRLF\n");
		mmdev->rx_buf[mmdev->rx_len] = '\0';
		complete(&mmdev->resp_recv_comp);
	}

	return len;
}

static const struct serdev_device_ops mm002_serdev_client_ops = {
	.receive_buf = mm002_receive_buf,
};

static int mm002_probe(struct serdev_device *sdev)
{
	struct mm002_device *mmdev;
	char *sz;
	int ret;

	dev_info(&sdev->dev, "Probing\n");

	mmdev = devm_kzalloc(&sdev->dev, sizeof(struct mm002_device), GFP_KERNEL);
	if (!mmdev)
		return -ENOMEM;

	mmdev->serdev = sdev;
	init_completion(&mmdev->resp_recv_comp);
	serdev_device_set_drvdata(sdev, mmdev);

	ret = serdev_device_open(sdev);
	if (ret) {
		dev_err(&sdev->dev, "Failed to open (%d)\n", ret);
		return ret;
	}

	serdev_device_set_baudrate(sdev, 38400);
	serdev_device_set_flow_control(sdev, false);
	serdev_device_set_client_ops(sdev, &mm002_serdev_client_ops);

	ret = mm002_get_version(mmdev, &sz, HZ);
	if (ret) {
		dev_err(&sdev->dev, "Failed to get version (%d)\n", ret);
		serdev_device_close(sdev);
		return ret;
	}

	dev_info(&sdev->dev, "firmware version: %s\n", sz);
	kfree(sz);

	dev_info(&sdev->dev, "Done.\n");

	return 0;
}

static void mm002_remove(struct serdev_device *sdev)
{
	serdev_device_close(sdev);

	dev_info(&sdev->dev, "Removed\n");
}

static const struct of_device_id mm002_of_match[] = {
	{ .compatible = "nemeus,mm002" },
	{}
};
MODULE_DEVICE_TABLE(of, mm002_of_match);

static struct serdev_device_driver mm002_serdev_driver = {
	.probe = mm002_probe,
	.remove = mm002_remove,
	.driver = {
		.name = "mm002",
		.of_match_table = mm002_of_match,
	},
};

static int __init mm002_init(void)
{
	int ret;

	ret = serdev_device_driver_register(&mm002_serdev_driver);
	if (ret)
		return ret;

	return 0;
}

static void __exit mm002_exit(void)
{
	serdev_device_driver_unregister(&mm002_serdev_driver);
}

module_init(mm002_init);
module_exit(mm002_exit);

MODULE_DESCRIPTION("Nemeus MM002 serdev driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");
