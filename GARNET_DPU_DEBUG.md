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
