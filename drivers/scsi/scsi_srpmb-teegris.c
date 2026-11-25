/*
 * Secure RPMB Driver for Exynos scsi rpmb
 *
 * Copyright (C) 2016 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/arm-smccc.h>
#include <linux/unaligned.h>
#include <linux/blk-mq.h>
#include <linux/suspend.h>

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_ioctl.h>

#include "scsi_srpmb.h"

#include "../misc/tzdev/tz_iwsock.h"

#define SRPMB_DEVICE_PROPNAME	"samsung,ufs-srpmb"

#define RPMB_SOCKET_NAME	"rpmb_socket"
#define RPMB_REQUEST_MAGIC	0x44444444
#define RPMB_REPLY_MAGIC	0x66666666

#define IOCTL_MAX_RETRY     (5)
#define SMC_SRPMB_WSM       0x82003811
#define SCSI_IOCTL_SECURITY_PROTOCOL_IN 7
#define SCSI_IOCTL_SECURITY_PROTOCOL_OUT 8

struct scsi_srpmb_ctx {
	struct platform_device *pdev;
	struct wakeup_source *wakesrc;
	struct srpmb_request *req;
	dma_addr_t wsm_phyaddr;
	void *wsm_virtaddr;
	spinlock_t lock;
	irq_hw_number_t hwirq;
	int irq;
};

static DECLARE_COMPLETION(scsi_srpmb_sdev_ready);
static struct scsi_device *scsi_srpmb_sdev;
static struct task_struct *scsi_srpmb_kthread;

/* Called when the UFS core has registered the RPMB well-known LUN. */
void samsung_srpmb_device_ready(struct scsi_device *sdev)
{
	if (scsi_srpmb_sdev || scsi_device_get(sdev))
		return;
	scsi_srpmb_sdev = sdev;
	sdev_printk(KERN_INFO, sdev, "SRPMB using UFS RPMB well-known LUN\n");
	complete(&scsi_srpmb_sdev_ready);
}

static int srpmb_scsi_ioctl_helper(struct scsi_device *sdev,
				   struct srpmb_request *req)
{
	bool send = req->cmd == SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
	u32 len = send ? req->outlen : req->inlen;
	u8 cdb[12] = { };
	struct scsi_sense_hdr sense = { };
	struct scsi_failure failure_defs[] = {
		{
			.result = SAM_STAT_CHECK_CONDITION,
			.sense = UNIT_ATTENTION,
			.asc = 0x29,
			.ascq = SCMD_FAILURE_ASCQ_ANY,
			.allowed = IOCTL_MAX_RETRY,
		},
		{ }
	};
	struct scsi_failures failures = { .failure_definitions = failure_defs };
	struct scsi_exec_args args = { .sshdr = &sense, .failures = &failures };
	void *buffer;
	int ret;

	if ((!send && req->cmd != SCSI_IOCTL_SECURITY_PROTOCOL_IN) ||
	    !len || len > RPMB_BUF_MAX_SIZE || len % RPMB_PACKET_SIZE)
		return -EINVAL;
	buffer = kzalloc(len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	if (send) {
		u16 opcode;

		memcpy(buffer, req->rpmb_data, len);
		opcode = get_unaligned_be16((u8 *)buffer + RPMB_REQRES);
		/* Never allow the irreversible RPMB authentication-key operation. */
		if (opcode == AUTHEN_KEY_PROGRAM_REQ) {
			kfree_sensitive(buffer);
			return -EPERM;
		}
	}
	cdb[0] = send ? SECURITY_PROTOCOL_OUT : SECURITY_PROTOCOL_IN;
	cdb[1] = 0xec;
	put_unaligned_be16(1, &cdb[2]);
	put_unaligned_be32(len, &cdb[6]);
	ret = scsi_execute_cmd(sdev, cdb, send ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN,
			       buffer, len, 30 * HZ, IOCTL_MAX_RETRY, &args);
	if (failure_defs[0].retries)
		sdev_printk(KERN_INFO, sdev, "SRPMB reset unit-attention retries: %d\n",
			    failure_defs[0].retries);
	if (ret)
		sdev_printk(KERN_ERR, sdev,
			    "SRPMB %s length %u: result 0x%x sense %02x/%02x/%02x\n",
			    send ? "out" : "in", len, ret,
			    sense.sense_key, sense.asc, sense.ascq);
	if (!ret && !send)
		memcpy(req->rpmb_data, buffer, len);
	kfree_sensitive(buffer);
	return ret ? (ret < 0 ? ret : -EIO) : 0;
}

static void swap_packet(void *src, void *dst)
{
	unsigned int i;
	char *src_buf = (char *)src;
	char *dst_buf = (char *)dst;

	for (i = 0; i < RPMB_PACKET_SIZE; i++)
		dst_buf[i] = src_buf[RPMB_PACKET_SIZE - 1 - i];
}

static void scsi_srpmb_update_status_flag(struct scsi_srpmb_ctx *ctx, unsigned int status)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx->req->status_flag = status;
	spin_unlock_irqrestore(&ctx->lock, flags);
}

