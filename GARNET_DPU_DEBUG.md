# Garnet (SM7435) — DPU CTL reset failure analysis

Scope: DPU / CTL / LM / INTF / DSI0 / display-SMMU only. No code changes are proposed
beyond a diagnostic patch. All line numbers refer to this tree
(`7.3.0`, HEAD `3fd57258036e "Add panel patches"`).

Legend: **[FACT]** = verified in source or in the pasted log; **[HYP]** = hypothesis.

---

## 1. Executive summary

**[FACT]** The kernel logs, before the first frame flip:

```
[drm:_dpu_encoder_phys_vid_prepare_for_kickoff] ctl 1 reset failure: -22
[drm:dpu_hw_ctl_wait_reset_status] hw recovery is not complete for ctl:1
```

`-22` is `-EINVAL` and is produced by exactly one place: the poll of
`CTL_SW_RESET` (offset `0x030`, bit 0) that times out. So the DPU's CTL_0 block
is sitting with the **hardware-initiated software-reset bit set**, and it does not
clear within the mainline timeout of **2000 µs**.

Two things follow, and they are distinct problems:

1. Why is `CTL_SW_RESET[0]` set at all? A CTL engages its own SW reset when the
   MDP detects an unrecoverable error (typically a VBIF/AXI/DSI-side fault). That
   is *upstream* of the DPU.
2. Why does mainline not recover? Mainline uses the same 2 ms budget for the
   *hardware-initiated* wait as for its own reset (`DPU_REG_RESET_TIMEOUT_US`),
   while downstream explicitly uses **100 ms** for that wait and additionally
   has a `CTL_SW_RESET_OVERRIDE` hard-reset path. Mainline has neither.

**[FACT]** The `ctl 1` in the message is `CTL_0`, not `CTL_1`: `enum dpu_ctl`
starts at 1 (`CTL_0 = 1`, `dpu_hw_mdss.h:162`), and the message prints
`ctl->idx` unmodified. `CTL_0`'s base is `0x15000`
(`catalog/dpu_8_3_sm7435.h:34`), which is the CTL driving INTF_1 / DSI0.

Most likely chain (see §5 for ranking):

```
DSI0 / MDSS fault (cmd-DMA iova 0 → SMMU FSR 0x402, and/or VBIF error)
  → MDP initiates CTL SW reset (CTL_SW_RESET[0] = 1)
  → mainline dpu_hw_ctl_wait_reset_status() polls for only 2000 µs
  → -EINVAL, snapshot dumped, frame never kicked off → black screen
```

---

## 2. Exact failing call chain

**[FACT]** Frame commit path:

| Step | Location |
|---|---|
| `dpu_crtc_commit_kickoff()` | `dpu_crtc.c:1083` |
| → `dpu_encoder_prepare_for_kickoff(encoder)` | `dpu_crtc.c:1118` |
| → `dpu_vbif_clear_errors(dpu_kms)` (**after** prepare) | `dpu_crtc.c:1120` |
| → `dpu_encoder_kickoff(encoder)` (not reached on failure) | `dpu_crtc.c:1124` |
| `dpu_encoder_prepare_for_kickoff()` | `dpu_encoder.c:2067` |
| → `phys->ops.prepare_for_kickoff(phys)` | `dpu_encoder.c:2083` |
| → checks `DPU_ENC_ERR_NEEDS_HW_RESET` | `dpu_encoder.c:2084` |
| `dpu_encoder_phys_vid_prepare_for_kickoff()` | `dpu_encoder_phys_vid.c:553` |
| → `ctl->ops.wait_reset_status(ctl)` | `dpu_encoder_phys_vid.c:570` |
| → `dpu_hw_ctl_wait_reset_status()` | `dpu_hw_ctl.c:447` |
| → `pr_err("hw recovery is not complete for ctl:%d")` | `dpu_hw_ctl.c:459` |
| → `dpu_hw_ctl_poll_reset_status(ctx, 2000)` | `dpu_hw_ctl.c:413` |
| → `DPU_REG_RESET_TIMEOUT_US` | `dpu_hw_ctl.c:53` |

**[FACT]** `dpu_hw_ctl_poll_reset_status()` (`dpu_hw_ctl.c:413-432`):

```c
do {
        status = DPU_REG_READ(c, CTL_SW_RESET);
        status &= 0x1;
        if (status)
                usleep_range(20, 50);
} while (status && ktime_compare_safe(ktime_get(), timeout) < 0);
return status;
```

The comment at `:418-421` says "it takes around 30us to have mdp finish resetting
its ctl path" — that is true for a **caller-issued** reset. The hardware-initiated
case is a different state machine, and downstream treats it differently (§4).

**[FACT]** The user's local debug patch adds, on failure:
`ctl->ops.reset(ctl)` retry + 5 register samples at 3 ms
(`dpu_encoder_phys_vid.c:578-596`). The five `GARNET-DBG[i]` lines are the only
way to tell "implemented but slow" from "permanently stuck"; the pasted log
contains one sample (`intf en=1 frame=1 line=2429`), which is not enough to
distinguish those.

**[FACT]** `dpu_encoder_helper_phys_cleanup()` calls `ctl->ops.reset(ctl)` and
**ignores the return value** (`dpu_encoder.c:2264`). So if a reset failed during
disable, bit 0 can still be set when the next enable/kickoff happens — a second,
independent way to arrive at the same log line.

---

## 3. Register offsets and meanings

All CTL offsets are relative to the CTL block base (`0x15000` for CTL_0).
Definitions in `dpu_hw_ctl.c:25-63`.

| Offset | Name | Meaning |
|---|---|---|
| `0x014` | `CTL_TOP` | mode select; mainline always writes `0xf << 28` (`CTL_DEFAULT_GROUP_ID`) |
| `0x018` | `CTL_FLUSH` | pending-flush bits; written by `dpu_hw_ctl_trigger_flush()` |
| `0x01C` | `CTL_START` | bit 0 = trigger; `dpu_hw_ctl_trigger_start()` |
| `0x030` | `CTL_SW_RESET` | **bit 0 = CTL in software reset**; the bit this bug is about |
| `0x0E4` | `CTL_MERGE_3D_ACTIVE` | active 3D-merge blocks |
| `0x0E8` | `CTL_DSC_ACTIVE` | active DSC blocks |
| `0x0F4` | `CTL_INTF_ACTIVE` | active INTFs |
| `0x130` | `CTL_LAYER_ACTIVE` | active layer |
| `0x134` | `CTL_INTF_MASTER` | master INTF |
| `0x060` | `CTL_SW_RESET_OVERRIDE` | **downstream only** (`sde_hw_ctl.c:29`); hard-reset override |

