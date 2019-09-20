/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/bootmem.h>
#include <linux/bug.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-iommu.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/barrier.h>
#ifndef CONFIG_ARM64
#include <asm/dma-iommu.h>
#endif
#include <soc/mediatek/smi.h>

#include "mtk_iommu.h"

#include "pseudo_m4u.h" /* For security dump and irq-callback.*/

#define REG_MMU_PT_BASE_ADDR			0x000

#define REG_MMU_INVALIDATE			0x020
#define F_ALL_INVLD				0x2
#define F_MMU_INV_RANGE				0x1

#define REG_MMU_INVLD_START_A			0x024
#define REG_MMU_INVLD_END_A			0x028

#define REG_MMU_INV_SEL				0x038
#define F_INVLD_EN0				BIT(0)
#define F_INVLD_EN1				BIT(1)

#define REG_MMU_STANDARD_AXI_MODE		0x048
#define REG_MMU_DCM_DIS				0x050
#define REG_MMU_WR_LEN_CTRL			0x054
#define F_MMU_MMU0_WR_THROT_DIS			BIT(5)
#define F_MMU_MMU1_WR_THROT_DIS			BIT(21)

#define REG_MMU_CTRL_REG			0x110
#define F_MMU_PREFETCH_RT_REPLACE_MOD		BIT(4)
/* The TF-protect-select is bit[5:4] in mt2712 while it's bit[6:5] in mt8173.*/
#define F_MMU_TF_PROTECT_SEL(prot)		(((prot) & 0x3) << 5)
#define F_MMU_TF_PROT_SEL(prot)			(((prot) & 0x3) << 4)

#define REG_MMU_IVRP_PADDR			0x114
#define F_MMU_IVRP_PA_SET(pa, ext)		((pa) | (!!(ext)))

#define REG_MMU_INT_CONTROL0			0x120
#define F_L2_MULIT_HIT_EN			BIT(0)
#define F_TABLE_WALK_FAULT_INT_EN		BIT(1)
#define F_PREETCH_FIFO_OVERFLOW_INT_EN		BIT(2)
#define F_MISS_FIFO_OVERFLOW_INT_EN		BIT(3)
#define F_PREFETCH_FIFO_ERR_INT_EN		BIT(5)
#define F_MISS_FIFO_ERR_INT_EN			BIT(6)
#define F_INT_CLR_BIT				BIT(12)

#define REG_MMU_INT_MAIN_CONTROL		0x124 /* below: mmu0 | mmu1 */
#define F_INT_TRANSLATION_FAULT			(BIT(0) | BIT(7))
#define F_INT_MAIN_MULTI_HIT_FAULT		(BIT(1) | BIT(8))
#define F_INT_INVALID_PA_FAULT			(BIT(2) | BIT(9))
#define F_INT_ENTRY_REPLACEMENT_FAULT		(BIT(3) | BIT(10))
#define F_INT_TLB_MISS_FAULT			(BIT(4) | BIT(11))
#define F_INT_MISS_TRANSACTION_FIFO_FAULT	(BIT(5) | BIT(12))
#define F_INT_PRETETCH_TRANSATION_FIFO_FAULT	(BIT(6) | BIT(13))

#define REG_MMU_CPE_DONE			0x12C

#define REG_MMU_FAULT_ST1			0x134
#define F_REG_MMU0_FAULT_MASK			GENMASK(6, 0)
#define F_REG_MMU1_FAULT_MASK			GENMASK(13, 7)

#define REG_MMU0_FAULT_VA			0x13c
#define F_MMU_FAULT_VA_WRITE_BIT		BIT(1)
#define F_MMU_FAULT_VA_LAYER_BIT		BIT(0)

#define REG_MMU0_INVLD_PA			0x140
#define REG_MMU1_FAULT_VA			0x144
#define REG_MMU1_INVLD_PA			0x148
#define REG_MMU0_INT_ID				0x150
#define REG_MMU1_INT_ID				0x154
#define F_MMU_INT_ID_LARB_ID(a)		(((a) >> 7) & 0x7)
#define F_MMU_INT_ID_PORT_ID(a)		(((a) >> 2) & 0x1f)