static void scsi_srpmb_get_write_counter(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;
	unsigned int status;
	int ret;

	__pm_stay_awake(ctx->wakesrc);

	if (ctx->req->data_len != RPMB_PACKET_SIZE) {
		dev_err(dev, "write counter data len is invalid\n");
		status = WRITE_COUTNER_DATA_LEN_ERROR;
		goto out;
	}

	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
	ctx->req->outlen = RPMB_PACKET_SIZE;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "write counter ioctl read error: %x\n", ret);
		status = WRITE_COUTNER_SECURITY_OUT_ERROR;
		goto out;
	}

	memset(ctx->req->rpmb_data, 0x0, ctx->req->data_len);
	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
	ctx->req->inlen = ctx->req->data_len;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "write counter ioctl write error: %x\n", ret);
		status = WRITE_COUTNER_SECURITY_IN_ERROR;
		goto out;
	}
	if (ctx->req->rpmb_data[RPMB_RESULT] || ctx->req->rpmb_data[RPMB_RESULT+1]) {
		dev_info(dev, "GET_WRITE_COUNTER: REQ/RES = %02x%02x, RESULT = %02x%02x\n",
			 ctx->req->rpmb_data[RPMB_REQRES], ctx->req->rpmb_data[RPMB_REQRES+1],
			 ctx->req->rpmb_data[RPMB_RESULT], ctx->req->rpmb_data[RPMB_RESULT+1]);
	}

	status = RPMB_PASSED;

out:
	scsi_srpmb_update_status_flag(ctx, status);
	__pm_relax(ctx->wakesrc);
}

static void scsi_srpmb_write(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;
	struct rpmb_packet packet;
	unsigned int status;
	int ret;

	__pm_stay_awake(ctx->wakesrc);

	if (ctx->req->data_len < RPMB_PACKET_SIZE || ctx->req->data_len > RPMB_PACKET_SIZE * 64 ||
	    ctx->req->data_len % RPMB_PACKET_SIZE) {
		dev_err(dev, "write data len is invalid\n");
		status = WRITE_DATA_LEN_ERROR;
		goto out;
	}

	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
	ctx->req->outlen = ctx->req->data_len;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "ioctl write data error: %x\n", ret);
		status = WRITE_DATA_SECURITY_OUT_ERROR;
		goto out;
	}

	memset(ctx->req->rpmb_data, 0x0, ctx->req->data_len);
	memset(&packet, 0x0, RPMB_PACKET_SIZE);

	packet.request = RESULT_READ_REQ;
	swap_packet(&packet, ctx->req->rpmb_data);
	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
	ctx->req->outlen = RPMB_PACKET_SIZE;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "ioctl write_data result error: %x\n", ret);
		status = WRITE_DATA_RESULT_SECURITY_OUT_ERROR;
		goto out;
	}

	memset(ctx->req->rpmb_data, 0x0, ctx->req->data_len);
	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
	ctx->req->inlen = RPMB_PACKET_SIZE;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "ioctl write_data result error: %x\n", ret);
		status = WRITE_DATA_SECURITY_IN_ERROR;
		goto out;
	}
	if (ctx->req->rpmb_data[RPMB_RESULT] || ctx->req->rpmb_data[RPMB_RESULT+1]) {
		dev_info(dev, "WRITE_DATA: REQ/RES = %02x%02x, RESULT = %02x%02x\n",
			 ctx->req->rpmb_data[RPMB_REQRES], ctx->req->rpmb_data[RPMB_REQRES+1],
			 ctx->req->rpmb_data[RPMB_RESULT], ctx->req->rpmb_data[RPMB_RESULT+1]);
	}

	status = RPMB_PASSED;

out:
	scsi_srpmb_update_status_flag(ctx, status);
	__pm_relax(ctx->wakesrc);
}