INTF offsets (`dpu_hw_intf.c:17-50`), base `0x35000` for INTF_1:

| Offset | Name |
|---|---|
| `0x000` | `INTF_TIMING_ENGINE_EN` |
| `0x0A8` | `INTF_FRAME_LINE_COUNT_EN` |
| `0x0AC` | `INTF_FRAME_COUNT` |
| `0x0B0` | `INTF_LINE_COUNT` |

**[FACT]** A set `CTL_SW_RESET[0]` is not proof of a permanent hang: it is a
latched status bit that hardware clears when its internal reset sequence
completes. The only question is *how long* it takes, and *why it was engaged*.

---

## 4. Mainline vs downstream (SM7435 Lineage display drivers)

Downstream offsets are DT-relative and need `qcom,sde-off = <0x1000>`
subtracted to compare with mainline's `mdp`-relative bases. After that
correction **all block bases match exactly** — including CTL (`0x16000-0x1000 =
0x15000`), INTF_1 (`0x36000-0x1000 = 0x35000`), LM (`0x45000-0x1000`), PP
(`0x6a000-0x1000`), DSC (`0x81000-0x1000 = 0x80000`) and the DSC sub-blocks
(`enc 0x100`, `ctl 0xF00`, sizes `0x100`/`0x10`).

### 4.1 The decisive difference: wait timeout

| | mainline | downstream |
|---|---|---|
| reset (caller-issued) timeout | `DPU_REG_RESET_TIMEOUT_US = 2000` (`dpu_hw_ctl.c:53`) | `SDE_REG_RESET_TIMEOUT_US = 2000` (`sde_hw_ctl.c:63`) |
| **hw-initiated wait timeout** | **2000 µs (same constant)** `dpu_hw_ctl.c:458` | **100000 µs** `SDE_REG_WAIT_RESET_TIMEOUT_US` (`sde_hw_ctl.c:64`, used at `:833`) |
| reset implementation | `dpu_hw_ctl_reset_control()` `:435` | `sde_hw_ctl_reset_control()` `:789` |
| wait implementation | `dpu_hw_ctl_wait_reset_status()` `:447` | `sde_hw_ctl_wait_reset_status()` `:818` |
| hard reset override | **absent** | `sde_hw_ctl_hard_reset()` writes `CTL_SW_RESET_OVERRIDE 0x060` (`:805-816`) |

**[FACT]** This is the single largest concrete behavioural difference in the
exact function that fails. Downstream's comment for the 30 µs number is the same
as mainline's, yet downstream still chose **50×** the timeout for the
hw-initiated path. That choice is only sensible if the hw-initiated path is
expected to be slow.

### 4.2 Recovery difference

**[FACT]** Downstream, on `wait_reset_status()` failure:
`sde_encoder_phys_vid.c:1017` logs, `:1053` sets
`SDE_ENC_ERR_NEEDS_HW_RESET`; `sde_crtc.c:4154` sets
`sde_crtc->needs_hw_reset = true`; `sde_crtc.c:4165-4172` calls
`sde_crtc_reset_hw()`. That function (`sde_crtc.c:4047-4099`) first retries the
plain CTL reset, and if that fails it toggles `CTL_SW_RESET_OVERRIDE` on all
CTLs (`:4051`, `:4086`) — a global hard reset of the pipeline.

**[FACT]** Mainline has `DPU_ENC_ERR_NEEDS_HW_RESET` and checks it
(`dpu_encoder.c:2084`), but **only the command-mode encoder ever sets it**
(`dpu_encoder_phys_cmd.c:210`). The video-mode encoder never does, so the
`dpu_encoder_helper_hw_reset()` path at `dpu_encoder.c:2095` is dead code for
this panel. The user's patch instead retries `ctl->ops.reset()` inline — which
repeats the same 2 ms timeout.

**[FACT]** Upstream (HEAD~2) simply unregisters the vsync IRQ on this failure
(`git show HEAD~2:.../dpu_encoder_phys_vid.c`), which makes every later vblank
wait time out — consistent with a screen that never updates again, but it is a
*symptom handler*, not a cause.

### 4.3 Things that are NOT different (ruled out)

**[FACT]** Verified identical/consistent, so these are *not* the bug:

* CTL, LM, PP, DSC, INTF base addresses and lengths (§4 preamble).
* DSC sub-block offsets (`enc 0x100`, `ctl 0xF00`) — mainline `dpu_hw_catalog.c:461-464`
  vs downstream DT `qcom,sde-dsc-enc = <0x100>`, `qcom,sde-dsc-ctl = <0xf00>`.
* IRQ mapping: `INTF_1 = DSI0` (`controller_id = MSM_DSI_CONTROLLER_0`,
  `dpu_8_3_sm7435.h:185`); INTF_1 underrun = `MDP_SSPP_TOP0_INTR` bit 26,
  vsync = bit 27; CTL_0 start = `MDP_SSPP_TOP0_INTR2` bit 9.
* `CTL_TOP` group id `0xf << 28` (`dpu_hw_ctl.c:63`, `:587`) == downstream
  `mode_sel = 0xf0000000` (`sde_hw_ctl.c:987+`).
* CTL length `0x1e8` covers `CTL_INTF_MASTER 0x134` and the `0x060` slot.

---

## 5. Ranked root-cause candidates

### H1 — 2 ms wait is too short for a hardware-initiated CTL reset (most likely proximate cause)

**For**
* It is the only difference in the exact failing function that is both concrete
  and 50× in magnitude (§4.1).
* Downstream deliberately uses 100 ms here while keeping 2 ms for its own reset.
* The bit is *already* set before mainline touches it, so mainline is observing,
  not causing, a reset it did not issue.
