# UFS port for Xiaomi Redmi Note 13 Pro 5G (garnet, Qualcomm SM7435 / "Parrot")

## Goal

Bring up the embedded UFS device of the garnet board in this mainline kernel
(`garnet-mainline`), which so far only contains a stage-1 skeleton (CPU, GIC,
timer, ramoops) plus an experimental dwc3/USB port.

## Sources used

| What | Where |
| --- | --- |
| Downstream kernel | `LineageOS/android_kernel_xiaomi_sm7435` (branch `lineage-23.2`, base `sm8450/lineage-20`) |
| Downstream device trees | `LineageOS/android_kernel_xiaomi_sm7435-devicetrees` |
| Stock device tree | `garnet-linux/fdt` (`garnet.dts`, decompiled stock DTB) |

## Hardware facts established during the port

### SoC / board

* SM7435 ("Parrot") is an SM8450 ("Waipio") derivative for the CPU/PCIe side, but
  with newer (LUCID_EVO) alpha PLLs.
* UFS host controller: `0x1d84000` (core, 0x3000), ICE `0x1d88000`, ICE hwkm
  `0x1d90000`, interrupt **GIC_SPI 265** (level high).
* QMP UFS PHY: `0x1d87000`, 2 lanes per direction, reference clock 19.2 MHz.
* GCC: base `0x100000`, size `0x1f4200` (identical to the mainline SM8450 GCC
  node geometry).

### Downstream UFS PHY: Parrot == Waipio

`drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-parrot.c` and
`...-v4-waipio.c` in the downstream kernel are **byte-identical** except for

* the file names / driver name,
* `#define COM_SIZE 0x1C8` (parrot) vs `0x1C0` (waipio) — a legacy-driver-only
  constant that describes the size of the common register block; it does not
  appear in the mainline (CAF `ufs-qmp-phy` style) driver.

The legacy downstream register map (`COM_BASE 0x000`, `PHY_BASE 0xc00`,
`TX_BASE 0x400`, `RX_BASE 0x600`) is identical to the block offsets used by the
mainline `phy-qcom-qmp-ufs.c` driver (`qmp_ufs_offsets`), so the mainline
driver models the same hardware.

### Downstream GCC (Parrot) UFS clocks

Taken verbatim from `drivers/clk/qcom/gcc-parrot.c`:

| Clock | Register | Notes |
| --- | --- | --- |
| `gcc_gpll0` | alpha PLL @ `0x0`, enable `0x62018` bit 0 | LUCID_EVO, parent `bi_tcxo` |
| `gcc_gpll0_out_even` | postdiv, shift 10, table `{0x1 -> 2}` | LUCID_EVO |
| `gcc_ufs_phy_axi_clk_src` | RCG2 `0x8702c`, mnd 8, hid 5 | 25/75/150/300 MHz |
| `gcc_ufs_phy_ice_core_clk_src` | RCG2 `0x87074`, mnd 0, hid 5 | 75/150/300 MHz |
| `gcc_ufs_phy_unipro_core_clk_src` | RCG2 `0x8708c`, mnd 0, hid 5 | 75/150/300 MHz |
| `gcc_ufs_phy_phy_aux_clk_src` | RCG2 `0x870a8`, mnd 0, hid 5 | 9.6/19.2 MHz from `bi_tcxo` |
| `gcc_ufs_phy_rx_symbol_0_clk_src` | mux `0x87060` (2 bit) | parents: PHY `rx_symbol_0`, `bi_tcxo` |
| `gcc_ufs_phy_rx_symbol_1_clk_src` | mux `0x870d0` (2 bit) | parents: PHY `rx_symbol_1`, `bi_tcxo` |
| `gcc_ufs_phy_tx_symbol_0_clk_src` | mux `0x87050` (2 bit) | parents: PHY `tx_symbol_0`, `bi_tcxo` |
| `gcc_aggre_ufs_phy_axi_clk` | branch `0x870d4` | |
| `gcc_ufs_phy_axi_clk` | branch `0x87018` | |
| `gcc_ufs_phy_ahb_clk` | branch `0x87020` | |
| `gcc_ufs_phy_ice_core_clk` | branch `0x8706c` | |
| `gcc_ufs_phy_phy_aux_clk` | branch `0x870a4` | |
| `gcc_ufs_phy_unipro_core_clk` | branch `0x87064` | |
| `gcc_ufs_phy_rx_symbol_0_clk` | branch `0x87028` | |
| `gcc_ufs_phy_rx_symbol_1_clk` | branch `0x870c0` | |
| `gcc_ufs_phy_tx_symbol_0_clk` | branch `0x87024` | |
| `gcc_ufs_0_clkref_en` | branch `0x9c000` | |
| `gcc_ufs_phy_gdsc` | GDSC `0x87004` | confirmed by the downstream `qcom,gdsc@187004` node (`0x100000 + 0x87004`) |
| `GCC_UFS_PHY_BCR` | reset `0x87000` | |

All of these offsets were verified to be identical in the downstream
`gcc-parrot.c` driver (they are *not* copied from the SM8450 driver).

## Changes made

### 1. `include/dt-bindings/clock/qcom,gcc-sm7435.h` (new)

Clock/reset/power-domain IDs for the SM7435 GCC.  The numbering is local to
this driver (the downstream `qcom,gcc-parrot` numbering is much larger); the
mapping to the downstream IDs is documented in the header.

### 2. `drivers/clk/qcom/gcc-sm7435.c` (new)

Focused GCC clock driver for SM7435, built on the mainline
`qcom-clk-ccm` framework (`common.h`, `gdsc.h`, `reset.h`):

