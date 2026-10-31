// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Ximi <ximiyadka124@proton.me>
 *
 * Qualcomm SM7435 (Snapdragon 7s Gen 2) Global Clock Controller.
 *
 * This driver currently implements the subset of the GCC that is required by
 * the UFS host controller and the QMP UFS PHY:
 *
 *   - GCC_GPLL0 / GCC_GPLL0_OUT_EVEN (parents of the UFS PHY clocks)
 *   - the UFS PHY AXI/AHB/UNIPRO/ICE/AUX clock roots and their branches
 *   - the UFS PHY symbol clock multiplexers
 *   - GCC_UFS_0_CLKREF_EN
 *   - the UFS PHY global domain power controller (UFS_PHY_GDSC)
 *   - the UFS PHY block reset (GCC_UFS_PHY_BCR)
 *
 * All register offsets, frequency tables and parent mappings are taken from
 * the downstream "qcom,parrot-gcc" clock driver
 * (drivers/clk/qcom/gcc-parrot.c) of the LineageOS SM7435 kernel.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,gcc-sm7435.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-mux.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

enum {
	P_BI_TCXO,
	P_GCC_GPLL0_OUT_EVEN,
	P_GCC_GPLL0_OUT_MAIN,
	P_UFS_PHY_RX_SYMBOL_0_CLK,
	P_UFS_PHY_RX_SYMBOL_1_CLK,
	P_UFS_PHY_TX_SYMBOL_0_CLK,
};

static struct clk_alpha_pll gcc_gpll0 = {
	.offset = 0x0,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr = {
		.enable_reg = 0x62018,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_gpll0",
			.parent_data = &(const struct clk_parent_data){
				.fw_name = "bi_tcxo",
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_fixed_lucid_evo_ops,
		},
	},
};

static const struct clk_div_table post_div_table_gcc_gpll0_out_even[] = {
	{ 0x1, 2 },
	{ }
};

static struct clk_alpha_pll_postdiv gcc_gpll0_out_even = {
	.offset = 0x0,
	.post_div_shift = 10,
	.post_div_table = post_div_table_gcc_gpll0_out_even,
	.num_post_div = ARRAY_SIZE(post_div_table_gcc_gpll0_out_even),
	.width = 4,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gcc_gpll0_out_even",
		.parent_hws = (const struct clk_hw*[]){
			&gcc_gpll0.clkr.hw,
		},
		.num_parents = 1,
		.ops = &clk_alpha_pll_postdiv_lucid_evo_ops,
	},
};

static const struct parent_map gcc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
	{ P_GCC_GPLL0_OUT_MAIN, 1 },
	{ P_GCC_GPLL0_OUT_EVEN, 6 },
};

static const struct clk_parent_data gcc_parent_data_0[] = {
	{ .fw_name = "bi_tcxo" },
	{ .hw = &gcc_gpll0.clkr.hw },
	{ .hw = &gcc_gpll0_out_even.clkr.hw },
};

static const struct parent_map gcc_parent_map_1[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data gcc_parent_data_1[] = {
	{ .fw_name = "bi_tcxo" },
};

static const struct parent_map gcc_parent_map_2[] = {
	{ P_UFS_PHY_RX_SYMBOL_0_CLK, 0 },
	{ P_BI_TCXO, 2 },
};

static const struct clk_parent_data gcc_parent_data_2[] = {
	{ .fw_name = "ufs_phy_rx_symbol_0_clk" },
	{ .fw_name = "bi_tcxo" },
};

static const struct parent_map gcc_parent_map_3[] = {
	{ P_UFS_PHY_RX_SYMBOL_1_CLK, 0 },
	{ P_BI_TCXO, 2 },
};

static const struct clk_parent_data gcc_parent_data_3[] = {
	{ .fw_name = "ufs_phy_rx_symbol_1_clk" },
	{ .fw_name = "bi_tcxo" },
};

static const struct parent_map gcc_parent_map_4[] = {
	{ P_UFS_PHY_TX_SYMBOL_0_CLK, 0 },
	{ P_BI_TCXO, 2 },
};

static const struct clk_parent_data gcc_parent_data_4[] = {
	{ .fw_name = "ufs_phy_tx_symbol_0_clk" },
	{ .fw_name = "bi_tcxo" },
};

static struct clk_regmap_mux gcc_ufs_phy_rx_symbol_0_clk_src = {
	.reg = 0x87060,
	.shift = 0,
	.width = 2,
	.parent_map = gcc_parent_map_2,
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_rx_symbol_0_clk_src",
			.parent_data = gcc_parent_data_2,
			.num_parents = ARRAY_SIZE(gcc_parent_data_2),
			.ops = &clk_regmap_mux_closest_ops,
		},
	},
};

static struct clk_regmap_mux gcc_ufs_phy_rx_symbol_1_clk_src = {
	.reg = 0x870d0,
	.shift = 0,
	.width = 2,
	.parent_map = gcc_parent_map_3,
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_rx_symbol_1_clk_src",
			.parent_data = gcc_parent_data_3,
			.num_parents = ARRAY_SIZE(gcc_parent_data_3),
			.ops = &clk_regmap_mux_closest_ops,
		},
	},
};