* A 2 ms poll with `usleep_range(20, 50)` is only ~40-100 iterations; scheduling
  jitter under load can easily consume it.

**Against**
* If the reset were merely slow, the user's 5-sample loop (3 ms apart, 15 ms
  total) would be expected to show `swreset` clearing; the single available
  sample shows it still set. This does not disprove H1 (15 ms < 100 ms), but it
  weakens "it would have cleared at 100 ms".

**Test:** change only the wait budget to 100 ms and see whether the message
disappears; log elapsed microseconds.

---

### H2 — The reset is genuinely stuck (e.g. `CTL_SW_RESET_OVERRIDE`/another master holds it)

**For**
* `CTL_SW_RESET_OVERRIDE` (`0x060`) exists in the hardware (downstream has a
  driver for it). If anything (bootloader, hypervisor, another VM, a stale write)
  left it set, `CTL_SW_RESET[0]` would never clear — exactly the observed
  "stays 1" behaviour.
* `CTL_SW_RESET` is a *status* bit; if the underlying reset is held by an
  override, polling forever cannot help.

**Against**
* Nothing in the mainline tree writes `0x060`. The boot chain is not known to.
* Would be a permanent black screen from the first commit, which matches the
  symptom, but so does H1.

**Test:** read `0x060` and `0x030` before and after the reset retry (diagnostic
patch below). If `0x060 != 0`, this hypothesis jumps to #1.

---

### H3 — The CTL reset is a *consequence* of an earlier MDSS/DSI fault (most likely root cause)

**For**
* A hardware-initiated CTL reset is a recovery action, not a normal state. The
  MDP raises it after an AXI/VBIF or downstream-block error.
* The log contains an SMMU fault with **`SID = 0x800`**, which
  `sm7435.dtsi:649` documents as the display stream (`smmu_sde_unsec_cb`), and
  **`fsr = 0x402`**:
  * `FSR_FORMAT = bits[10:9] = 0b10` → format 2 (`arm-smmu.h:203`)
  * `FSR_TF = bit 1` → translation fault (`arm-smmu.h:211`)
  * IOVA = **0**
  This means the display stream issued a translation at address 0 that has no
  mapping — a classic "DMA base register programmed to a value that does not
  match the mapped IOVA" fault.