#define MTK_PROTECT_PA_ALIGN			128

/*
 * Get the local arbiter ID and the portid within the larb arbiter
 * from mtk_m4u_id which is defined by MTK_M4U_ID.
 */
#define MTK_M4U_TO_LARB(id)		(((id) >> 5) & 0xf)
#define MTK_M4U_TO_PORT(id)		((id) & 0x1f)

struct mtk_iommu_domain {
	spinlock_t			pgtlock; /* lock for page table */

	struct io_pgtable_cfg		cfg;
	struct io_pgtable_ops		*iop;

	struct iommu_domain		domain;
};

static struct iommu_ops mtk_iommu_ops;
static const struct of_device_id mtk_iommu_of_ids[];

static struct mtk_iommu_domain *to_mtk_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct mtk_iommu_domain, domain);
}

static unsigned long mtk_iommu_pgt_base;

unsigned long mtk_get_pgt_base(void)
{
	return mtk_iommu_pgt_base;
}
EXPORT_SYMBOL(mtk_get_pgt_base);

static void mtk_iommu_tlb_flush_all(void *cookie)
{
	struct mtk_iommu_data *data = cookie;

	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0, data->base + REG_MMU_INV_SEL);
	writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
	wmb(); /* Make sure the tlb flush all done */
}

static void mtk_iommu_tlb_add_flush_nosync(unsigned long iova, size_t size,
					   size_t granule, bool leaf,
					   void *cookie)
{
	struct mtk_iommu_data *data = cookie;

	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0, data->base + REG_MMU_INV_SEL);

	writel_relaxed(iova, data->base + REG_MMU_INVLD_START_A);
	writel_relaxed(iova + size - 1, data->base + REG_MMU_INVLD_END_A);
	writel_relaxed(F_MMU_INV_RANGE, data->base + REG_MMU_INVALIDATE);
}

static void mtk_iommu_tlb_sync(void *cookie)
{
	struct mtk_iommu_data *data = cookie;
	int ret;
	u32 tmp;

	ret = readl_poll_timeout_atomic(data->base + REG_MMU_CPE_DONE, tmp,
					tmp != 0, 10, 100000);
	if (ret) {
		dev_warn(data->dev,
			 "Partial TLB flush timed out, falling back to full flush\n");
		mtk_iommu_tlb_flush_all(cookie);
	}
	/* Clear the CPE status */
	writel_relaxed(0, data->base + REG_MMU_CPE_DONE);
}

static const struct iommu_gather_ops mtk_iommu_gather_ops = {
	.tlb_flush_all = mtk_iommu_tlb_flush_all,
	.tlb_add_flush = mtk_iommu_tlb_add_flush_nosync,
	.tlb_sync = mtk_iommu_tlb_sync,
};