* `gcc_gpll0` + `gcc_gpll0_out_even` (LUCID_EVO),
* the UFS PHY clock roots, branches and symbol clock multiplexers listed above,
* the UFS PHY global domain power controller (`UFS_PHY_GDSC`),
* the UFS PHY block reset (`GCC_UFS_PHY_BCR`).

Only the UFS related clocks are implemented; everything else keeps being
handled by the bootloader as before.

Build glue: `drivers/clk/qcom/Makefile` (`CONFIG_SM_GCC_7435`) and
`drivers/clk/qcom/Kconfig` (`SM_GCC_7435`, selects `QCOM_GDSC`).

### 3. `drivers/phy/qualcomm/phy-qcom-qmp-ufs.c`

* new `sm7435_ufsphy_cfg` (same init tables/layout as the SM8450 PHY, which is
  the same hardware as the downstream Parrot PHY),
* new compatible `qcom,sm7435-qmp-ufs-phy`,
* `devm_regulator_bulk_get_const()` is now only called when the SoC
  configuration actually has regulators (`num_vregs != 0`).  SM7435 has none
  for now because the PMIC LDOs are not driven by mainline yet (see below).

### 4. `drivers/ufs/host/ufs-qcom.c` / `ufs-qcom.h`

* new compatible `qcom,sm7435-ufshc` with a new `ufs_qcom_sm7435_drvdata`,
* new `no_icc` flag in `struct ufs_qcom_drvdata`: the generic `qcom,ufshc`
  path requires the `ufs-ddr` / `cpu-ufs` interconnect paths, and this kernel
  has no NOC/interconnect support for the SM7435 yet.  Without the flag the
  host controller would fail to probe with "failed to acquire interconnect
  path".  Bandwidth votes are simply skipped (`icc_set_bw()` handles a NULL
  path), so the driver change only relaxes a hard requirement.

### 5. `arch/arm64/boot/dts/qcom/sm7435.dtsi`

* `bi_tcxo`: 19.2 MHz fixed clock (stand-in for the RPMh CXO),
* `gcc: clock-controller@100000` (`qcom,gcc-sm7435`),
* `ufs_mem_hc: ufshc@1d84000` (`qcom,sm7435-ufshc`, `qcom,ufshc`),
* `ufs_mem_phy: phy@1d87000` (`qcom,sm7435-qmp-ufs-phy`).

### 6. `arch/arm64/boot/dts/qcom/sm7435-xiaomi-garnet.dts`

Enables `ufs_mem_hc` and `ufs_mem_phy` (both are `status = "ok"` in the stock
device tree).

### 7. Device tree bindings

* new `Documentation/devicetree/bindings/clock/qcom,gcc-sm7435.yaml`,
* `qcom,sm7435-qmp-ufs-phy` added to
  `Documentation/devicetree/bindings/phy/qcom,sc8280xp-qmp-ufs-phy.yaml`,
* `qcom,sm7435-ufshc` added to
  `Documentation/devicetree/bindings/ufs/qcom,ufs.yaml`.

## Known gaps / remaining runtime work

1. **PMIC rails are not driven.**  The UFS rails (vcc = PM6450 L24, vccq =
   PM6450 L13, vccq2 = PM6450 L19, vdda-phy = PM6450 L5, vdda-pll = PM6450
   L16) are exposed downstream through RPMh resources ("qcom,rpmh-vrm-regulator"
   with `pmic5-ldo` type).  Mainline's `qcom-rpmh-regulator` driver supports a
   different binding and does not know the PM6450.  Until that is ported the
   rails are left enabled by the bootloader, and the UFS PHY therefore does not
   request any regulator.
2. **RPMh clock.**  `bi_tcxo` is a fixed 19.2 MHz clock instead of the real
   RPMh CXO (the downstream device tree points at `rpmh` clock id 0).  This is
   correct in frequency (the UFS reference clock is 19.2 MHz) but it is not
   power managed.
3. **No SMMU/NOC/interconnect** support in this tree yet, so the UFS node uses
   `dma-coherent` and no interconnect entries.
4. **Inline crypto (ICE)** is not enabled (no `qcom,ice` phandle); the UFS
   driver tolerates its absence.
5. The GCC driver implements only the UFS clocks — the other GCC clocks of the
   SoC are still only available through the bootloader.
6. `CONFIG_SM_GCC_7435` needs to be enabled (it defaults to `ARCH_QCOM`).

## Runtime test checklist (on the device)

1. `dmesg | grep -i ufs` — the host controller must probe and the link must
   reach `UFS_HB_MODE_`/`ufshcd` "Link is up" with the expected gear.
2. `ls /sys/class/ufs` / `cat /sys/class/ufs/ufs*/device/description` —
   the UFS device must be enumerated.
3. `dmesg | grep -i "ufs_phy\|gcc"` — the PHY must report `PCS ready` and no
   `link start failed` / `pll lock` errors.
4. Mount the UFS partition and check I/O (`dd`, `fio`) — this validates the
   AXI/AHB/UNIPRO clocks and the 19.2 MHz reference clock.
5. Check `cat /sys/kernel/debug/clk/gcc_sm7435/clk_summary` (or
   `/sys/kernel/debug/clk/*`) for the UFS clock rates: core clock 300 MHz,
   UNIPRO core 300 MHz, ref clock 19.2 MHz.
6. Verify that the link survives suspend/resume (`echo suspend >/sys/power/state`
   → resume → repeat the I/O test) and that the GDSC is powered off/on
   correctly.
7. Compare the behaviour with the downstream kernel for the same partition
   (e.g. `dmesg` gear/rate negotiation).