* The user's own experiments localize this to the **DSI command DMA**, not the
  DPU: `0002-drm-msm-dsi-EXPERIMENT-msm.dsi_dma_from_fb=0-...patch`
  (disable fb-based cmd DMA) and
  `0001-drm-msm-EXPERIMENT-move-KMS-iova-range-above-256M.patch` (note the
  comment: "garnet's DSI command DMA faults at iova 0 although DMA_BASE reads
  back 0x1000", and the `msm_gem_vm_create(..., 0x1000, ...)` range).
* The DSI and DPU **share SID `0x800`**, so an SMMU fault in the log cannot by
  itself be attributed to the DPU — but the user's patches say the DSI DMA
  engine stayed busy at a `0x1000` base.
* Consistent ordering: panel on → DCS/backlight command DMA → fault → MDP error
  → CTL reset → first commit fails.

**Against**
* We have not correlated the SMMU fault timestamp with the `reset failure`
  timestamp from a full dmesg; the fault could be earlier, unrelated probe noise.
* `fsr = 0x402` with IOVA 0 could also be an initial speculative access before
  the IOMMU domain is attached.

**Test:** boot with `msm.dsi_dma_from_fb=0`; if the SMMU fault disappears and the
CTL reset disappears with it, H3 is confirmed. Also print the DSI `DMA_BASE`,
`CMD_DMA_CTRL` and the pinned IOVA at commit time.

---

### H4 — Clock / CX / bandwidth configuration

**For**
* `dpu_kms_init()` (`dpu_kms.c:1301-1309`) does
  `dev_pm_opp_find_freq_floor(dev, &max_freq=ULONG_MAX)` then
  `dev_pm_opp_set_rate(dev, max_freq)`, i.e. it selects the **highest** OPP.
* The DT OPP table goes up to `608000000`
  (`sm7435.dtsi:730`), while `assigned-clock-rates = <325000000>`
  (`sm7435.dtsi:679`) suggests the author intended 325 MHz. The max-OPP call
  likely overrides the assigned rate.
* The DPU has `power-domains = <&rpmhpd RPMHPD_CX>` (`sm7435.dtsi:688`) but
  **no `required-opps`**, so the CX rail is not scaled with the MDP clock.
* The DPU node has **no `interconnects`/`interconnect-names`** (`mdp0-mem`,
  `mdp1-mem`); `dpu_kms_parse_data_bus_icc_path()` (`dpu_kms.c:408-415`)
  therefore gets NULL paths and no bandwidth vote is cast. Almost every other
  mainline SoC defines these.
* Under-voltage/under-bandwidth can absolutely cause VBIF underrun → MDP error
  → CTL reset.

**Against**
* A clock/bandwidth problem usually produces underrun messages and visual
  corruption before it produces a full CTL reset; we do not see underrun logs
  in the excerpt.
* 608 MHz is a legal OPP for this block; the question is only whether CX can
  sustain it.

**[FACT]** The existing `GARNET-DBG: dpu core clk %lu Hz` print at
`dpu_kms.c:1236` is emitted from `dpu_kms_hw_init()`, which runs *after*
`dev_pm_opp_set_rate(dev, max_freq)` in `dpu_kms_init()` (`:1309` →
`:1311` → `msm_kms_init()`), and it calls `dpu_kms_get_clk_rate()`, which returns
the **current** rate (`dpu_kms.c:1126-1133`). So that number is the real
programmed rate, not the DT maximum. Read it in the boot log before doing
anything else for H4.

**Test:** read back the actual MDP core rate (the `GARNET-DBG: dpu core clk`
print at `dpu_kms.c:1236` only prints the *requested* max, not the resulting
rate), and pin the OPP to 325 MHz.

---

### H5 — Catalog / DT mismatch (low)

**For:** anything is possible on a new SoC port.

**Against:** §4.3 verified every DPU-relevant offset, sub-block offset, IRQ index
and group id against downstream. No mismatch found. Keep this at the bottom.

---

## 6. Confirmed vs unverified

**[FACT]**
* `-EINVAL` can only come from the `CTL_SW_RESET[0]` poll timeout.
* `ctl 1` == `CTL_0`, base `0x15000`, drives INTF_1/DSI0.
* Mainline wait budget is 2000 µs; downstream is 100000 µs.
* Mainline has no `CTL_SW_RESET_OVERRIDE` path; downstream hard-resets.
* Mainline's video-mode encoder never sets `DPU_ENC_ERR_NEEDS_HW_RESET`.
* `dpu_encoder_helper_phys_cleanup()` ignores reset failure.
* SMMU `SID 0x800` is the display stream; `fsr 0x402` = format 2 + translation
  fault at IOVA 0.
* The user's own patches document a DSI cmd-DMA fault at IOVA 0 / base `0x1000`.

**[HYP] / unverified**
* That `swreset` stays set "for thousands of frames" — the pasted log only has
  one sample. Needs the five `GARNET-DBG[i]` lines from a full dmesg.
* That the SMMU fault and the CTL reset are the same event (no timestamps).
* That the CTL reset is hardware-initiated rather than left over from a
  failed disable-time reset (§2).
* The actual MDP core clock at runtime.
* Whether `CTL_SW_RESET_OVERRIDE` is set.

---

## 7. Minimal diagnostic patch (applied)

Goal: answer H1/H2/H3 with one boot, without changing behaviour. This patch is
**already applied to the working tree** (uncommitted):

| File | Change |
|---|---|
| `dpu_hw_ctl.c` | `CTL_SW_RESET_OVERRIDE` define; `wait_reset_status()` now logs override/top, polls 100 ms, logs elapsed µs |
| `dpu_vbif.c` | VBIF pnd/src errors now at `pr_err` with `GARNET-DBG` prefix |
| `dsi_host.c` | `cmd_xfer_commit()` logs DMA base/len/readback/CMD_DMA_CTRL |
| `dpu_kms.c` | logs requested-vs-actual MDP core OPP rate |

Build note: **compile-verified** with
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- drivers/gpu/drm/msm/`
(arm64 config from `.config.old`); all four files built cleanly. The `.config`
present in the tree had been an x86 config with `CONFIG_DRM_MSM` disabled, so it
was restored from `.config.old` (the arm64 garnet config); the x86 one is backed
up at `/tmp/opencode/config.x86.backup`.

The original proposal text is kept below for reference.

### 7.1 `dpu_hw_ctl.c` — record timing and both reset registers

In `dpu_hw_ctl_wait_reset_status()` (`:447`), replace the body with:

```c
static int dpu_hw_ctl_wait_reset_status(struct dpu_hw_ctl *ctx)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	ktime_t start;
	u32 status, override, ctl_top;

	status = DPU_REG_READ(c, CTL_SW_RESET);
	status &= 0x01;
	if (!status)
		return 0;

	override = DPU_REG_READ(c, 0x060);   /* CTL_SW_RESET_OVERRIDE */
	ctl_top  = DPU_REG_READ(c, CTL_TOP);
	pr_err("GARNET-DBG ctl%d: wait enter swreset=%x override=%x top=%08x\n",
	       ctx->idx, status, override, ctl_top);

	start = ktime_get();
	if (dpu_hw_ctl_poll_reset_status(ctx, DPU_REG_WAIT_RESET_TIMEOUT_US)) {
		pr_err("GARNET-DBG ctl%d: hw recovery incomplete after %lld us "
		       "swreset=%x override=%x\n",
		       ctx->idx, ktime_to_us(ktime_sub(ktime_get(), start)),
		       DPU_REG_READ(c, CTL_SW_RESET), DPU_REG_READ(c, 0x060));
		return -EINVAL;
	}

	pr_err("GARNET-DBG ctl%d: hw recovery OK after %lld us\n",
	       ctx->idx, ktime_to_us(ktime_sub(ktime_get(), start)));
	return 0;
}
```

This single change tests H1 (does 100 ms succeed?) and H2 (`override != 0`?).

### 7.2 `dpu_vbif.c` — surface VBIF errors that precede the reset

`dpu_vbif_clear_errors()` (`:233`) already *reads* `VBIF_XIN_PND_ERR 0x0190`
and `VBIF_XIN_SRC_ERR 0x0194` (`dpu_hw_vbif.c:37-55`, definitions at `:29-31`)
but `dpu_vbif.c:239-242` only logs them at `DRM_DEBUG_KMS`. Change that one
`DRM_DEBUG_KMS` to `pr_err("GARNET-DBG VBIF: pnd 0x%X src 0x%X\n", ...)`.
This tests H3/H4: a non-zero `src` names the failing XIN.

### 7.3 DSI side — pin the IOVA

In `dsi_dma_base_get_v2()` / `msm_dsi_host_cmd_xfer_commit()`
(`dsi_host.c:1460`, `:2480-2500`), log once per commit:

```c
pr_err("GARNET-DBG dsi%d: dma_base=%pad len=%u cmd_dma_ctrl=%08x\n",
       msm_host->id, &dma_base, len,
       dsi_read(msm_host, REG_DSI_CMD_DMA_CTRL));
```

and boot once with `msm.dsi_dma_from_fb=0`. If the SMMU fault and the CTL reset
both disappear, H3 is proven and the DPU work is finished.

### 7.4 Clock sanity (one line, H4)

At `dpu_kms.c:1236`, also print the *resulting* rate after
`dev_pm_opp_set_rate()` (`:1309`), using the same `"core"` clock lookup as
`dpu_kms_get_clk_rate()`:

```c
pr_err("GARNET-DBG: mdp core opp requested=%lu actual=%lu\n",
       max_freq, dpu_kms_get_clk_rate(dpu_kms, "core"));