static irqreturn_t mtk_iommu_isr(int irq, void *dev_id)
{
	struct mtk_iommu_data *data = dev_id;
	struct mtk_iommu_domain *dom = data->m4u_dom;
	u32 int_state, regval, fault_iova, fault_pa;
	unsigned int fault_larb, fault_port;
	bool layer, write;

	/* Read error info from registers */
	int_state = readl_relaxed(data->base + REG_MMU_FAULT_ST1);
	if (int_state & F_REG_MMU0_FAULT_MASK) {
		regval = readl_relaxed(data->base + REG_MMU0_INT_ID);
		fault_iova = readl_relaxed(data->base + REG_MMU0_FAULT_VA);
		fault_pa = readl_relaxed(data->base + REG_MMU0_INVLD_PA);
	} else {
		regval = readl_relaxed(data->base + REG_MMU1_INT_ID);
		fault_iova = readl_relaxed(data->base + REG_MMU1_FAULT_VA);
		fault_pa = readl_relaxed(data->base + REG_MMU1_INVLD_PA);
	}
	layer = fault_iova & F_MMU_FAULT_VA_LAYER_BIT;
	write = fault_iova & F_MMU_FAULT_VA_WRITE_BIT;
	fault_larb = F_MMU_INT_ID_LARB_ID(regval);
	fault_port = F_MMU_INT_ID_PORT_ID(regval);

	/* bdpsys larb remap. */
	if (data->match_type == m4u_mt8695 && fault_larb == 2)
		fault_larb = 8;

	if (report_iommu_fault(&dom->domain, data->dev, fault_iova,
			       write ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ)) {
		dev_err_ratelimited(
			data->dev,
			"fault type=0x%x iova=0x%x pa=0x%x larb=%d port=%d layer=%d %s sec 0x%x 0x%x\n",
			int_state, fault_iova, fault_pa, fault_larb, fault_port,
			layer, write ? "write" : "read"
			#ifdef CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT
			, m4u_dump_secpgd(fault_larb, fault_port, fault_iova)
			#else
			, 0
			#endif
			, mtk_iommu_translation_fault_cb(fault_larb, fault_port,
							 fault_iova)
			);
	}

	/* Interrupt clear */
	regval = readl_relaxed(data->base + REG_MMU_INT_CONTROL0);
	regval |= F_INT_CLR_BIT;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL0);

	mtk_iommu_tlb_flush_all(data);

	return IRQ_HANDLED;
}

static void mtk_iommu_config(struct mtk_iommu_data *data,
			     struct device *dev, bool enable)
{
	struct mtk_iommu_client_priv *head, *cur, *next;
	struct mtk_smi_larb_iommu    *larb_mmu;
	unsigned int                 larbid, portid;

	head = dev->archdata.iommu;
	list_for_each_entry_safe(cur, next, &head->client, client) {
		larbid = MTK_M4U_TO_LARB(cur->mtk_m4u_id);
		portid = MTK_M4U_TO_PORT(cur->mtk_m4u_id);
		larb_mmu = &data->smi_imu.larb_imu[larbid];

		dev_dbg(dev, "%s iommu port: %d\n",
			enable ? "enable" : "disable", portid);

		if (enable)
			larb_mmu->mmu |= MTK_SMI_MMU_EN(portid);
		else
			larb_mmu->mmu &= ~MTK_SMI_MMU_EN(portid);
	}
}

static int mtk_iommu_domain_finalise(struct mtk_iommu_data *data)
{
	struct mtk_iommu_domain *dom = data->m4u_dom;

	spin_lock_init(&dom->pgtlock);

	dom->cfg = (struct io_pgtable_cfg) {
		.quirks = IO_PGTABLE_QUIRK_ARM_NS |
			IO_PGTABLE_QUIRK_NO_PERMS |
			IO_PGTABLE_QUIRK_TLBI_ON_MAP,
		.pgsize_bitmap = mtk_iommu_ops.pgsize_bitmap,
		.ias = 32,
		.oas = 32,
		.tlb = &mtk_iommu_gather_ops,
		.iommu_dev = data->dev,
	};

	if (data->enable_4GB)
		dom->cfg.quirks |= IO_PGTABLE_QUIRK_ARM_MTK_4GB;

	dom->iop = alloc_io_pgtable_ops(ARM_V7S, &dom->cfg, data);
	if (!dom->iop) {
		dev_err(data->dev, "Failed to alloc io pgtable\n");
		return -EINVAL;
	}

	/* Update our support page sizes bitmap */
	mtk_iommu_ops.pgsize_bitmap = dom->cfg.pgsize_bitmap;

	/* mtk iommu(mt2712/mt8695) use bit[1:0] as address bit[33:32] */
	data->m4u_dom->cfg.arm_v7s_cfg.ttbr[0] &= ~(BIT(0) | BIT(1));

	mtk_iommu_pgt_base = data->m4u_dom->cfg.arm_v7s_cfg.ttbr[0];

	writel(data->m4u_dom->cfg.arm_v7s_cfg.ttbr[0],
	       data->base + REG_MMU_PT_BASE_ADDR);
	return 0;
}