static void scsi_srpmb_read(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;
	unsigned int status;
	int ret;

	__pm_stay_awake(ctx->wakesrc);

	if (ctx->req->data_len < RPMB_PACKET_SIZE || ctx->req->data_len > RPMB_PACKET_SIZE * 64 ||
	    ctx->req->data_len % RPMB_PACKET_SIZE) {
		dev_err(dev, "read data len is invalid\n");
		status = READ_LEN_ERROR;
		goto out;
	}

	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_OUT;
	ctx->req->outlen = RPMB_PACKET_SIZE;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "ioctl read data error: %x\n", ret);
		status = READ_DATA_SECURITY_OUT_ERROR;
		goto out;
	}

	memset(ctx->req->rpmb_data, 0x0, ctx->req->data_len);
	ctx->req->cmd = SCSI_IOCTL_SECURITY_PROTOCOL_IN;
	ctx->req->inlen = ctx->req->data_len;

	ret = srpmb_scsi_ioctl_helper(scsi_srpmb_sdev, ctx->req);
	if (ret < 0) {
		dev_err(dev, "ioctl result read data error : %x\n", ret);
		status = READ_DATA_SECURITY_IN_ERROR;
		goto out;
	}
	if (ctx->req->rpmb_data[RPMB_RESULT] || ctx->req->rpmb_data[RPMB_RESULT+1]) {
		dev_info(dev, "READ_DATA: REQ/RES = %02x%02x, RESULT = %02x%02x\n",
			 ctx->req->rpmb_data[RPMB_REQRES], ctx->req->rpmb_data[RPMB_REQRES+1],
			 ctx->req->rpmb_data[RPMB_RESULT], ctx->req->rpmb_data[RPMB_RESULT+1]);
	}


	status = RPMB_PASSED;

out:
	scsi_srpmb_update_status_flag(ctx, status);
	__pm_relax(ctx->wakesrc);
}

static struct sock_desc *scsi_srpmb_accept_swd_connection(struct device *dev)
{
	int ret;
	struct sock_desc *srpmb_conn;
	struct sock_desc *srpmb_listen;

	srpmb_listen = tz_iwsock_socket(1);
	if (IS_ERR(srpmb_listen)) {
		dev_err(dev, "failed to create iwd socket, err = %ld\n", PTR_ERR(srpmb_listen));
		return srpmb_listen;
	}
	dev_info(dev, "Created socket\n");

	ret = tz_iwsock_listen(srpmb_listen, RPMB_SOCKET_NAME);
	if (ret) {
		dev_err(dev, "failed make iwd socket listening, err = %d\n", ret);
		srpmb_conn = ERR_PTR(ret);
		goto out;
	}
	dev_info(dev, "Make socket listening\n");

	srpmb_conn = tz_iwsock_accept(srpmb_listen);
	if (IS_ERR(srpmb_conn)) {
		dev_err(dev, "failed to accept connection, err = %ld\n", PTR_ERR(srpmb_conn));
		goto out;
	}
	dev_info(dev, "Accepted connection\n");

out:
	tz_iwsock_release(srpmb_listen);

	return srpmb_conn;
}

static void scsi_srpmb_release_connection(struct sock_desc *srpmb_conn)
{
	tz_iwsock_release(srpmb_conn);
}

static int scsi_srpmb_send_reply(struct sock_desc *srpmb_conn, struct device *dev)
{
	unsigned int sock_data = RPMB_REPLY_MAGIC;
	ssize_t len;
	int ret;

	len = tz_iwsock_write(srpmb_conn, &sock_data, sizeof(sock_data), 0);
	if (len != sizeof(sock_data)) {
		ret = len >= 0 ? -EMSGSIZE : len;
		dev_err(dev, "failed to send reply, err = %d\n", ret);
		return ret;
	} else {
		dev_info(dev, "Sent reply\n");
	}

	return 0;
}

static int scsi_srpmb_wait_request(struct sock_desc *srpmb_conn, struct device *dev)
{
	int ret;
	ssize_t len;
	unsigned int sock_data;

	len = tz_iwsock_read(srpmb_conn, &sock_data, sizeof(sock_data), 0);
	if (len > 0 && len != sizeof(sock_data)) {
		dev_err(dev, "failed to receive request, invalid len = %zd\n", len);
		return -EMSGSIZE;
	} else if (!len) {
		dev_err(dev, "connection was reset by peer\n");
		return -ECONNRESET;
	} else if (len < 0) {
		ret = len;
		dev_err(dev, "error while receiving request from SWd, err = %u\n", ret);
		return ret;
	}

	if (sock_data != RPMB_REQUEST_MAGIC) {
		dev_err(dev, "received invalid request, data = %u\n", sock_data);
		return -EINVAL;
	}

	return 0;
}

