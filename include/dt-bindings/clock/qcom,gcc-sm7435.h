/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, Ximi <ximiyadka124@proton.me>
 *
 * Clock and reset IDs for the Qualcomm SM7435 (Snapdragon 7s Gen 2)
 * Global Clock Controller.
 *
 * Only the subset required by the UFS host controller and the QMP UFS PHY
 * is described here.  The IDs are local to this driver and deliberately do
 * not match the (much larger) downstream "qcom,gcc-parrot" numbering.
 *
 * Downstream ID mapping for the implemented clocks:
 *   GCC_GPLL0                      <- GCC_GPLL0
 *   GCC_GPLL0_OUT_EVEN             <- GCC_GPLL0_OUT_EVEN
 *   GCC_AGGRE_UFS_PHY_AXI_CLK      <- GCC_AGGRE_UFS_PHY_AXI_CLK
 *   GCC_UFS_0_CLKREF_EN            <- GCC_UFS_0_CLKREF_EN
 *   GCC_UFS_PHY_AHB_CLK            <- GCC_UFS_PHY_AHB_CLK
 *   GCC_UFS_PHY_AXI_CLK            <- GCC_UFS_PHY_AXI_CLK
 *   GCC_UFS_PHY_AXI_CLK_SRC        <- GCC_UFS_PHY_AXI_CLK_SRC
 *   GCC_UFS_PHY_ICE_CORE_CLK       <- GCC_UFS_PHY_ICE_CORE_CLK
 *   GCC_UFS_PHY_ICE_CORE_CLK_SRC   <- GCC_UFS_PHY_ICE_CORE_CLK_SRC
 *   GCC_UFS_PHY_PHY_AUX_CLK        <- GCC_UFS_PHY_PHY_AUX_CLK
 *   GCC_UFS_PHY_PHY_AUX_CLK_SRC    <- GCC_UFS_PHY_PHY_AUX_CLK_SRC
 *   GCC_UFS_PHY_RX_SYMBOL_0_CLK    <- GCC_UFS_PHY_RX_SYMBOL_0_CLK
 *   GCC_UFS_PHY_RX_SYMBOL_0_CLK_SRC<- GCC_UFS_PHY_RX_SYMBOL_0_CLK_SRC
 *   GCC_UFS_PHY_RX_SYMBOL_1_CLK    <- GCC_UFS_PHY_RX_SYMBOL_1_CLK
 *   GCC_UFS_PHY_RX_SYMBOL_1_CLK_SRC<- GCC_UFS_PHY_RX_SYMBOL_1_CLK_SRC
 *   GCC_UFS_PHY_TX_SYMBOL_0_CLK    <- GCC_UFS_PHY_TX_SYMBOL_0_CLK
 *   GCC_UFS_PHY_TX_SYMBOL_0_CLK_SRC<- GCC_UFS_PHY_TX_SYMBOL_0_CLK_SRC
 *   GCC_UFS_PHY_UNIPRO_CORE_CLK    <- GCC_UFS_PHY_UNIPRO_CORE_CLK
 *   GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC<- GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC
 */

#ifndef _DT_BINDINGS_CLK_QCOM_GCC_SM7435_H
#define _DT_BINDINGS_CLK_QCOM_GCC_SM7435_H

/* GCC clocks */
#define GCC_GPLL0				0
#define GCC_GPLL0_OUT_EVEN			1
#define GCC_AGGRE_UFS_PHY_AXI_CLK		2
#define GCC_UFS_0_CLKREF_EN			3
#define GCC_UFS_PHY_AHB_CLK			4
#define GCC_UFS_PHY_AXI_CLK			5
#define GCC_UFS_PHY_AXI_CLK_SRC			6
#define GCC_UFS_PHY_ICE_CORE_CLK		7
#define GCC_UFS_PHY_ICE_CORE_CLK_SRC		8
#define GCC_UFS_PHY_PHY_AUX_CLK			9
#define GCC_UFS_PHY_PHY_AUX_CLK_SRC		10
#define GCC_UFS_PHY_RX_SYMBOL_0_CLK		11
#define GCC_UFS_PHY_RX_SYMBOL_0_CLK_SRC		12
#define GCC_UFS_PHY_RX_SYMBOL_1_CLK		13
#define GCC_UFS_PHY_RX_SYMBOL_1_CLK_SRC		14
#define GCC_UFS_PHY_TX_SYMBOL_0_CLK		15
#define GCC_UFS_PHY_TX_SYMBOL_0_CLK_SRC		16
#define GCC_UFS_PHY_UNIPRO_CORE_CLK		17
#define GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC		18

/* GCC resets */
#define GCC_UFS_PHY_BCR				0

/* GCC power domains */
#define UFS_PHY_GDSC				0

#endif /* _DT_BINDINGS_CLK_QCOM_GCC_SM7435_H */
