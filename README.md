# Samsung Galaxy S10+ (Exynos 9820 / beyond2lte) Mainline Linux 7.2 Kernel for Android 16

Booting an unmodified Android 16 custom ROM (Project Infinity X) on the Samsung Galaxy S10+ (SM-G975F, Exynos 9820, `beyond2lte`) using a 100% mainline Linux 7.2-rc2 kernel, open-source Mesa Panfrost / PanVK Mali-G76 GPU drivers, zero-copy Exynos 9820 DECON KMS, and full Broadcom BCM4375 Wi-Fi 6.

## Highlights

- **Kernel:** Mainline Linux 7.2-rc2 (`aarch64`, built with LLVM/Clang) + **KernelSU Next v3.4.2** (UAPI 5, working manager + root) + `CONFIG_DM_SNAPSHOT=y` (DSU support), packaged with `uniLoader` into a standard Samsung `boot.img`
- **Android 16 ROM:** Boots the real encrypted `/data` partition (`fscrypt` + `dm-default-key` + TEEGRIS `tzdev` + UFS `scsi_srpmb`) without modifying `/system` or `/vendor` permanently (all Mesa/HWC/firmware overrides live in a read-only RAM `overlayfs` from `/cache/mainline-gpu`)
- **Display (Zero-Copy 60 Hz KMS + Brightness):** Native `exynos9820-decon-kms` handover driver (`1440x3040 @ 60 Hz`) scanning out directly through `SysMMU_DPUD0` with hardware TE vblank interrupts (`GIC SPI 183`), PSR, hardware R/B channel swap in GF0 IDMA, and S6E3HA9 DSIM0 brightness (`0xB1` AOR) + `DISPLAY_OFF`/`DISPLAY_ON` DPMS
- **Audio (Stereo Speakers):** Firmware-less Exynos 9820 `ABOX` (`exynos9820-abox`), Cirrus Logic Madera `CS47L93` codec, dual `CS35L40` speaker amplifiers (`S32_LE` TDM slot width fix), `pd_aud` TZPC SMC restore, `CLKOUT0` 26 MHz MCLK, and a minimal open-source Android Primary Audio HAL (`android/audio_hw_exynos9820.c`)
- **Hardware Video Codec (MFC v13):** Mainline `s5p-mfc` v13 (`188d0000.video-codec`) with `CMU_MFC` runtime PM, dual `sysmmu_mfc0`/`sysmmu_mfc1` IOMMUs, and `pd_mfc` TZPC SMC restore, exposing `/dev/video0` (`s5p-mfc-dec`) and `/dev/video1` (`s5p-mfc-enc`) using stock `/vendor/firmware/mfc_fw.bin`
- **Ultrasonic In-Display Fingerprint (`qbt2000`):** Qualcomm QBT2000 driver ported to Linux 7.2 (`drivers/fingerprint/`) with Secure-World `SPI1` (`PERIC0 USI00`) clocks, TEEgris (`tzdev`) trustlet integration, and `s6sy761` touchscreen FOD trigger bridge (full enrollment and unlock working)
- **Haptics, Power & Thermal:** Custom open-source AIDL v2 Vibrator HAL (`android/vibrator_mainline.c`) driving `max77693-haptic` (`EV_FF` / `FF_RUMBLE`), plus mainline `powerhint.json` (CPU + Panfrost devfreq boost) and `thermal_info_config.json` (6 live thermal zones)
- **GPU (OpenGL ES 3.1 + Vulkan):**
  - Non-coherent Exynos 9820 Mali-G76 Panfrost kernel support (AArch64 stage-1 page tables + `GPU_COHERENCY_ENABLE` non-coherent programming after reset, 0 GPU faults)
  - Full vendor Mali-G76 DVFS scaling (`156 MHz` to `702 MHz`) via ACPM IPC
  - Mesa 26.3.1 Panfrost OpenGL ES 3.1 + **PanVK Vulkan 1.3** (`vulkan.mali.so`) + `drm_hwcomposer` (HWC3) + `minigbm`
