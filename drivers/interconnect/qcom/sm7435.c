// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal SM7435 (parrot) interconnect provider: only the GPU -> LLCC -> DDR
 * path (gem_noc + mc_virt). Values taken from downstream parrot.c.
 */

#include <linux/device.h>
#include <linux/interconnect.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <dt-bindings/interconnect/qcom,sm7435-rpmh.h>

#include "bcm-voter.h"
#include "icc-common.h"
#include "icc-rpmh.h"

static struct qcom_icc_node ebi = {
	.name = "ebi",
	.channels = 2,
	.buswidth = 4,
};

static struct qcom_icc_node llcc_mc = {
	.name = "llcc_mc",
	.channels = 2,
	.buswidth = 4,
	.num_links = 1,
	.link_nodes = { &ebi },
};

static struct qcom_icc_node qns_llcc = {
	.name = "qns_llcc",
	.channels = 2,
	.buswidth = 16,
	.num_links = 1,
	.link_nodes = { &llcc_mc },
};

/* link to the cnoc master omitted: that provider is not ported */
static struct qcom_icc_node qns_gem_noc_cnoc = {
	.name = "qns_gem_noc_cnoc",
	.channels = 1,
	.buswidth = 16,
};

static struct qcom_icc_node qnm_gpu = {
	.name = "qnm_gpu",
	.channels = 2,
	.buswidth = 32,
	.num_links = 2,
	.link_nodes = { &qns_gem_noc_cnoc, &qns_llcc },
};

static struct qcom_icc_bcm bcm_acv = {
	.name = "ACV",
	.enable_mask = 0x8,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_mc0 = {
	.name = "MC0",
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &ebi },
};

static struct qcom_icc_bcm bcm_sh0 = {
	.name = "SH0",
	.keepalive = true,
	.num_nodes = 1,
	.nodes = { &qns_llcc },
};

static struct qcom_icc_bcm * const gem_noc_bcms[] = {
	&bcm_sh0,
};

static struct qcom_icc_node * const gem_noc_nodes[] = {
	[MASTER_GFX3D] = &qnm_gpu,
	[SLAVE_GEM_NOC_CNOC] = &qns_gem_noc_cnoc,
	[SLAVE_LLCC] = &qns_llcc,
};

static const struct qcom_icc_desc sm7435_gem_noc = {
	.nodes = gem_noc_nodes,
	.num_nodes = ARRAY_SIZE(gem_noc_nodes),
	.bcms = gem_noc_bcms,
	.num_bcms = ARRAY_SIZE(gem_noc_bcms),
};

static struct qcom_icc_bcm * const mc_virt_bcms[] = {
	&bcm_acv,
	&bcm_mc0,
};

static struct qcom_icc_node * const mc_virt_nodes[] = {
	[MASTER_LLCC] = &llcc_mc,
	[SLAVE_EBI1] = &ebi,
};

static const struct qcom_icc_desc sm7435_mc_virt = {
	.nodes = mc_virt_nodes,
	.num_nodes = ARRAY_SIZE(mc_virt_nodes),
	.bcms = mc_virt_bcms,
	.num_bcms = ARRAY_SIZE(mc_virt_bcms),
};

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,sm7435-gem-noc", .data = &sm7435_gem_noc },
	{ .compatible = "qcom,sm7435-mc-virt", .data = &sm7435_mc_virt },
	{ }
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qcom_icc_rpmh_probe,
	.remove = qcom_icc_rpmh_remove,
	.driver = {
		.name = "qnoc-sm7435",
		.of_match_table = qnoc_of_match,
	},
};

static int __init qnoc_driver_init(void)
{
	return platform_driver_register(&qnoc_driver);
}
core_initcall(qnoc_driver_init);

static void __exit qnoc_driver_exit(void)
{
	platform_driver_unregister(&qnoc_driver);
}
module_exit(qnoc_driver_exit);

MODULE_DESCRIPTION("SM7435 minimal NoC driver");
MODULE_LICENSE("GPL");