static int scsi_srpmb_kthread_work(void *data)
{
	struct scsi_srpmb_ctx *ctx = (struct scsi_srpmb_ctx *)data;
	struct device *dev = &ctx->pdev->dev;
	struct sock_desc *srpmb_conn;
	int ret;

	srpmb_conn = scsi_srpmb_accept_swd_connection(dev);
	if (IS_ERR(srpmb_conn))
		return PTR_ERR(srpmb_conn);

	if (!wait_for_completion_timeout(&scsi_srpmb_sdev_ready, msecs_to_jiffies(5000))) {
		dev_err(dev, "timeout occurred while waiting for scsi device\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	while (!kthread_should_stop()) {
		ret = scsi_srpmb_wait_request(srpmb_conn, dev);
		if (ret) {
			dev_err(dev, "Failed to wait request, ret = %d\n", ret);
			goto out;
		}

		switch(ctx->req->type) {
		case GET_WRITE_COUNTER:
			scsi_srpmb_get_write_counter(ctx);
			break;
		case WRITE_DATA:
			scsi_srpmb_write(ctx);
			break;
		case READ_DATA:
			scsi_srpmb_read(ctx);
			break;
		default:
			dev_err(dev, "Received unsupported request: %x\n", ctx->req->type);
			scsi_srpmb_update_status_flag(ctx, RPMB_INVALID_COMMAND);
			break;
		}

		ret = scsi_srpmb_send_reply(srpmb_conn, dev);
		if (ret)
			dev_err(dev, "Failed to send reply, ret = %d\n", ret);
	}

out:
	scsi_srpmb_release_connection(srpmb_conn);

	return ret;
}

static struct scsi_srpmb_ctx *scsi_srpmb_create_ctx(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scsi_srpmb_ctx *ctx;

	ctx = kzalloc(sizeof(struct scsi_srpmb_ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(dev, "Fail to alloc for scsi rpmb context\n");
		return ERR_PTR(-ENOMEM);
	}

	ctx->pdev = pdev;

	spin_lock_init(&ctx->lock);
	ctx->wakesrc = wakeup_source_register(dev, "srpmb");
	if (!ctx->wakesrc) {
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	return ctx;
}

static void scsi_srpmb_destroy_ctx(struct scsi_srpmb_ctx *ctx)
{
	wakeup_source_unregister(ctx->wakesrc);
	kfree(ctx);
}

static int scsi_srpmb_init_wsm(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;

	ctx->wsm_virtaddr = dma_alloc_coherent(dev,
			sizeof(struct srpmb_request) + RPMB_BUF_MAX_SIZE,
			&ctx->wsm_phyaddr, GFP_KERNEL);
	if (!ctx->wsm_virtaddr) {
		dev_err(dev, "Fail to alloc for srpmb wsm (world shared memory)\n");
		return -ENOMEM;
	}

	ctx->req = ctx->wsm_virtaddr;

	return 0;
}

static void scsi_srpmb_release_wsm(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;

	dma_free_coherent(dev, sizeof(struct srpmb_request) + RPMB_BUF_MAX_SIZE,
			  ctx->wsm_virtaddr, ctx->wsm_phyaddr);
}

static irqreturn_t scsi_srpmb_interrupt(int intr, void *arg)
{
	return IRQ_HANDLED;
}

static int scsi_srpmb_init_irq(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;
	struct irq_data *rpmb_irqd;
	int ret;

	ctx->irq = irq_of_parse_and_map(dev->of_node, 0);
	if (ctx->irq <= 0) {
		dev_err(dev, "Fail to get irq number for scsi rpmb\n");
		return -ENOENT;
	}
	dev_info(dev, "Requested irq resource\n");

	/* Get irq_data from irq number */
	rpmb_irqd = irq_get_irq_data(ctx->irq);
	if (!rpmb_irqd) {
		dev_err(dev, "Fail to get irq_data from irq number\n");
		return -ENOENT;
	}
	dev_info(dev, "Requested irq data\n");

	/* Get hwirq from irq_data */
	ctx->hwirq = irqd_to_hwirq(rpmb_irqd);

	ret = request_irq(ctx->irq, scsi_srpmb_interrupt,
			IRQF_TRIGGER_RISING, ctx->pdev->name, ctx);
	if (ret) {
		dev_err(dev, "Fail to request irq handler for scsi srpmb, error=%d\n", ret);
		return ret;
	}
	dev_info(dev, "Requested irq %d\n", ctx->irq);

	return 0;
}

static void scsi_srpmb_release_irq(struct scsi_srpmb_ctx *ctx)
{
	free_irq(ctx->irq, ctx);
}

static int scsi_srpmb_register_resources(struct scsi_srpmb_ctx *ctx)
{
	struct device *dev = &ctx->pdev->dev;
	int ret;

	/* Smc call to transfer wsm address to secure world */
	struct arm_smccc_res res;

	/* Samsung's exynos_smc() issues a DSB before the monitor call. */
	asm volatile("dsb sy" ::: "memory");
	arm_smccc_smc(SMC_SRPMB_WSM, ctx->wsm_phyaddr, ctx->hwirq, 0,
		      0, 0, 0, 0, &res);
	ret = (int)res.a0;
	if (ret)
		dev_err(dev, "Failed to register shared memory: %d\n", ret);
	else
		dev_info(dev, "Registered WSM\n");

	return ret;
}

static int scsi_srpmb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scsi_srpmb_ctx *ctx;
	int ret;

	BUILD_BUG_ON(sizeof(struct rpmb_packet) != RPMB_PACKET_SIZE);

	ctx = scsi_srpmb_create_ctx(pdev);
	if (IS_ERR(ctx)) {
		dev_err(dev, "Fail to alloc context: error=%ld\n", PTR_ERR(ctx));
		return PTR_ERR(ctx);
	}

	ret = scsi_srpmb_init_wsm(ctx);
	if (ret) {
		dev_err(dev, "Fail to initialize wsm\n");
		goto destroy_ctx;
	}

	ret = scsi_srpmb_init_irq(ctx);
	if (ret) {
		dev_err(dev, "Fail to initialize irq\n");
		goto release_wsm;
	}

	ret = scsi_srpmb_register_resources(ctx);
	if (ret) {
		dev_err(dev, "Fail to register resources\n");
		goto release_irq;
	}

	platform_set_drvdata(pdev, ctx);
	dev_info(dev, "Published  context\n");

	scsi_srpmb_kthread = kthread_run(scsi_srpmb_kthread_work, ctx, "scsi_srpmb_worker");
	if (IS_ERR(scsi_srpmb_kthread)) {
		/* Here we can't release IRQ or WSM due to ATF
		 * doesn't support such option, so system can crash
		 * if we release IRQ or WSM here without releasing it in ATF */
		return PTR_ERR(scsi_srpmb_kthread);
	}

	return 0;

release_irq:
	scsi_srpmb_release_irq(ctx);
release_wsm:
	scsi_srpmb_release_wsm(ctx);
destroy_ctx:
	scsi_srpmb_destroy_ctx(ctx);

	return ret;
}

static void scsi_srpmb_remove(struct platform_device *pdev)
{
	struct scsi_srpmb_ctx *ctx = platform_get_drvdata(pdev);

	kthread_stop(scsi_srpmb_kthread);

	scsi_srpmb_release_irq(ctx);
	scsi_srpmb_release_wsm(ctx);
	scsi_srpmb_destroy_ctx(ctx);
}

static const struct of_device_id of_match_table[] = {
	{ .compatible = SRPMB_DEVICE_PROPNAME },
	{ }
};

static struct platform_driver scsi_srpmb_plat_driver = {
	.probe = scsi_srpmb_probe,
	.driver = {
		.name = "exynos-ufs-srpmb",
		.owner = THIS_MODULE,
		.suppress_bind_attrs = true,
		.of_match_table = of_match_table,
	},
	.remove = scsi_srpmb_remove,
};

static int __init scsi_srpmb_init(void)
{
	return platform_driver_register(&scsi_srpmb_plat_driver);
}

static void __exit scsi_srpmb_exit(void)
{
	platform_driver_unregister(&scsi_srpmb_plat_driver);
}

/* Register shared memory early; tz_iwsock_listen waits for TZDEV readiness. */
subsys_initcall(scsi_srpmb_init);
module_exit(scsi_srpmb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UFS SRPMB driver");