static struct clk_regmap_mux gcc_ufs_phy_tx_symbol_0_clk_src = {
	.reg = 0x87050,
	.shift = 0,
	.width = 2,
	.parent_map = gcc_parent_map_4,
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_tx_symbol_0_clk_src",
			.parent_data = gcc_parent_data_4,
			.num_parents = ARRAY_SIZE(gcc_parent_data_4),
			.ops = &clk_regmap_mux_closest_ops,
		},
	},
};

static const struct freq_tbl ftbl_gcc_ufs_phy_axi_clk_src[] = {
	F(25000000, P_GCC_GPLL0_OUT_EVEN, 12, 0, 0),
	F(75000000, P_GCC_GPLL0_OUT_EVEN, 4, 0, 0),
	F(150000000, P_GCC_GPLL0_OUT_MAIN, 4, 0, 0),
	F(300000000, P_GCC_GPLL0_OUT_MAIN, 2, 0, 0),
	{ }
};

static struct clk_rcg2 gcc_ufs_phy_axi_clk_src = {
	.cmd_rcgr = 0x8702c,
	.mnd_width = 8,
	.hid_width = 5,
	.parent_map = gcc_parent_map_0,
	.freq_tbl = ftbl_gcc_ufs_phy_axi_clk_src,
	.hw_clk_ctrl = true,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gcc_ufs_phy_axi_clk_src",
		.parent_data = gcc_parent_data_0,
		.num_parents = ARRAY_SIZE(gcc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_gcc_ufs_phy_ice_core_clk_src[] = {
	F(75000000, P_GCC_GPLL0_OUT_EVEN, 4, 0, 0),
	F(150000000, P_GCC_GPLL0_OUT_MAIN, 4, 0, 0),
	F(300000000, P_GCC_GPLL0_OUT_MAIN, 2, 0, 0),
	{ }
};

static struct clk_rcg2 gcc_ufs_phy_ice_core_clk_src = {
	.cmd_rcgr = 0x87074,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gcc_parent_map_0,
	.freq_tbl = ftbl_gcc_ufs_phy_ice_core_clk_src,
	.hw_clk_ctrl = true,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gcc_ufs_phy_ice_core_clk_src",
		.parent_data = gcc_parent_data_0,
		.num_parents = ARRAY_SIZE(gcc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_rcg2 gcc_ufs_phy_unipro_core_clk_src = {
	.cmd_rcgr = 0x8708c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gcc_parent_map_0,
	.freq_tbl = ftbl_gcc_ufs_phy_ice_core_clk_src,
	.hw_clk_ctrl = true,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gcc_ufs_phy_unipro_core_clk_src",
		.parent_data = gcc_parent_data_0,
		.num_parents = ARRAY_SIZE(gcc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_gcc_ufs_phy_phy_aux_clk_src[] = {
	F(9600000, P_BI_TCXO, 2, 0, 0),
	F(19200000, P_BI_TCXO, 1, 0, 0),
	{ }
};

static struct clk_rcg2 gcc_ufs_phy_phy_aux_clk_src = {
	.cmd_rcgr = 0x870a8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gcc_parent_map_1,
	.freq_tbl = ftbl_gcc_ufs_phy_phy_aux_clk_src,
	.hw_clk_ctrl = true,
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gcc_ufs_phy_phy_aux_clk_src",
		.parent_data = gcc_parent_data_1,
		.num_parents = ARRAY_SIZE(gcc_parent_data_1),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_branch gcc_aggre_ufs_phy_axi_clk = {
	.halt_reg = 0x870d4,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x870d4,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x870d4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_aggre_ufs_phy_axi_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_axi_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_0_clkref_en = {
	.halt_reg = 0x9c000,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x9c000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_0_clkref_en",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_ahb_clk = {
	.halt_reg = 0x87020,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x87020,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x87020,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_ahb_clk",
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_axi_clk = {
	.halt_reg = 0x87018,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x87018,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x87018,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_axi_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_axi_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_ice_core_clk = {
	.halt_reg = 0x8706c,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x8706c,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x8706c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_ice_core_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_ice_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_phy_aux_clk = {
	.halt_reg = 0x870a4,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x870a4,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x870a4,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_phy_aux_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_phy_aux_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_rx_symbol_0_clk = {
	.halt_reg = 0x87028,
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x87028,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_rx_symbol_0_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_rx_symbol_0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_rx_symbol_1_clk = {
	.halt_reg = 0x870c0,
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x870c0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_rx_symbol_1_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_rx_symbol_1_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_tx_symbol_0_clk = {
	.halt_reg = 0x87024,
	.halt_check = BRANCH_HALT_DELAY,
	.clkr = {
		.enable_reg = 0x87024,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_tx_symbol_0_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_tx_symbol_0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch gcc_ufs_phy_unipro_core_clk = {
	.halt_reg = 0x87064,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x87064,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x87064,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gcc_ufs_phy_unipro_core_clk",
			.parent_hws = (const struct clk_hw*[]){
				&gcc_ufs_phy_unipro_core_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct gdsc ufs_phy_gdsc = {
	.gdscr = 0x87004,
	.pd = {
		.name = "ufs_phy_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
};

static struct clk_regmap *gcc_sm7435_clocks[] = {
	[GCC_GPLL0] = &gcc_gpll0.clkr,
	[GCC_GPLL0_OUT_EVEN] = &gcc_gpll0_out_even.clkr,
	[GCC_AGGRE_UFS_PHY_AXI_CLK] = &gcc_aggre_ufs_phy_axi_clk.clkr,
	[GCC_UFS_0_CLKREF_EN] = &gcc_ufs_0_clkref_en.clkr,
	[GCC_UFS_PHY_AHB_CLK] = &gcc_ufs_phy_ahb_clk.clkr,
	[GCC_UFS_PHY_AXI_CLK] = &gcc_ufs_phy_axi_clk.clkr,
	[GCC_UFS_PHY_AXI_CLK_SRC] = &gcc_ufs_phy_axi_clk_src.clkr,
	[GCC_UFS_PHY_ICE_CORE_CLK] = &gcc_ufs_phy_ice_core_clk.clkr,
	[GCC_UFS_PHY_ICE_CORE_CLK_SRC] = &gcc_ufs_phy_ice_core_clk_src.clkr,
	[GCC_UFS_PHY_PHY_AUX_CLK] = &gcc_ufs_phy_phy_aux_clk.clkr,
	[GCC_UFS_PHY_PHY_AUX_CLK_SRC] = &gcc_ufs_phy_phy_aux_clk_src.clkr,
	[GCC_UFS_PHY_RX_SYMBOL_0_CLK] = &gcc_ufs_phy_rx_symbol_0_clk.clkr,
	[GCC_UFS_PHY_RX_SYMBOL_0_CLK_SRC] = &gcc_ufs_phy_rx_symbol_0_clk_src.clkr,
	[GCC_UFS_PHY_RX_SYMBOL_1_CLK] = &gcc_ufs_phy_rx_symbol_1_clk.clkr,
	[GCC_UFS_PHY_RX_SYMBOL_1_CLK_SRC] = &gcc_ufs_phy_rx_symbol_1_clk_src.clkr,
	[GCC_UFS_PHY_TX_SYMBOL_0_CLK] = &gcc_ufs_phy_tx_symbol_0_clk.clkr,
	[GCC_UFS_PHY_TX_SYMBOL_0_CLK_SRC] = &gcc_ufs_phy_tx_symbol_0_clk_src.clkr,
	[GCC_UFS_PHY_UNIPRO_CORE_CLK] = &gcc_ufs_phy_unipro_core_clk.clkr,
	[GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC] = &gcc_ufs_phy_unipro_core_clk_src.clkr,
};

static const struct qcom_reset_map gcc_sm7435_resets[] = {
	[GCC_UFS_PHY_BCR] = { 0x87000 },
};

static struct gdsc *gcc_sm7435_gdscs[] = {
	[UFS_PHY_GDSC] = &ufs_phy_gdsc,
};

static const struct regmap_config gcc_sm7435_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x9c010,
	.fast_io = true,
};

static const struct qcom_cc_desc gcc_sm7435_desc = {
	.config = &gcc_sm7435_regmap_config,
	.clks = gcc_sm7435_clocks,
	.num_clks = ARRAY_SIZE(gcc_sm7435_clocks),
	.resets = gcc_sm7435_resets,
	.num_resets = ARRAY_SIZE(gcc_sm7435_resets),
	.gdscs = gcc_sm7435_gdscs,
	.num_gdscs = ARRAY_SIZE(gcc_sm7435_gdscs),
};

static const struct of_device_id gcc_sm7435_match_table[] = {
	{ .compatible = "qcom,gcc-sm7435" },
	{ }
};
MODULE_DEVICE_TABLE(of, gcc_sm7435_match_table);

static int gcc_sm7435_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	int ret;

	regmap = qcom_cc_map(pdev, &gcc_sm7435_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return qcom_cc_really_probe(&pdev->dev, &gcc_sm7435_desc, regmap);
}

static struct platform_driver gcc_sm7435_driver = {
	.probe = gcc_sm7435_probe,
	.driver = {
		.name = "gcc-sm7435",
		.of_match_table = gcc_sm7435_match_table,
	},
};

static int __init gcc_sm7435_init(void)
{
	return platform_driver_register(&gcc_sm7435_driver);
}
subsys_initcall(gcc_sm7435_init);

static void __exit gcc_sm7435_exit(void)
{
	platform_driver_unregister(&gcc_sm7435_driver);
}
module_exit(gcc_sm7435_exit);

MODULE_DESCRIPTION("QTI GCC SM7435 Driver");
MODULE_LICENSE("GPL v2");