#ifdef CONFIG_ARM64
static struct iommu_domain *mtk_iommu_domain_alloc(unsigned type)
{
	struct mtk_iommu_domain *dom;

	if (type != IOMMU_DOMAIN_DMA)
		return NULL;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	if (iommu_get_dma_cookie(&dom->domain)) {
		kfree(dom);
		return NULL;
	}

	dom->domain.geometry.aperture_start = 0;
	dom->domain.geometry.aperture_end = DMA_BIT_MASK(32);
	dom->domain.geometry.force_aperture = true;

	return &dom->domain;
}
#else
static struct iommu_domain *mtk_iommu_domain_alloc(unsigned type)
{
	struct mtk_iommu_domain *dom;

	if (type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	return &dom->domain;
}
#endif

static void mtk_iommu_domain_free(struct iommu_domain *domain)
{
#ifdef CONFIG_ARM64
	iommu_put_dma_cookie(domain);
#endif
	kfree(to_mtk_domain(domain));
}

static int mtk_iommu_attach_device(struct iommu_domain *domain,
				   struct device *dev)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	struct mtk_iommu_client_priv *priv = dev->archdata.iommu;
	struct mtk_iommu_data *data;
	int ret;

	if (!priv)
		return -ENODEV;

	data = dev_get_drvdata(priv->m4udev);
	if (!data->m4u_dom) {
		data->m4u_dom = dom;
		ret = mtk_iommu_domain_finalise(data);
		if (ret) {
			data->m4u_dom = NULL;
			return ret;
		}
	} else if (data->m4u_dom != dom) {
		/* All the client devices should be in the same m4u domain */
		dev_err(dev, "try to attach into the error iommu domain\n");
		return -EPERM;
	}

	mtk_iommu_config(data, dev, true);
	return 0;
}

static void mtk_iommu_detach_device(struct iommu_domain *domain,
				    struct device *dev)
{
	struct mtk_iommu_client_priv *priv = dev->archdata.iommu;
	struct mtk_iommu_data *data;

	if (!priv)
		return;

	data = dev_get_drvdata(priv->m4udev);
	mtk_iommu_config(data, dev, false);
}

static int mtk_iommu_map(struct iommu_domain *domain, unsigned long iova,
			 phys_addr_t paddr, size_t size, int prot)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dom->pgtlock, flags);
	ret = dom->iop->map(dom->iop, iova, paddr, size, prot);
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	return ret;
}

static size_t mtk_iommu_unmap(struct iommu_domain *domain,
			      unsigned long iova, size_t size)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	size_t unmapsz;

	spin_lock_irqsave(&dom->pgtlock, flags);
	unmapsz = dom->iop->unmap(dom->iop, iova, size);
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	return unmapsz;
}

static phys_addr_t mtk_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t iova)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	phys_addr_t pa;

	spin_lock_irqsave(&dom->pgtlock, flags);
	pa = dom->iop->iova_to_phys(dom->iop, iova);
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	return pa;
}

#ifdef CONFIG_ARM64
static int mtk_iommu_add_device(struct device *dev)
{
	struct iommu_group *group;

	if (!dev->archdata.iommu) /* Not a iommu client device */
		return -ENODEV;

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR(group))
		return PTR_ERR(group);

	iommu_group_put(group);
	return 0;
}
#else
/*
 * MTK generation one iommu HW only support one iommu domain, and all the client
 * sharing the same iova address space.
 */
static int mtk_iommu_create_mapping(struct device *dev,
				    struct of_phandle_args *args)
{
	struct mtk_iommu_client_priv *head, *priv, *next;
	struct platform_device *m4updev;
	struct dma_iommu_mapping *mtk_mapping;
	struct device *m4udev;
	int ret;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	if (!dev->archdata.iommu) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		head = kzalloc(sizeof(*head), GFP_KERNEL);
		if (!head)
			return -ENOMEM;

