// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  linux/drivers/mmc/sdio_ops.c
 *
 *  Copyright 2006-2007 Pierre Ossman
 */

#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>

#include "core.h"
#include "sdio_ops.h"

int mmc_send_io_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd = {};
	int i, err = 0;

	pr_info("sdio-ops: I am in mmc_send_io_op_cond.\n");
	cmd.opcode = SD_IO_SEND_OP_COND;
	cmd.arg = ocr;
	cmd.flags = MMC_RSP_SPI_R4 | MMC_RSP_R4 | MMC_CMD_BCR;

	for (i = 100; i; i--) {
		err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
		if (err)
			break;

		/* if we're just probing, do a single pass */
		if (ocr == 0) {
			pr_info("sdio_ops: Single pass because we're probing, ocr==0;\
 in mmc_send_io_op_cond.\n");
			break;
		}

		/* otherwise wait until reset completes */
		if (mmc_host_is_spi(host)) {
			/*
			 * Both R1_SPI_IDLE and MMC_CARD_BUSY indicate
			 * an initialized card under SPI, but some cards
			 * (Marvell's) only behave when looking at this
			 * one.
			 */
			if (cmd.resp[1] & MMC_CARD_BUSY) {
				pr_info("sdio_ops: cmd.resp[1] has MMC_CARD_BUSY, so spi card is initialized;\
exiting loop in mmc_send_io_op_cond.\n");
				break;
			}
		} else {
			if (cmd.resp[0] & MMC_CARD_BUSY) {
				pr_info("sdio_ops: cmd.resp[0] has MMC_CARD_BUSY, so non-spi card is initialized;\
exiting loop in mmc_send_io_op_cond.\n");
				break;
			}
		}
		
		pr_info("sdio_ops: Ran out of i loops in mmc_send_io_op_cond. Returning -ETIMEDOUT.\
Executing mmc_delay(10).");
		err = -ETIMEDOUT;
		// i need to learn ftrace
		// it maybe breaks this kernel . . .
		//increase?
		mmc_delay(10);
	}

	if (rocr) {
		pr_info("sdio_ops: rocr detected in mmc_send_io_op_cond.\
Assigning *rocr = cmd.resp[1 or 0] based on spi.\n");
		*rocr = cmd.resp[mmc_host_is_spi(host) ? 1 : 0];
	}
	pr_err("sdio_ops: End of function, returning %d", err);
	return err;
}

static int mmc_io_rw_direct_host(struct mmc_host *host, int write, unsigned fn,
	unsigned addr, u8 in, u8 *out)
{
	struct mmc_command cmd = {};
	int err;

	if (fn > 7) {
		pr_err("sdio_ops: fn > 7 in mmc_io_rw_direct_host, returning -EINVAL\n");
		return -EINVAL;
	}
	
	/* sanity check */
	if (addr & ~0x1FFFF) {  // all 1s
		pr_err("sdio_ops: addr & ~0x1FFFF in mmc_io_rw_direct_host,\
		returning -EINVAL.\n");
		return -EINVAL;
	}
	
	//check these?
	cmd.opcode = SD_IO_RW_DIRECT; 
	cmd.arg = write ? 0x80000000 : 0x00000000;
	cmd.arg |= fn << 28;	//this
	cmd.arg |= (write && out) ? 0x08000000 : 0x00000000;
	cmd.arg |= addr << 9;	//this
	cmd.arg |= in;
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(host, &cmd, 0); 
	if (err) {
		pr_err("sdio_ops: err = mmc_wait_for_cmd returned, in \
mmc_io_rw_direct_host, returning err=%d", err);
		return err;
	}
	
	if (mmc_host_is_spi(host)) {
		pr_info("sdio_ops: mmc_host_is_spi, errors already reported\n.");
		/* host driver already reported errors */
	} else {
		if (cmd.resp[0] & R5_ERROR) {
			pr_err("sdio_ops: R5_ERROR. Returning -EIO in io_rw_direct_host. The relevant cmd.resp = %08x .\n", cmd.resp[0]);
			return -EIO;
		}
		if (cmd.resp[0] & R5_FUNCTION_NUMBER) {
			pr_err("sdio_ops: R5_FUNCTION_NUMBER. Returning -EINVAL in io_rw_direct_host.\n");
			return -EINVAL;
		}
			
		if (cmd.resp[0] & R5_OUT_OF_RANGE) {
			pr_err("sdio_ops: R5_OUT_OF_RANGE. Returning -ERANGE in io_rw_direct_host.\n");
			return -ERANGE;
		}
	}

	if (out) {
		pr_info("sdio_ops: out is true in mmc_io_rw_direct_host\n");
		if (mmc_host_is_spi(host)) {
			pr_info("sdio_ops: host_is_spi, Assigning out in mmc_io_rw_direct_host out condition\n");
			*out = (cmd.resp[0] >> 8) & 0xFF;
		}
		else {
			pr_info("sdio_ops: host is not spi. Assigning out.\n");
			*out = cmd.resp[0] & 0xFF;
		}
	}

	return 0;
}

