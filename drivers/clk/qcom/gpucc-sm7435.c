// SPDX-License-Identifier: GPL-2.0-only
/*
 * SM7435 (parrot) GPU clock controller, ported from the downstream
 * gpucc-parrot.c to the mainline clk-qcom API.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,sm7435-gpucc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap-divider.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

enum {
	DT_BI_TCXO,
	DT_GPLL0_OUT_MAIN,
	DT_GPLL0_OUT_MAIN_DIV,
	DT_GCC_GPU_SNOC_DVM_GFX_CLK,
};

enum {
	P_BI_TCXO,
	P_GPLL0_OUT_MAIN,
	P_GPLL0_OUT_MAIN_DIV,
	P_GPU_CC_PLL0_OUT_MAIN,
	P_GPU_CC_PLL1_OUT_MAIN,
};

static const struct pll_vco lucid_evo_vco[] = {
	{ 249600000, 2000000000, 0 },
};

/* 590MHz */
static const struct alpha_pll_config gpu_cc_pll0_config = {
	.l = 0x1e,
	.cal_l = 0x44,
	.alpha = 0xbaaa,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x32aa299c,
	.user_ctl_val = 0x00000001,
	.user_ctl_hi_val = 0x00000805,
};

static struct clk_alpha_pll gpu_cc_pll0 = {
	.offset = 0x0,
	.vco_table = lucid_evo_vco,
	.num_vco = ARRAY_SIZE(lucid_evo_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "gpu_cc_pll0",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

/* 1000MHz */
static const struct alpha_pll_config gpu_cc_pll1_config = {
	.l = 0x34,
	.cal_l = 0x44,
	.alpha = 0x1555,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x32aa299c,
	.user_ctl_val = 0x00000001,
	.user_ctl_hi_val = 0x00000805,
};

static struct clk_alpha_pll gpu_cc_pll1 = {
	.offset = 0x1000,
	.vco_table = lucid_evo_vco,
	.num_vco = ARRAY_SIZE(lucid_evo_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "gpu_cc_pll1",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

static const struct parent_map gpu_cc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
	{ P_GPLL0_OUT_MAIN, 5 },
	{ P_GPLL0_OUT_MAIN_DIV, 6 },
};

static const struct clk_parent_data gpu_cc_parent_data_0[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_GPLL0_OUT_MAIN },
	{ .index = DT_GPLL0_OUT_MAIN_DIV },
};

static const struct parent_map gpu_cc_parent_map_1[] = {
	{ P_BI_TCXO, 0 },
	{ P_GPU_CC_PLL0_OUT_MAIN, 1 },
	{ P_GPU_CC_PLL1_OUT_MAIN, 3 },
	{ P_GPLL0_OUT_MAIN, 5 },
	{ P_GPLL0_OUT_MAIN_DIV, 6 },
};

static const struct clk_parent_data gpu_cc_parent_data_1[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &gpu_cc_pll0.clkr.hw },
	{ .hw = &gpu_cc_pll1.clkr.hw },
	{ .index = DT_GPLL0_OUT_MAIN },
	{ .index = DT_GPLL0_OUT_MAIN_DIV },
};

static const struct parent_map gpu_cc_parent_map_2[] = {
	{ P_BI_TCXO, 0 },
	{ P_GPU_CC_PLL1_OUT_MAIN, 3 },
	{ P_GPLL0_OUT_MAIN, 5 },
	{ P_GPLL0_OUT_MAIN_DIV, 6 },
};

static const struct clk_parent_data gpu_cc_parent_data_2[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &gpu_cc_pll1.clkr.hw },
	{ .index = DT_GPLL0_OUT_MAIN },
	{ .index = DT_GPLL0_OUT_MAIN_DIV },
};