		dev->archdata.iommu = head;
		INIT_LIST_HEAD(&head->client);
		head->m4udev = &m4updev->dev;
	} else {
		head = dev->archdata.iommu;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_free_mem;
	}
	priv->mtk_m4u_id = args->args[0];
	list_add_tail(&priv->client, &head->client);

	m4udev = head->m4udev;
	mtk_mapping = m4udev->archdata.iommu;
	if (!mtk_mapping) {
		/* MTK iommu support 4GB iova address space. */
		mtk_mapping = arm_iommu_create_mapping(&platform_bus_type,
						0, 1ULL << 32);
		if (IS_ERR(mtk_mapping)) {
			pr_err("arm_iommu_create_mapping error\n");
			ret = PTR_ERR(mtk_mapping);
			goto err_free_mem;
		}
		m4udev->archdata.iommu = mtk_mapping;
	}

	ret = arm_iommu_attach_device(dev, mtk_mapping);
	if (ret)
		goto err_release_mapping;

	return 0;

err_release_mapping:
	arm_iommu_release_mapping(mtk_mapping);
	m4udev->archdata.iommu = NULL;
err_free_mem:
	list_for_each_entry_safe(priv, next, &head->client, client)
		kfree(priv);
	kfree(head);
	dev->archdata.iommu = NULL;
	return ret;
}

static int mtk_iommu_add_device(struct device *dev)
{
	struct iommu_group *group;
	struct of_phandle_args iommu_spec;
	int idx = 0;

	while (!of_parse_phandle_with_args(dev->of_node, "iommus",
					   "#iommu-cells", idx,
					   &iommu_spec)) {
		mtk_iommu_create_mapping(dev, &iommu_spec);
		of_node_put(iommu_spec.np);
		idx++;
	}

	/*
	 * if one of the iommu client get in here before iommu master,
	 * then iommu master has set it's dev->archdata.iommu as dma_iommu_mapping
	 * in mtk_iommu_create_mapping, then it will goto the mtk_iommu_device_group,
	 * but the dev->archdata.iommu is different for iommu client and iommu master,
	 * system may crash there since we always parse it as iommu client's priv data.
	 *
	 * We using the of_parse_phandle_with_args to identify whether it's a iommu
	 * client or iommu master, since only iommu client have the iommus property
	 * in it's device node.
	 */
	if (!idx) {
		dev_dbg(dev, "%s, %d, not a iommu client\n", __func__, __LINE__);
		return -ENODEV;
	}

	if (!dev->archdata.iommu) /* Not a iommu client device */
		return -ENODEV;

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR(group))
		return PTR_ERR(group);

	iommu_group_put(group);
	return 0;
}
#endif

static void mtk_iommu_remove_device(struct device *dev)
{
	struct mtk_iommu_client_priv *head, *cur, *next;

	head = dev->archdata.iommu;
	if (!head)
		return;

	list_for_each_entry_safe(cur, next, &head->client, client) {
		list_del(&cur->client);
		kfree(cur);
	}
	kfree(head);
	dev->archdata.iommu = NULL;

	iommu_group_remove_device(dev);
}

static struct iommu_group *mtk_iommu_device_group(struct device *dev)
{
	struct mtk_iommu_data *data;
	struct mtk_iommu_client_priv *priv;

	priv = dev->archdata.iommu;
	if (!priv)
		return ERR_PTR(-ENODEV);

	/* All the client devices are in the same m4u iommu-group */
	data = dev_get_drvdata(priv->m4udev);
	if (!data->m4u_group) {
		data->m4u_group = iommu_group_alloc();
		if (IS_ERR(data->m4u_group))
			dev_err(dev, "Failed to allocate M4U IOMMU group\n");
	}
	return data->m4u_group;
}

#ifdef CONFIG_ARM64
static int mtk_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct mtk_iommu_client_priv *head, *priv, *next;
	struct platform_device *m4updev;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	if (!dev->archdata.iommu) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		of_node_put(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		head = kzalloc(sizeof(*head), GFP_KERNEL);
		if (!head)
			return -ENOMEM;

		dev->archdata.iommu = head;
		INIT_LIST_HEAD(&head->client);
		head->m4udev = &m4updev->dev;
	} else {
		head = dev->archdata.iommu;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		goto err_free_mem;

	priv->mtk_m4u_id = args->args[0];
	list_add_tail(&priv->client, &head->client);

	return 0;

err_free_mem:
	list_for_each_entry_safe(priv, next, &head->client, client)
		kfree(priv);
	kfree(head);
	dev->archdata.iommu = NULL;
	return -ENOMEM;
}
#endif