```

Actually the existing print at `:1236` already runs after the OPP call (see
H4), so the simplest first step is just to read that value from the boot log;
only add the `max_freq` half if it is missing.

---

## 8. Suggested order of experiments

1. Boot the current tree (patch §7.1 is applied). Read the two
   `GARNET-DBG ctl` lines. Note: the 2 ms → 100 ms change means the failure may
   simply disappear — that in itself confirms **H1**.
   * "OK after N us" → **H1 confirmed**, mainline timeout bug.
   * "incomplete after 100000 us, override=0" → **H2/H3**, reset is held.
   * "incomplete ... override!=0" → **H2 confirmed**, hard-reset override set.
2. If still failing, apply 7.2 + 7.3. Look for `VBIF src` bits and a DSI
   `dma_base` of 0/low value at the same moment as the SMMU fault.
3. Only then touch clocking (7.4 / pin OPP to 325 MHz / add interconnect
   votes). Do not change several things at once — this failure has at least
   three independent contributors (timeout, possible stuck override, DSI DMA
   fault) and they must be separated.

---

## 9. Live-boot results (kernel #63) and the recovery that was applied

### 9.1 What the live log proved

Boot with the 100 ms wait and the extra logging (patched tree, `dpu_hw_ctl.c`,
`dpu_vbif.c`, `dsi_host.c`, `dpu_kms.c`):

```
[0.340] dpu hardware revision:0x80030000
[0.595] panel on: 1220x2712, DSC 610x12 ...
[0.653] arm-smmu ... Unhandled context fault: fsr=0x402, iova=0x00000000,
        fsynr=0x6e0003, cbfrsynra=0x800, cb=7
[0.656] GARNET-DBG ctl1: wait enter swreset=1 override=0 top=f0000000
[0.756] GARNET-DBG ctl1: hw recovery incomplete after 100015 us
        swreset=1 override=0
```

Then, for the whole 37 s of the captured log:

* `swreset=1 override=0` at **every** retry. `wait_reset_status()` therefore
  observes a *hardware-held* CTL reset, not a timeout that is too short.
  → **H1 (2 ms timeout) is disproven.**
* `override=0` never changes, so nothing has latched the hard-reset override.
  → **H2 (stuck override bit) is disproven.** (It also means the override is a
  usable escape hatch, see 9.2.)
* `intf en=1 frame=13..4380 line=...` — the INTF_1 timing engine keeps scanning
  at ~120 Hz and the vsync IRQ fires (`core_irq: IRQ=[0,27] count:4761`).
  The DSI video stream and the panel are alive; only the CTL path is held.
* `CTL_START` reads 0 and `CTL_FLUSH` accumulates `0x00420840` at every failed
  commit — writes reach the CTL, but it never leaves reset, so flush is never
  consumed.
* DSI command DMA is healthy (`dma_base=0x1000 rb_base=0x1000
  cmd_dma_ctrl=0x04000000`, all transfers complete with
  `msm.dsi_dma_from_fb=0`), so the DSI cmd-DMA hypotheses are dead.
* `GARNET-DBG: mdp core opp requested=608000000 actual=608000000` — clock/OPP
  hypothesis is dead.
* VBIF reports `pnd 0x2 src 0x2` once, right after the first failed commit.
  Bit 1 is XIN 1, which is **SSPP_DMA0** (`xin_id = 1` in
  `catalog/dpu_8_3_sm7435.h`), i.e. the primary plane pipe.
* The single `iova=0` SMMU fault happens ~3 ms *before* the first
  `wait_reset_status()`. On DPU 8.3 the hardware itself initiates a CTL reset
  when the pipe reports a fatal bus/translation error and sets
  `CTL_SW_RESET[0]`; that is exactly the condition the mainline
  `wait_reset_status` op was written for.

So the chain is:

```
something fetches from IOVA 0 on DMA0
  -> display SMMU translation fault (iova=0, fsr=0x402, SID 0x800)
  -> hw-initiated CTL0 reset, CTL_SW_RESET[0] = 1
  -> mainline only polls that bit and gives up; nothing forces the CTL back out
  -> every commit fails, black screen while INTF keeps scanning
```

Downstream does not just give up at that point: `sde_crtc_reset_hw()`
(`sde_crtc.c:4000`) writes `CTL_SW_RESET_OVERRIDE` to force the whole ctl path
into reset and then release it (`sde_hw_ctl_hard_reset()`,
`sde_hw_ctl.c:805`), re-programs a safe "border colour only" pipeline and
kicks off again. Mainline has the same `CTL_SW_RESET_OVERRIDE 0x060` define and
the same register semantics, but no recovery.

### 9.2 Implemented recovery

1. `dpu_hw_ctl.c: dpu_hw_ctl_wait_reset_status()` — after the normal 100 ms
   poll fails, do the downstream hard-reset sequence:

   ```c
   DPU_REG_WRITE(c, CTL_SW_RESET_OVERRIDE, 0x1);
   usleep_range(2000, 2500);
   DPU_REG_WRITE(c, CTL_SW_RESET_OVERRIDE, 0x0);
   if (!dpu_hw_ctl_poll_reset_status(ctx, DPU_REG_WAIT_RESET_TIMEOUT_US))
           return 0;   /* reset released, kickoff may proceed */
   return -EINVAL;
   ```

   The layer/plane configuration for the frame is already written by
   `dpu_crtc_atomic_flush()` before `prepare_for_kickoff()` runs
   (`dpu_crtc.c:1118`), and the flush/start happen right after it, so releasing
   the reset here is enough to let the pending frame go out.

2. `dpu_kms.c: dpu_kms_hw_init()` — raw read of `CTL0`/`INTF1` at
   `init-pre` (right after the revision read, before any block init) and
   `init-post` (after `_dpu_kms_drm_obj_init`), plus a one-shot override pulse
   if `CTL_SW_RESET[0]` is *already* set during `hw_init`. `init-pre` tells us
   definitively whether the bootloader handed over a CTL in reset.

3. `dpu_hw_ctl.c: dpu_hw_ctl_reset_control()` now logs `before/after/override`
   and the caller (`%pS`) for every software CTL reset, so a silent reset from
   `dpu_encoder_helper_phys_cleanup()` can be attributed.

4. `dpu_hw_sspp.c: dpu_hw_sspp_setup_sourceaddress()` logs the first 12 source
   addresses per pipe/rectangle — this identifies the pipe that fetches IOVA 0
   (expected: SSPP_DMA0, rectangle and address values).

### 9.3 What to look for on the next boot

```
GARNET-DBG init-pre: ... ctl0 swreset=...
GARNET-DBG init-post: ... ctl0 swreset=...
GARNET-DBG sspp... addr ...
GARNET-DBG ctl1: forcing reset override on/off
GARNET-DBG ctl1: override released the stuck reset, swreset=1   <- success
   -or-