int mmc_io_rw_direct(struct mmc_card *card, int write, unsigned fn,
	unsigned addr, u8 in, u8 *out)
{
	return mmc_io_rw_direct_host(card->host, write, fn, addr, in, out);
}

int mmc_io_rw_extended(struct mmc_card *card, int write, unsigned fn,
	unsigned addr, int incr_addr, u8 *buf, unsigned blocks, unsigned blksz)
{
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_data data = {};
	struct scatterlist sg, *sg_ptr;
	struct sg_table sgtable;
	unsigned int nents, left_size, i;
	unsigned int seg_size = card->host->max_seg_size;
	int err;

	WARN_ON(blksz == 0);

	/* sanity check */
	if (addr & ~0x1FFFF)
		return -EINVAL;

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_IO_RW_EXTENDED;
	cmd.arg = write ? 0x80000000 : 0x00000000;
	cmd.arg |= fn << 28;
	cmd.arg |= incr_addr ? 0x04000000 : 0x00000000;
	cmd.arg |= addr << 9;
	if (blocks == 0)
		cmd.arg |= (blksz == 512) ? 0 : blksz;	/* byte mode */
	else
		cmd.arg |= 0x08000000 | blocks;		/* block mode */
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

	data.blksz = blksz;
	/* Code in host drivers/fwk assumes that "blocks" always is >=1 */
	data.blocks = blocks ? blocks : 1;
	data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;

	left_size = data.blksz * data.blocks;
	nents = DIV_ROUND_UP(left_size, seg_size);
	if (nents > 1) {
		if (sg_alloc_table(&sgtable, nents, GFP_KERNEL))
			return -ENOMEM;

		data.sg = sgtable.sgl;
		data.sg_len = nents;

		for_each_sg(data.sg, sg_ptr, data.sg_len, i) {
			sg_set_buf(sg_ptr, buf + i * seg_size,
				   min(seg_size, left_size));
			left_size -= seg_size;
		}
	} else {
		data.sg = &sg;
		data.sg_len = 1;

		sg_init_one(&sg, buf, left_size);
	}

	mmc_set_data_timeout(&data, card);

	mmc_pre_req(card->host, &mrq);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error)
		err = cmd.error;
	else if (data.error)
		err = data.error;
	else if (mmc_host_is_spi(card->host))
		/* host driver already reported errors */
		err = 0;
	else if (cmd.resp[0] & R5_ERROR)
		err = -EIO;
	else if (cmd.resp[0] & R5_FUNCTION_NUMBER)
		err = -EINVAL;
	else if (cmd.resp[0] & R5_OUT_OF_RANGE)
		err = -ERANGE;
	else
		err = 0;

	mmc_post_req(card->host, &mrq, err);

	if (nents > 1)
		sg_free_table(&sgtable);

	return err;
}

int sdio_reset(struct mmc_host *host)
{
	int ret;
	u8 abort;

	/* SDIO Simplified Specification V2.0, 4.4 Reset for SDIO */

	ret = mmc_io_rw_direct_host(host, 0, 0, SDIO_CCCR_ABORT, 0, &abort);
	if (ret)
		abort = 0x08;
	else
		abort |= 0x08;

	return mmc_io_rw_direct_host(host, 1, 0, SDIO_CCCR_ABORT, abort, NULL);
}