static const struct parent_map gpu_cc_parent_map_3[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data gpu_cc_parent_data_3[] = {
	{ .index = DT_BI_TCXO },
};

static const struct freq_tbl ftbl_gpu_cc_ff_clk_src[] = {
	F(200000000, P_GPLL0_OUT_MAIN_DIV, 1.5, 0, 0),
	{ }
};

static struct clk_rcg2 gpu_cc_ff_clk_src = {
	.cmd_rcgr = 0x9474,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gpu_cc_parent_map_0,
	.freq_tbl = ftbl_gpu_cc_ff_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "gpu_cc_ff_clk_src",
		.parent_data = gpu_cc_parent_data_0,
		.num_parents = ARRAY_SIZE(gpu_cc_parent_data_0),
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_gpu_cc_gmu_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	F(200000000, P_GPLL0_OUT_MAIN_DIV, 1.5, 0, 0),
	F(500000000, P_GPU_CC_PLL1_OUT_MAIN, 2, 0, 0),
	{ }
};

static struct clk_rcg2 gpu_cc_gmu_clk_src = {
	.cmd_rcgr = 0x9318,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gpu_cc_parent_map_1,
	.freq_tbl = ftbl_gpu_cc_gmu_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "gpu_cc_gmu_clk_src",
		.parent_data = gpu_cc_parent_data_1,
		.num_parents = ARRAY_SIZE(gpu_cc_parent_data_1),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_gpu_cc_hub_clk_src[] = {
	F(150000000, P_GPLL0_OUT_MAIN_DIV, 2, 0, 0),
	{ }
};

static struct clk_rcg2 gpu_cc_hub_clk_src = {
	.cmd_rcgr = 0x93ec,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gpu_cc_parent_map_2,
	.freq_tbl = ftbl_gpu_cc_hub_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "gpu_cc_hub_clk_src",
		.parent_data = gpu_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(gpu_cc_parent_data_2),
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_gpu_cc_xo_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	{ }
};

static struct clk_rcg2 gpu_cc_xo_clk_src = {
	.cmd_rcgr = 0x9010,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = gpu_cc_parent_map_3,
	.freq_tbl = ftbl_gpu_cc_xo_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "gpu_cc_xo_clk_src",
		.parent_data = gpu_cc_parent_data_3,
		.num_parents = ARRAY_SIZE(gpu_cc_parent_data_3),
		.ops = &clk_rcg2_ops,
	},
};

#define GPU_DIV(_name, _reg, _parent)					\
static struct clk_regmap_div _name = {					\
	.reg = _reg,							\
	.shift = 0,							\
	.width = 4,							\
	.clkr.hw.init = &(const struct clk_init_data) {			\
		.name = #_name,						\
		.parent_hws = (const struct clk_hw*[]) { &_parent.clkr.hw },\
		.num_parents = 1,					\
		.flags = CLK_SET_RATE_PARENT,				\
		.ops = &clk_regmap_div_ro_ops,				\
	},								\
}

GPU_DIV(gpu_cc_demet_div_clk_src, 0x9054, gpu_cc_xo_clk_src);
GPU_DIV(gpu_cc_hub_ahb_div_clk_src, 0x9430, gpu_cc_hub_clk_src);
GPU_DIV(gpu_cc_hub_cx_int_div_clk_src, 0x942c, gpu_cc_hub_clk_src);
GPU_DIV(gpu_cc_xo_div_clk_src, 0x9050, gpu_cc_xo_clk_src);

/* branch with a parent clock; variable name == clock name */
#define GPU_BRANCH_P(_name, _reg, _check, _parent, _flags, _ops)	\
static struct clk_branch _name = {					\
	.halt_reg = _reg,						\
	.halt_check = _check,						\
	.clkr = {							\
		.enable_reg = _reg,					\
		.enable_mask = BIT(0),					\
		.hw.init = &(const struct clk_init_data) {		\
			.name = #_name,					\
			.parent_hws = (const struct clk_hw*[]) { &_parent.clkr.hw },\
			.num_parents = 1,				\
			.flags = _flags,				\
			.ops = &_ops,					\
		},							\
	},								\
}

/* branch without a parent clock */
#define GPU_BRANCH_NP(_name, _reg, _check)				\
static struct clk_branch _name = {					\
	.halt_reg = _reg,						\
	.halt_check = _check,						\
	.clkr = {							\
		.enable_reg = _reg,					\
		.enable_mask = BIT(0),					\
		.hw.init = &(const struct clk_init_data) {		\
			.name = #_name,					\
			.ops = &clk_branch2_ops,			\
		},							\
	},								\
}

GPU_BRANCH_P(gpu_cc_ahb_clk, 0x911c, BRANCH_HALT_DELAY,
	     gpu_cc_hub_ahb_div_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_crc_ahb_clk, 0x9120, BRANCH_HALT_VOTED,
	     gpu_cc_hub_ahb_div_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_cx_ff_clk, 0x914c, BRANCH_HALT,
	     gpu_cc_ff_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_cx_gmu_clk, 0x913c, BRANCH_HALT,
	     gpu_cc_gmu_clk_src, CLK_SET_RATE_PARENT, clk_branch2_aon_ops);
GPU_BRANCH_P(gpu_cc_cxo_aon_clk, 0x9004, BRANCH_HALT_VOTED,
	     gpu_cc_xo_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_cxo_clk, 0x9144, BRANCH_HALT,
	     gpu_cc_xo_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_demet_clk, 0x900c, BRANCH_HALT,
	     gpu_cc_demet_div_clk_src, CLK_SET_RATE_PARENT, clk_branch2_aon_ops);
GPU_BRANCH_P(gpu_cc_freq_measure_clk, 0x9008, BRANCH_HALT,
	     gpu_cc_xo_div_clk_src, CLK_SET_RATE_PARENT, clk_branch2_ops);
GPU_BRANCH_P(gpu_cc_hub_aon_clk, 0x93e8, BRANCH_HALT,
	     gpu_cc_hub_clk_src, CLK_SET_RATE_PARENT, clk_branch2_aon_ops);
GPU_BRANCH_P(gpu_cc_hub_cx_int_clk, 0x9148, BRANCH_HALT,
	     gpu_cc_hub_cx_int_div_clk_src, CLK_SET_RATE_PARENT, clk_branch2_aon_ops);

GPU_BRANCH_NP(gpu_cc_hlos1_vote_gpu_smmu_clk, 0x7000, BRANCH_HALT_VOTED);
GPU_BRANCH_NP(gpu_cc_memnoc_gfx_clk, 0x9150, BRANCH_HALT);
GPU_BRANCH_NP(gpu_cc_mnd1x_0_gfx3d_clk, 0x9288, BRANCH_HALT_SKIP);
GPU_BRANCH_NP(gpu_cc_mnd1x_1_gfx3d_clk, 0x928c, BRANCH_HALT_SKIP);
GPU_BRANCH_NP(gpu_cc_sleep_clk, 0x9134, BRANCH_HALT_VOTED);

static struct clk_branch gpu_cc_cx_snoc_dvm_clk = {
	.halt_reg = 0x9130,
	.halt_check = BRANCH_HALT_VOTED,
	.clkr = {
		.enable_reg = 0x9130,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "gpu_cc_cx_snoc_dvm_clk",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_GCC_GPU_SNOC_DVM_GFX_CLK,
			},
			.num_parents = 1,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct gdsc gpu_cx_gdsc = {
	.gdscr = 0x9108,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x8,
	.pd = {
		.name = "gpu_cx_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
};

static struct gdsc gpu_gx_gdsc = {
	.gdscr = 0x905c,
	.clamp_io_ctrl = 0x9504,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x2,
	.pd = {
		.name = "gpu_gx_gdsc",
		.power_on = gdsc_gx_do_nothing_enable,
	},
	.parent = &gpu_cx_gdsc.pd,
	.pwrsts = PWRSTS_OFF_ON,
	.flags = CLAMP_IO | SW_RESET | AON_RESET | POLL_CFG_GDSCR,
};

static struct clk_regmap *gpu_cc_sm7435_clocks[] = {
	[GPU_CC_AHB_CLK] = &gpu_cc_ahb_clk.clkr,
	[GPU_CC_CRC_AHB_CLK] = &gpu_cc_crc_ahb_clk.clkr,
	[GPU_CC_CX_FF_CLK] = &gpu_cc_cx_ff_clk.clkr,
	[GPU_CC_CX_GMU_CLK] = &gpu_cc_cx_gmu_clk.clkr,
	[GPU_CC_CX_SNOC_DVM_CLK] = &gpu_cc_cx_snoc_dvm_clk.clkr,
	[GPU_CC_CXO_AON_CLK] = &gpu_cc_cxo_aon_clk.clkr,
	[GPU_CC_CXO_CLK] = &gpu_cc_cxo_clk.clkr,
	[GPU_CC_DEMET_CLK] = &gpu_cc_demet_clk.clkr,
	[GPU_CC_DEMET_DIV_CLK_SRC] = &gpu_cc_demet_div_clk_src.clkr,
	[GPU_CC_FF_CLK_SRC] = &gpu_cc_ff_clk_src.clkr,
	[GPU_CC_FREQ_MEASURE_CLK] = &gpu_cc_freq_measure_clk.clkr,
	[GPU_CC_GMU_CLK_SRC] = &gpu_cc_gmu_clk_src.clkr,
	[GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK] = &gpu_cc_hlos1_vote_gpu_smmu_clk.clkr,
	[GPU_CC_HUB_AHB_DIV_CLK_SRC] = &gpu_cc_hub_ahb_div_clk_src.clkr,
	[GPU_CC_HUB_AON_CLK] = &gpu_cc_hub_aon_clk.clkr,
	[GPU_CC_HUB_CLK_SRC] = &gpu_cc_hub_clk_src.clkr,
	[GPU_CC_HUB_CX_INT_CLK] = &gpu_cc_hub_cx_int_clk.clkr,
	[GPU_CC_HUB_CX_INT_DIV_CLK_SRC] = &gpu_cc_hub_cx_int_div_clk_src.clkr,
	[GPU_CC_MEMNOC_GFX_CLK] = &gpu_cc_memnoc_gfx_clk.clkr,
	[GPU_CC_MND1X_0_GFX3D_CLK] = &gpu_cc_mnd1x_0_gfx3d_clk.clkr,
	[GPU_CC_MND1X_1_GFX3D_CLK] = &gpu_cc_mnd1x_1_gfx3d_clk.clkr,
	[GPU_CC_PLL0] = &gpu_cc_pll0.clkr,
	[GPU_CC_PLL1] = &gpu_cc_pll1.clkr,
	[GPU_CC_SLEEP_CLK] = &gpu_cc_sleep_clk.clkr,
	[GPU_CC_XO_CLK_SRC] = &gpu_cc_xo_clk_src.clkr,
	[GPU_CC_XO_DIV_CLK_SRC] = &gpu_cc_xo_div_clk_src.clkr,
};

static struct gdsc *gpu_cc_sm7435_gdscs[] = {
	[GPU_CX_GDSC] = &gpu_cx_gdsc,
	[GPU_GX_GDSC] = &gpu_gx_gdsc,
};

static const struct qcom_reset_map gpu_cc_sm7435_resets[] = {
	[GPU_CC_ACD_BCR] = { 0x9358 },
	[GPU_CC_CX_BCR] = { 0x9104 },
	[GPU_CC_FAST_HUB_BCR] = { 0x93e4 },
	[GPU_CC_FF_BCR] = { 0x9470 },
	[GPU_CC_GFX3D_AON_BCR] = { 0x9198 },
	[GPU_CC_GMU_BCR] = { 0x9314 },
	[GPU_CC_GX_BCR] = { 0x9058 },
	[GPU_CC_RBCPR_BCR] = { 0x91e0 },
	[GPU_CC_XO_BCR] = { 0x9000 },
	[GPU_CC_FREQUENCY_LIMITER_IRQ_CLEAR] = { 0x9538, 0 },
};

static const struct regmap_config gpu_cc_sm7435_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x9988,
	.fast_io = true,
};

static const struct qcom_cc_desc gpu_cc_sm7435_desc = {
	.config = &gpu_cc_sm7435_regmap_config,
	.clks = gpu_cc_sm7435_clocks,
	.num_clks = ARRAY_SIZE(gpu_cc_sm7435_clocks),
	.resets = gpu_cc_sm7435_resets,
	.num_resets = ARRAY_SIZE(gpu_cc_sm7435_resets),
	.gdscs = gpu_cc_sm7435_gdscs,
	.num_gdscs = ARRAY_SIZE(gpu_cc_sm7435_gdscs),
};

static const struct of_device_id gpu_cc_sm7435_match_table[] = {
	{ .compatible = "qcom,sm7435-gpucc" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpu_cc_sm7435_match_table);

static int gpu_cc_sm7435_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	int ret;

	regmap = qcom_cc_map(pdev, &gpu_cc_sm7435_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	clk_lucid_evo_pll_configure(&gpu_cc_pll0, regmap, &gpu_cc_pll0_config);
	clk_lucid_evo_pll_configure(&gpu_cc_pll1, regmap, &gpu_cc_pll1_config);

	/* undocumented write done by the downstream driver */
	regmap_write(regmap, 0x9534, 0x0);

	ret = qcom_cc_really_probe(&pdev->dev, &gpu_cc_sm7435_desc, regmap);
	if (ret)
		return ret;

	/* in-rush current workaround from downstream (AXI/GMU sleep overlap) */
	regmap_update_bits(regmap, gpu_cc_cx_gmu_clk.clkr.enable_reg, 0xf0, 0xf0);

	return 0;
}

static struct platform_driver gpu_cc_sm7435_driver = {
	.probe = gpu_cc_sm7435_probe,
	.driver = {
		.name = "sm7435-gpucc",
		.of_match_table = gpu_cc_sm7435_match_table,
	},
};
module_platform_driver(gpu_cc_sm7435_driver);

MODULE_DESCRIPTION("QTI SM7435 GPU clock controller driver");
MODULE_LICENSE("GPL");