- **Power Management (Full Android S2idle):** `WFI-s2idle` cpuidle state (`cpuidle-psci`), `genpd` runtime-suspend resume guard, `madera` system sleep PM ops, `pinctrl` AUD/NOIRQ fixes, `pci-exynos` s2idle keep-link, `s6sy761` resume `SENSE_ON`, and `brcmfmac` `mbdata_in_ring` + OOB `host_wake` (`gpa0:7`) + `PM_MAX`
- **Bluetooth (`BCM4375B1`):** Mainline `hci_bcm` over `PERIC1` `usi_uart_bt` (`serial1`, `3 Mbps`) loading Murata B1 `BCM4362A2.hcd` + lightweight open-source `HCI_CHANNEL_USER` `libbt-vendor.so` bridge (`android/bt_vendor_mainline.c`) for Android 16
- **Sensors (`BCM4775` Sensor Hub):** Mainline `bcm4775-shub` IIO driver over `PERIC1` `spi17` (`USI16`) + standalone open-source Android AIDL v3 `ISensors` HAL (`android/sensors_mainline.cpp`) providing Accelerometer (Auto-Rotate), Ambient Light (Auto-Brightness), Proximity, and Compass Heading
- **Wi-Fi 6 (802.11ax):** Broadcom BCM4375 (`14e4:4475`) over Exynos 9820 PCIe Gen1 x1 + DWC MSI + `brcmfmac` (rev7 IPC `host_cap` negotiation, `ringupd` skip, D2H XOR sync, DMA barriers, 128-byte random seed)
- **CPU & RAM:** All 8 CPU cores online (4x Cortex-A55 @ 1.95 GHz, 2x Cortex-A75 @ 2.31 GHz, 2x Exynos M4 @ 2.53 GHz) and full 8 GB RAM (`7.4 GB` usable) with Samsung `debug_snapshot` and `ect` memory carveouts properly reserved
- **Touch & USB:** Samsung `s6sy761` touchscreen (batched `input_sync()` + reliable I2C probe retry) and `max77705` MUIC auto-USB switching (automatic ADB without cable replug)

## Current Status

| Subsystem | Status | Notes |
|---|---|---|
| Boot / Encrypted `/data` | Working | `tzdev` + `dm-default-key` + `fscrypt` + `scsi_srpmb` |
| Display (`exynos9820-decon-kms`) | Working | Zero-copy 1440x3040 60 Hz hardware TE VSYNC |
| GPU (Panfrost GLES 3.1 + PanVK 1.3) | Working | Mesa 26.3.1, 156 to 702 MHz DVFS, 0 GPU faults |
| Wi-Fi 6 (`BCM4375` / `brcmfmac`) | Working | 5 GHz 802.11ax, DHCP, NTP clock sync |
| Touchscreen (`s6sy761`) | Working | Smooth multitouch, reliable boot probe |
| All 8 CPU Cores + 8 GB RAM | Working | Exynos M4 cores active, reserved memory fixed |
| Auto-ADB (`max77705` MUIC) | Working | Enumerates on warm boot without replug |
| Display Brightness & Screen Off (S6E3HA9) | Working | DSIM0 `0xB1` AOR dimming + `0x28`/`0x29` DPMS |
| Stereo Speaker Audio (`ABOX` + `CS35L40`) | Working | `hw:0,1` 48 kHz stereo `S32_LE` + custom Android Audio HAL |
| Hardware Video Codec (`s5p-mfc` v13) | Kernel Working | `/dev/video0` & `/dev/video1` live with `mfc_fw.bin`; V4L2 Codec2 HAL TODO for Android apps |
| Ultrasonic Fingerprint (`qbt2000`) | Working | Full enrollment & unlock via TEEgris + `s6sy761` FOD bridge |
| Vibrator / Haptics (`max77693-haptic`) | Working | Custom AIDL v2 `IVibrator` HAL (`EV_FF` / `FF_RUMBLE`) |
| Power & Thermal HALs | Working | Mainline CPU/Panfrost `powerhint.json` + 6 live thermal zones |
| S2idle Sleep (`WFI-s2idle` + Wi-Fi `host_wake`) | Working | Full Android suspend/resume with Wi-Fi `PM_MAX`, `gpa0:7` wake, and touch resume |
| Bluetooth (`BCM4375B1`) | Working | `hci_bcm` `3 Mbps` + custom `HCI_CHANNEL_USER` `libbt-vendor.so` bridge |
| Sensors (`BCM4775` IIO Hub) | Working | Accelerometer (Auto-Rotate), Light, Proximity, Compass via custom AIDL v3 `ISensors` HAL |
| KernelSU Next (`v3.4.2`) + DSU | Working | UAPI 5 + `__sys_setresuid` hook + `CONFIG_DM_SNAPSHOT=y` |
| Camera, RIL, S2R | TODO | Planned next milestones |