/* this function would only be called once, we do not need to handle re-entry issue. */
static void mtk_iommu_get_dm_regions(struct device *dev, struct list_head *list)
{
	struct iommu_dm_region *region;

	/* for framebuffer region */
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return;

	INIT_LIST_HEAD(&region->list);
#ifdef CONFIG_MTK_FB
	region->start = mtkfb_get_fb_base();
	region->length = mtkfb_get_fb_size();
#endif
	region->prot = IOMMU_READ | IOMMU_WRITE;
	list_add_tail(&region->list, list);
}

static void mtk_iommu_put_dm_regions(struct device *dev, struct list_head *list)
{
	struct  iommu_dm_region *region, *tmp;

	list_for_each_entry_safe(region, tmp, list, list)
	kfree(region);
}

static struct iommu_ops mtk_iommu_ops = {
	.domain_alloc	= mtk_iommu_domain_alloc,
	.domain_free	= mtk_iommu_domain_free,
	.attach_dev	= mtk_iommu_attach_device,
	.detach_dev	= mtk_iommu_detach_device,
	.map		= mtk_iommu_map,
	.unmap		= mtk_iommu_unmap,
	.map_sg		= default_iommu_map_sg,
	.iova_to_phys	= mtk_iommu_iova_to_phys,
	.add_device	= mtk_iommu_add_device,
	.remove_device	= mtk_iommu_remove_device,
	.device_group	= mtk_iommu_device_group,
#ifdef CONFIG_ARM64
	.of_xlate	= mtk_iommu_of_xlate,
#endif
	.pgsize_bitmap	= SZ_4K | SZ_64K | SZ_1M | SZ_16M,
	.get_dm_regions = mtk_iommu_get_dm_regions,
	.put_dm_regions = mtk_iommu_put_dm_regions,
};

static void mtk_iommu_interrupt_disable(const struct mtk_iommu_data *data)
{
	writel_relaxed(0, data->base + REG_MMU_INT_MAIN_CONTROL);
}

static int mtk_iommu_hw_init(const struct mtk_iommu_data *data)
{
	u32 regval;
	int ret;

	ret = clk_prepare_enable(data->bclk);
	if (ret) {
		dev_err(data->dev, "Failed to enable iommu bclk(%d)\n", ret);
		return ret;
	}

	if (data->match_type == m4u_mt8173) {
		regval = F_MMU_PREFETCH_RT_REPLACE_MOD |
			F_MMU_TF_PROTECT_SEL(2);
	} else {
		/* mt8695 use the same hw with mt2712 */
		regval = F_MMU_TF_PROT_SEL(2);
	}
	writel_relaxed(regval, data->base + REG_MMU_CTRL_REG);

	regval = F_L2_MULIT_HIT_EN |
		F_TABLE_WALK_FAULT_INT_EN |
		F_PREETCH_FIFO_OVERFLOW_INT_EN |
		F_MISS_FIFO_OVERFLOW_INT_EN |
		F_PREFETCH_FIFO_ERR_INT_EN |
		F_MISS_FIFO_ERR_INT_EN;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL0);

	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_MISS_TRANSACTION_FIFO_FAULT |
		F_INT_PRETETCH_TRANSATION_FIFO_FAULT;
	writel_relaxed(regval, data->base + REG_MMU_INT_MAIN_CONTROL);

	writel_relaxed(F_MMU_IVRP_PA_SET(data->protect_base, data->enable_4GB),
		       data->base + REG_MMU_IVRP_PADDR);

	writel_relaxed(0, data->base + REG_MMU_DCM_DIS);

	/* Default Enable write in-order. */
	writel_relaxed(0, data->base + REG_MMU_STANDARD_AXI_MODE);

	regval = readl_relaxed(data->base + REG_MMU_WR_LEN_CTRL);
	regval &= ~(F_MMU_MMU0_WR_THROT_DIS | F_MMU_MMU1_WR_THROT_DIS);
	writel_relaxed(regval, data->base + REG_MMU_WR_LEN_CTRL);

	if (devm_request_irq(data->dev, data->irq, mtk_iommu_isr, 0,
			     dev_name(data->dev), (void *)data)) {
		writel_relaxed(0, data->base + REG_MMU_PT_BASE_ADDR);
		clk_disable_unprepare(data->bclk);
		dev_err(data->dev, "Failed @ IRQ-%d Request\n", data->irq);
		return -ENODEV;
	}

	return 0;
}

