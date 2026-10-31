/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */

#ifndef _DT_BINDINGS_CLK_QCOM_GCC_SM7435_H
#define _DT_BINDINGS_CLK_QCOM_GCC_SM7435_H

/* GCC clocks */
#define GCC_UFS_PHY_AXI_CLK			0
#define GCC_AGGRE_UFS_PHY_AXI_CLK		1
#define GCC_UFS_PHY_AHB_CLK			2
#define GCC_UFS_PHY_UNIPRO_CORE_CLK		3
#define GCC_UFS_PHY_TX_SYMBOL_0_CLK		4
#define GCC_UFS_PHY_RX_SYMBOL_0_CLK		5
#define GCC_UFS_PHY_RX_SYMBOL_1_CLK		6
#define GCC_UFS_PHY_PHY_AUX_CLK			7
#define GCC_UFS_0_CLKREF_EN			8

/* GCC resets */
#define GCC_UFS_PHY_BCR				0

/* GCC power domains */
#define UFS_PHY_GDSC				0

#endif