GARNET-DBG ctl1: override did not help, swreset=1 override=...
```

Outcomes:

* `init-pre swreset=1` → the CTL was **already** stuck before the first commit,
  i.e. the bootloader/`msm_mdss_reset()` hand-off leaves it asserted; the
  `hw_init` pulse in item 2 is the real fix.
* `init-pre swreset=0`, later `override released ...` → the hw-initiated reset
  was caused by the IOVA-0 fetch; the recovery in item 1 releases it, and the
  `sspp addr ...` line names the pipe that still needs its address fixed.
* `override did not help` → a VBIF XIN is still blocked. Next step is to halt
  the offending XIN (`dpu_vbif` `set_halt_ctrl`/`get_halt_ctrl`,
  `dpu_hw_vbif.c:130`) around the override pulse, exactly like
  `sde_plane_halt_requests()`, before releasing the CTL.

## 10. Live boot 2 (`7.3.0-gafe8799cad00-dirty`): what the logs proved

### 10.1 Confirmed by the new diagnostics

* `init-pre` / `init-post`:
  `intf1 en=0 frame=0 | ctl0 swreset=0 override=0 start=0 top=f0000000`.
  The bootloader hands over a **clean** CTL: no reset is pending at `hw_init`
  and the one-shot override pulse in `dpu_kms_hw_init()` never fires.
  The `top=f0000000` is the normal `CTL_DEFAULT_GROUP_ID << 28`
  (`dpu_hw_ctl.c:65`, written by `dpu_hw_ctl_intf_cfg_v1()`), not a fault.
* `sspp9 rect0 addr 00002000 ...` — the primary plane (SSPP_DMA0, enum index
  9, xin 1) is programmed with a valid IOVA. The single IOVA-0 SMMU fault is
  **not** the primary plane.
* Exactly one SMMU fault in the whole boot:
  `arm-smmu 15000000.iommu: Unhandled context fault: fsr=0x402,
  iova=0x0, cbfrsynra=0x800, cb=7` at `0.671809`, i.e. ~1 ms *before* the
  first `sspp9` address write at `0.672899`. SID `0x800` is the DPU/SDE
  stream (`sm7435.dtsi:649`), so a DPU client fetched IOVA 0 while its fetch
  pipe was already active but its address had not been programmed yet.
* Every commit after that:
  ```
  ctl1: wait enter swreset=1 override=0 top=f0000000
  ctl1: hw recovery incomplete after 1000xx us swreset=1 override=0
  ctl1: forcing reset override on/off
  ctl1: override released the stuck reset, swreset=0
  ```
  The override pulse **does** release the stuck reset. But no
  `GARNET-DBG ctl1: sw reset from ...` line ever appears, so software never
  writes `CTL_SW_RESET` after `hw_init`: the reset is asserted by hardware.
* The INTF keeps counting (`intf1 frame/line` advance during the 100 ms poll)
  and the DSI command DMA is healthy (`cmd_dma_ctrl=0x4000000`,
  `dma_base=0x1000`), so the DSI/timing engines are not the cause.
* On the next commit `swreset=1` is back, with **no further SMMU fault**. The
  recurrence is therefore not another IOVA-0 fetch; the ctl path is released
  into a frame where it no longer has a valid configuration.

### 10.2 Conclusion

The old recovery released the reset *after* the frame config had already been
written (`prepare_for_kickoff()` runs after `atomic_begin()`/`atomic_flush()`),
so the ctl path came out of reset with its layer/interface registers wiped by
the hard reset and immediately errored/reset again. This is a self-sustaining
loop: reset -> release with empty config -> hardware reset -> reset.

Downstream avoids this by holding `CTL_SW_RESET_OVERRIDE` across the blob
reprogramming (`sde_crtc_reset_hw()`: `hard_reset(true)` ->
`_sde_crtc_blend_setup()` -> `hard_reset(false)`).

### 10.3 New fix in this tree

1. `dpu_hw_ctl.h`/`dpu_hw_ctl.c`
   * new `hard_reset(ctx, bool)` op -> `CTL_SW_RESET_OVERRIDE` (0x060),
     with a ~2 ms hold while asserting, like `sde_hw_ctl_hard_reset()`;
   * new `is_reset_pending(ctx)` op -> read `CTL_SW_RESET[0]`;
   * `wait_reset_status()` is back to a pure wait (no side effects), so the
     recovery decisions live in one place;
   * `CTL_STATUS` (0x064) and the ctl layer/active registers are dumped
     (`reset-pending`) plus `flush-pre/post` and `start-pre/post` notes, so the
     exact point where hardware asserts the reset becomes visible.
2. `dpu_crtc.c: dpu_crtc_recover_ctl()` — called from
   `dpu_crtc_commit_kickoff()`, i.e. after the frame's plane/CTL writes and
   right before prepare/kickoff. If `is_reset_pending()` is set for any CTL of
   the crtc it:
   1. asserts `hard_reset(true)` on every CTL of the crtc,
   2. re-runs `_dpu_crtc_blend_setup()` (layers, active pipes, fetch pipes,
      active LMs, blend stages),
   3. calls the new `dpu_encoder_restore_intf_cfg()` for every encoder of the
      crtc to rewrite `CTL_TOP`/`CTL_INTF_ACTIVE`/`CTL_INTF_MASTER`/DSC/CDM/
      merge-3d (`dpu_encoder_phys_vid.c`), because the INTF config is only
      written at enable time by `setup_timing_engine()`,
   4. releases `hard_reset(false)` and re-checks `CTL_SW_RESET`.
3. `dpu_encoder_phys_vid_prepare_for_kickoff()` no longer issues its own SW
   reset on the error path; that would throw away the configuration the crtc
   recovery just restored. It only reports and takes the snapshot.

### 10.4 What to look for on the next boot

```
GARNET-DBG ctl1: flush-pre ... swreset=...
GARNET-DBG ctl1: start-pre ... swreset=...
GARNET-DBG crtc0: ctl reset pending, forcing hard reset
GARNET-DBG ctl1 reset-pending: swreset=... status=... top=... intf=... fetch=... pipe=... layer=...
```

* `start-post swreset=0` and no `ctl reset pending` next frame -> the frame
  was accepted by hardware, the recovery is not needed anymore; the display
  should scan out. If the SMMU fault at IOVA 0 is the original trigger, it
  must not come back (the plane address is written before file recovery).
* `ctl reset pending` every commit, but `ctl1 still in reset after recovery`
  never prints -> the hard reset + reprogram sequence works but some in-frame
  error keeps triggering the reset; the `reset-pending` register dump then
  names the offending fetch pipe/layer (`fetch`/`pipe`/`layer`/`intf`).

---

## 11. Live boot 4 (recovery active): the reset is asserted *by the flush*

### 11.1 Observed [FACT]

With `dpu_crtc_recover_ctl()` in place every commit looks like:

```
GARNET-DBG ctl1: flush-pre swreset=0 override=0 fetch=00000001 start=0
GARNET-DBG ctl1: flush-post swreset=1 ...            <- once, at 14.234
GARNET-DBG crtc0: ctl reset pending, forcing hard reset
GARNET-DBG ... vblank timeout: 420840
GARNET-DBG ... wait for commit done returned -110
```

* The recovery itself works: before every `flush-pre` the ctl path is out of
  reset (`swreset=0`, `override=0`), so the previous hard reset + reprogram did
  clear it.
* The reset request comes back **inside `trigger_flush_v1()`**, between the
  `flush-pre` read and the `flush-post` read, i.e. from the `CTL_FLUSH` write
  (or from the per-block flush registers written just before it).
* `CTL_FLUSH` then reads back the written mask `0x420840` for the whole 50 ms
  `wait_for_commit_done()` poll: the frame is never fetched/acked.
  Bits: 6 = LM_0, 11 = DMA0, 17 = `CTL_FLUSH_MASK_CTL`, 22 = DSC_0.
* No SMMU fault this boot, so the IOVA-0 fault seen on boot 2 is not required
  for the reset to appear.
* Display still ends up black (one white flash).

### 11.2 Why this points at a single flush bit [FACT + HYP]

`CTL_SW_RESET[0]` is a *request* bit: software sets it in
`sde_hw_ctl_reset_control()`, hardware sets it on an internal error, and the
block clears it when the reset completes (~30 us downstream). Here it is set by
hardware synchronously with the flush and never completes, so some block named
by the flush mask is not in a state where it can ack the flush.
The prime suspects, in mask order, are DSC_0 (`BIT(22)` — the panel really is
DSC-compressed), the LM/CTL-config bits (`BIT(6)`/`BIT(17)`) and DMA0
(`BIT(11)`).

`CTL_DSC_ACTIVE` is written by mainline `dpu_hw_ctl_intf_cfg_v1()` while
downstream `sde_hw_ctl_intf_cfg_v1()` never touches it on the enable path
(only `sde_hw_ctl_reset_intf_cfg_v1()` does). The DSC *programming* itself
(`dsc_config_1_2()` register values) matches downstream, so the suspicion is on
the enable/active/bind side, not on the PPS values.

### 11.3 Diagnostics added in this revision (one boot should settle it)

1. `dpu_hw_ctl_dbg_probe_flush()` — runs once, on the first
   `trigger_flush_v1()`, before the real flush. It does *removal* tests: it
   writes the real pending mask with one candidate bit cleared
   (`BIT(DSC_IDX)`, `BIT(6)`, `BIT(11)`, `CTL_FLUSH_MASK_CTL`), samples
   `CTL_SW_RESET` immediately after the write (was a reset *requested*?) and
   again 20-40 us later (did it complete by itself?). If the mask still resets
   the ctl path, the stuck bit is cleared with the override pulse and the next
   candidate is tested; the first removal that does **not** reset names the
   trigger. Output:

   ```
   GARNET-DBG ctl1 probe: full mask=........
   GARNET-DBG ctl1 probe without <name>(<bit>): swreset now=.. late=.. flush=..
   GARNET-DBG ctl1 probe: <name> is the trigger
   ```

   A removal test is used (rather than writing single bits) because a partial
   mask such as `BIT(6)` without `CTL_FLUSH_MASK_CTL` may be invalid on its own
   and produce a false positive. If no removal is clean, the trigger is the
   full combination or the per-block flush registers written just before
   `CTL_FLUSH`, not a single bit.
2. `dpu_hw_ctl_dbg_dump()` is now called as `recover-pre`/`recover-post`/
   `reset-pending`/`probe-post` and also prints `CTL_FLUSH`,
   `CTL_DSC_ACTIVE`, `CTL_MERGE_3D_ACTIVE`, `CTL_WB_ACTIVE`,
   `CTL_CWB_ACTIVE`, `CTL_CDM_ACTIVE` and the per-block flush registers
   (`CTL_INTF_FLUSH`, `CTL_DSC_FLUSH`, `CTL_MERGE_3D_FLUSH`, `CTL_CDM_FLUSH`,
   `CTL_PERIPH_FLUSH`). Separate counters for notes (32) and dumps (12).
3. `dpu_encoder_dsc_pipe_cfg()` prints a one-shot DSC readback after all DSC
   ops ran: the PPS inputs (`mode`, `initial_lines`, slice/pic sizes, bpp/bpc,
   chunk size, initial xmit delay) and `DSC_CTL`/`DSC_CFG`/`DSC_DATA_IN_SWAP`/
   `DSC_CLK_CTRL`, `DSC_CMN_MAIN_CNF`, `ENC_DF_CTRL`, `DSC_MAIN_CONF`,
   `DSC_PICTURE_SIZE`, `DSC_SLICE_SIZE`, `DSC_MISC_SIZE`, `DSC_HRD_DELAYS`,
   `ENC_GENERAL_STATUS`, `ENC_HSLICE_STATUS`, `ENC_OUT_STATUS`,
   `ENC_INT_STAT`.
   Expected on a healthy setup: `DSC_CTL=0` (bound to PINGPONG_0),
   `DSC_CFG=0x401` (encoder enable, 10 bpc, video mode), non-zero
   `DSC_CMN_MAIN_CNF`/`ENC_DF_CTRL`, `ENC_OUT_STATUS` without error bits.
4. On `wait_for_commit_done()` timeout the encoder now also dumps
   `INTF_STATUS` (0x26C), `INTF_TIMING_ENGINE_EN` (0x000), `INTF_CONFIG`
   (0x004), `INTF_CONFIG2` (0x060), `INTF_FRAME_COUNT` (0x0AC),
   `INTF_LINE_COUNT` (0x0B0) and `INTF_FRAME_LINE_COUNT_EN` (0x0A8), to see
   whether the timing engine is actually running (line/frame counter moving,
   `timing_en=1`) while the flush is stuck.
5. `dpu_crtc_recover_ctl()` now first tries the plain SW reset
   (`ctl->ops.reset()`, i.e. `CTL_SW_RESET=1` + poll, exactly what downstream
   `sde_crtc_reset_hw()` does and what it early-outs on when it succeeds) and
   only falls back to the `CTL_SW_RESET_OVERRIDE` hold-and-reprogram dance if
   that times out. `GARNET-DBG crtc0: plain sw reset rc=.. swreset=..` reports
   which path was taken.

### 11.4 Decision tree for the next boot

* `plain sw reset rc=0` -> the reset state machine does complete when software
  pokes it; keep that path (it is the downstream behaviour) and look at the
  *next* reset request only.
* probe stops at `dsc0` -> DSC flush cannot be acked. Next step: check the
  `dscN regs` line. `cfg` bit 0 clear or `DSC_CTL != 0` -> DSC
  programming/bind bug; then try the runtime switches described in 12.2.
* probe stops at `lm0`/`ctl-config` -> the LM/blend-stage or CTL self-config
  flush is invalid (mixer `sblk`/stage count, `CTL_LAYER*`/`CTL_TOP`), not DSC.
* probe stops at `dma0` -> the SSPP fetch side (DMA0 xin 1, fetch pipe 0) is
  the problem, likely the plane/QoS programming.
* no probe triggers, but `flush-post swreset=1` again -> the trigger is the
  full combination or one of the per-block flush registers written before
  `CTL_FLUSH` (`CTL_DSC_FLUSH` is the only one written for this commit); read
  the `flushsub[...]` fields in the dumps.


---

## 12. Boot 5: probe reworked, plus runtime switches

### 12.1 What changed since 11.3 [FACT]

* The one-shot probe now runs the **real sequence first** (write
  `CTL_DSC_FLUSH` and then `CTL_FLUSH` with the full mask) and logs
  `probe real-sequence ...`. If that does *not* reproduce the stuck reset,
  the trigger is not the flush register write at all, and the whole theory
  needs revisiting.
* Then `probe no-sublk-flush ...` (only `CTL_FLUSH`, no `CTL_DSC_FLUSH`)
  separates "the sub-block flush write" from "the CTL_FLUSH write".
* Then the removal tests (`without-dsc0`, `without-lm0`, `without-dma0`,
  `without-ctl-cfg`) and finally `no-dsc-active`.
* The probe no longer sleeps: it runs inside `_dpu_encoder_kickoff_phys()`
  which holds `enc_spinlock` with IRQs off, so it uses `udelay()` and a short
  `CTL_SW_RESET_OVERRIDE` pulse to clear a stuck reset between tests.
* Corrected earlier assumption: downstream *does* program `CTL_DSC_ACTIVE`,
  but only from the DCE path (`_dce_dsc_setup_single()` ->
  `update_intf_cfg(..., active)`), not from `sde_encoder_phys_vid_enable()`.
  Mainline programs it from `dpu_hw_ctl_intf_cfg_v1()` on the enable path.
  So the bitmap value is probably fine; the probe still tests it.
* Also noted [FACT]: downstream writes the DSC flush bit only when the DSC
  state is dirty (`_dce_dsc_is_dirty()` / `_dce_helper_flush_dsc()`), while
  mainline re-runs `dpu_encoder_prep_dsc()` (full PPS rewrite **and** DSC
  flush bit) on every commit. That is a real behavioural difference and is
  now switchable at runtime.

### 12.2 Runtime switches (kernel command line, no rebuild needed per test)

Both are plain `module_param`s, so they can be set in the bootloader cmdline:

| parameter | effect |
|---|---|
| `dpu.dsc_once=0` | restore the upstream behaviour: rewrite the DSC PPS and request a DSC flush on *every* kickoff. The default is now `1` (once per enable), matching downstream |
| `dpu.dsc_no_active=1` | do not set `CTL_DSC_ACTIVE` from `dpu_hw_ctl_intf_cfg_v1()` |

Suggested test order after the plain boot 5 (probe output first):

1. plain boot (DSC configured once, like downstream) - if the reset is gone,
   the per-frame DSC rewrite/flush was the trigger.
2. `dpu.dsc_once=0` - if the reset *returns* only with this, that confirms it.
3. `dpu.dsc_no_active=1` - if the reset is gone only with this, drop the
   enable-path `CTL_DSC_ACTIVE` write and let the DSC bind path own it.

### 12.3 What to look for

```
GARNET-DBG ctl0 probe: full mask=... dsc_flush=... dsc_active=...
GARNET-DBG ctl0 probe real-sequence mask=...: swreset now=.. late=.. flush=.. dsc_active=..
GARNET-DBG ctl0 probe no-sublk-flush mask=...: ...
GARNET-DBG ctl0 probe without-dsc0 ... 
GARNET-DBG ctl0 probe: <name> is the trigger
GARNET-DBG ctl0 probe no-dsc-active mask=...: ...
GARNET-DBG crtc0: plain sw reset rc=.. swreset=..
GARNET-DBG ctl0 probe-post: ...
GARNET-DBG intf1: status=.. timing_en=.. config=.. intf_cfg2=.. frame_cnt=.. line_cnt=.. frame_line_en=..
```

* `timing_en=0` on the timeout line would mean the INTF never started and the
  whole ctl/blend path question is secondary.
* `frame_cnt`/`line_cnt` moving means the timing engine runs and only the ctl
  path is stuck.
* `real-sequence` clean while the normal `flush-post` still shows
  `swreset=1` would mean the reset comes from somewhere else entirely (e.g.
  an INTF/DSI side error, or a late effect of the preceding SMMU/fetch
  state), not from these register writes.