static const struct component_master_ops mtk_iommu_com_ops = {
	.bind		= mtk_iommu_bind,
	.unbind		= mtk_iommu_unbind,
};

static struct mtk_iommu_data *gmtk_iommu;

static int mtk_iommu_probe(struct platform_device *pdev)
{
	const struct of_device_id        *of_id;
	struct mtk_iommu_data   *data;
	struct device           *dev = &pdev->dev;
	struct resource         *res;
	struct component_match  *match = NULL;
	void                    *protect;
	int                     i, larb_nr, ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;

	of_id = of_match_node(mtk_iommu_of_ids, dev->of_node);
	data->match_type = (enum mtk_iommu_match_type)of_id->data;

	/* Protect memory. HW will access here while translation fault.*/
	protect = devm_kzalloc(dev, MTK_PROTECT_PA_ALIGN * 2, GFP_KERNEL);
	if (!protect)
		return -ENOMEM;
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	/* Whether the current dram is over 4GB */
	data->enable_4GB = !!(max_pfn > (BIT_ULL(32) >> PAGE_SHIFT));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	gmtk_iommu = data;

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0)
		return data->irq;

	data->bclk = devm_clk_get(dev, "bclk");
	if (IS_ERR(data->bclk))
		return PTR_ERR(data->bclk);

	larb_nr = of_count_phandle_with_args(dev->of_node,
					     "mediatek,larbs", NULL);
	if (larb_nr < 0)
		return larb_nr;
	data->smi_imu.larb_nr = larb_nr;

	for (i = 0; i < larb_nr; i++) {
		struct device_node *larbnode;
		struct platform_device *plarbdev;
		unsigned int idx;

		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
		if (!larbnode)
			return -EINVAL;

		if (!of_device_is_available(larbnode))
			continue;

		ret = of_property_read_u32(larbnode, "mediatek,larbidx", &idx);
		/* The index is consecutive if there is no this property */
		if (ret)
			idx = i;

		plarbdev = of_find_device_by_node(larbnode);
		of_node_put(larbnode);
		if (!plarbdev) {
			plarbdev = of_platform_device_create(
						larbnode, NULL,
						platform_bus_type.dev_root);
			if (!plarbdev)
				return -EPROBE_DEFER;
		}
		data->smi_imu.larb_imu[idx].dev = &plarbdev->dev;

		component_match_add(dev, &match, compare_of, larbnode);
	}

	platform_set_drvdata(pdev, data);

	ret = mtk_iommu_hw_init(data);
	if (ret)
		return ret;

	if (!iommu_present(&platform_bus_type))
		bus_set_iommu(&platform_bus_type, &mtk_iommu_ops);

	return component_master_add_with_match(dev, &mtk_iommu_com_ops, match);
}

static int mtk_iommu_remove(struct platform_device *pdev)
{
	struct mtk_iommu_data *data = platform_get_drvdata(pdev);

	if (iommu_present(&platform_bus_type))
		bus_set_iommu(&platform_bus_type, NULL);

	mtk_iommu_interrupt_disable(data);
	free_io_pgtable_ops(data->m4u_dom->iop);
	clk_disable_unprepare(data->bclk);
	devm_free_irq(&pdev->dev, data->irq, data);
	component_master_del(&pdev->dev, &mtk_iommu_com_ops);
	return 0;
}