## How It Works (Why `/system` and `/vendor` Stay Untouched)

Stock Samsung Android 16 ROMs ship with proprietary Mali Bifrost (`libGLES_mali.so`), Exynos HWC, and vendor kernel expectations. Instead of rebuilding the entire ROM, this port:

1. Boots mainline Linux 7.2-rc2 from `/dev/block/by-name/boot` via `uniLoader`.
2. Mounts a small RAM `overlayfs` over `/vendor` from `/cache/mainline-gpu` containing open-source Mesa 26.3.1 (Panfrost GLES + PanVK Vulkan 1.3), `drm_hwcomposer` (HWC3), `minigbm` gralloc, custom Primary Audio & Vibrator HALs, and Broadcom `brcmfmac` firmware symlinks.
3. Keeps `/system`, `/vendor`, and `/data` intact so you can switch back to a stock/LineageOS boot image at any time.

## Repository Layout

- `linux/`: Mainline Linux 7.2-rc2 kernel tree with Exynos 9820 Android bring-up patches
- `graphics-stack/`: Mesa 25.0.7 (Panfrost + PanVK), `drm_hwcomposer`, `minigbm`, and Android cross-build configs
- `tools/`: Boot image packaging scripts (`package_boot.py`)
- `NOTES/`: Detailed bring-up logs and root-cause documentation

## Credits

Built on top of the Exynos 9820 mainline Linux community work:

- [chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux) (Exynos 9820 mainline kernel base)
- [tannerbruhn/exynos-9820-mainline-linux](https://github.com/tannerbruhn/exynos-9820-mainline-linux) (BCM4375 `brcmfmac` PCIe IPC fixes, GPU OPP table, and reserved memory findings)
- [omnipha/linux-samsung-s10-exynos9820](https://github.com/omnipha/linux-samsung-s10-exynos9820) (Gregor Sprint's non-coherent Exynos 9820 Mali-G76 Panfrost patch and `debug_snapshot`/`ect` memory reservations)
- [pulpoff/linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte) (`exynos9820-decon-kms` zero-copy handover KMS and `SysMMU_DPUD0` DT version support)
- Mesa Panfrost / PanVK, `drm_hwcomposer`, `minigbm`, `uniLoader`, and KernelSU Next

## Keywords

Samsung Galaxy S10+, SM-G975F, beyond2lte, Exynos 9820, mainline Linux 7.2, Android 16, Project Infinity X, Panfrost, PanVK, Vulkan, Mali-G76, Mesa 25, drm_hwcomposer, minigbm, exynos9820-decon-kms, BCM4375, brcmfmac, Wi-Fi 6, Exynos M4, TEEGRIS, tzdev, dm-default-key, KernelSU Next, uniLoader, open source Android kernel.