static void mtk_iommu_shutdown(struct platform_device *pdev)
{
	mtk_iommu_remove(pdev);
}

static int __maybe_unused mtk_iommu_suspend(struct device *dev)
{
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	void __iomem *base = data->base;

	reg->standard_axi_mode = readl_relaxed(base +
					       REG_MMU_STANDARD_AXI_MODE);
	reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM_DIS);
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);
	reg->int_control0 = readl_relaxed(base + REG_MMU_INT_CONTROL0);
	reg->int_main_control = readl_relaxed(base + REG_MMU_INT_MAIN_CONTROL);
	reg->wr_len_ctl = readl_relaxed(base + REG_MMU_WR_LEN_CTRL);
	mtk_iommu_interrupt_disable(data);
	return 0;
}

static int __maybe_unused mtk_iommu_resume(struct device *dev)
{
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	void __iomem *base = data->base;

	data->m4u_dom->cfg.arm_v7s_cfg.ttbr[0] &= ~(BIT(0) | BIT(1));
	writel_relaxed(data->m4u_dom->cfg.arm_v7s_cfg.ttbr[0],
		       base + REG_MMU_PT_BASE_ADDR);
	writel_relaxed(reg->standard_axi_mode,
		       base + REG_MMU_STANDARD_AXI_MODE);
	writel_relaxed(reg->wr_len_ctl, base + REG_MMU_WR_LEN_CTRL);
	writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM_DIS);
	writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);
	writel_relaxed(reg->int_control0, base + REG_MMU_INT_CONTROL0);
	writel_relaxed(reg->int_main_control, base + REG_MMU_INT_MAIN_CONTROL);
	writel_relaxed(F_MMU_IVRP_PA_SET(data->protect_base, data->enable_4GB),
		       base + REG_MMU_IVRP_PADDR);
	return 0;
}

const struct dev_pm_ops mtk_iommu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_iommu_suspend, mtk_iommu_resume)
};

static const struct of_device_id mtk_iommu_of_ids[] = {
	{ .compatible = "mediatek,mt8173-m4u", .data = (void *)m4u_mt8173},
	{ .compatible = "mediatek,mt2712-m4u", .data = (void *)m4u_mt2712},
	{ .compatible = "mediatek,mt8695-m4u", .data = (void *)m4u_mt8695},
	{}
};

static struct platform_driver mtk_iommu_driver = {
	.probe	= mtk_iommu_probe,
	.remove	= mtk_iommu_remove,
	.shutdown = mtk_iommu_shutdown,
	.driver	= {
		.name = "mtk-iommu",
		.of_match_table = of_match_ptr(mtk_iommu_of_ids),
		.pm = &mtk_iommu_pm_ops,
	}
};

#ifdef CONFIG_ARM64
static int mtk_iommu_init_fn(struct device_node *np)
{
	static bool init_done;
	int ret;
	struct platform_device *pdev;

	if (!init_done) {
		pdev = of_platform_device_create(np, NULL,
						 platform_bus_type.dev_root);
		if (!pdev)
			return -ENOMEM;

		ret = platform_driver_register(&mtk_iommu_driver);
		if (ret) {
			pr_err("%s: Failed to register driver\n", __func__);
			return ret;
		}
		init_done = true;
	}

	of_iommu_set_ops(np, &mtk_iommu_ops);
	return 0;
}
#else
static int mtk_iommu_init_fn(struct device_node *np)
{
	int ret;

	ret = platform_driver_register(&mtk_iommu_driver);
	if (ret) {
		pr_err("%s: Failed to register driver\n", __func__);
		return ret;
	}

	return 0;
}
#endif

IOMMU_OF_DECLARE(mt8173m4u, "mediatek,mt8173-m4u", mtk_iommu_init_fn);
IOMMU_OF_DECLARE(mt2712m4u, "mediatek,mt2712-m4u", mtk_iommu_init_fn);
IOMMU_OF_DECLARE(mt8695m4u, "mediatek,mt8695-m4u", mtk_iommu_init_fn);
