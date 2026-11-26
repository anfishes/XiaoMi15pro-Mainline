# Xiaomi 15 Pro (`haotian`) — Mainline Linux Bring-up
！！！含有vibe内容，过敏谨慎食用。
Mainline Linux (`7.2.10` based on `linux-sm8750-piano`) + Denial / Phonewave Wayland bring-up for the **Xiaomi 15 Pro** (`haotian`, model `2410DPN6CC`, Qualcomm Snapdragon 8 Elite / SM8750P).

> **Disclaimer & Firmware Notice**
> - This repository contains **only open-source kernel patches, device trees, AudioReach topology/UCM2, and helper scripts**.
> - **No proprietary Qualcomm or Xiaomi firmware binaries** are included. Extract device-signed firmware read-only from your own phone using `scripts/extract-firmware.sh`.
> - Flashing custom boot images carries risk of data loss or bricking. Always back up your stock `boot_a` first, and **never write to `super`, `persist`, or UFS RPMB**.

---

## Hardware Status

| Subsystem | Hardware / Driver | Status | Notes |
| --- | --- | --- | --- |
| **Display** | Xiaomi O2 1440×3200 @ 120Hz 10bpc DSC 1.2 (`panel-xiaomi-o2`, DPU CMD mode) | Working | Native KMS, backlight, sleep/wake, IC-SPR RGB 4:4:4 accurate color, boot/shutdown fbcon (`TER16x32`) |
| **GPU** | Adreno 830 (`msm` + `gen80000_zap.mbn`, Freedreno / Impeller GLES 3.2) | Working | Full Wayland hardware acceleration |
| **Touchscreen** | Synaptics S3910 SPI (`synaptics_tcm_spi`) | Working | Real RPMh `vreg_l12b_1p8` regulator |
| **Wi-Fi 7** | WCN7860 / FastConnect 7900 (`ath12k_wifi7`, PCIe0 Gen3 x2) | Working | Built as `=m` (2.4/5 GHz verified) |
| **Bluetooth & HID** | WCN7860 BT on `uart14` (`hci_qca`, `qcom,wcn7860-bt`) + `uhid`/`hidp` | Working | BLE + Classic BR/EDR, BT keyboards verified |
| **Internal Speaker** | Dual Cirrus Logic `cs35l43` on `i2c_hub_0` (`0x40`/`0x42`), `SECONDARY_TDM_RX_0` | Working | `PARAM_ID_TDM_LANE_CFG` (Lane 1) + `MFC` topology + UCM2 (`HiFi.conf` disables bottom `B` amp by default due to hardware fault on bring-up unit) |
| **ADSP / CDSP** | `qcom_q6v5_pas` (`remoteproc0`/`remoteproc1`) + FastRPC + Q6APM | Working | `adspslpi_mem` sized to `0x4580000` |
| **Sensors** | `hexagonrpcd` + `steinerd` | Working | Accel, Gyro, Light, Orientation (`slot_suffix=_a` fallback under SunUEFI) |
| **USB OTG & Gadget** | `dwc3` (`a600000.usb`, dual-role via `pmic_glink`) + `xiaomi_mca` external 5V boost | Working | Host mode (4.99V VBUS, USB mouse/HID verified) + Peripheral CDC Ethernet (`10.77.71.2`) & ACM serial |
| **Battery & Charging** | TI `bq27z561` (`i2c5`) + `xiaomi_mca` + `pmic_glink` UCSI | Working | Blacklist `qcom_battmgr`; udev rule auto-arms `cc_toggle=1` and per-type safe current limits |
| **Cellular / 5G Data** | SM8750 MPSS (`remoteproc0`) + IPA v5.5 (`3f40000.ipa` / `rmnet_ipa0`) + `qwes-bridge` + `dmail` | Working | Live 5G SA/4G data (`rmnet_data0`, QMAP v5); open-source `qwes-bridge` satisfies QTEE `IPFM` license checks (strictly RO RPMB + sandboxed `persist/data` copy) + `haotian-data-watchdog` |
| **Camera (Ultrawide)** | Rear Ultrawide Samsung `S5KJN1` (`i2c8 @ 0x3d`, `CSIPHY2`, `PM8010M` + `BOB2`) via `qcom-camss` | Working | 12.5MP (`4080x3072`) 10-bit RAW Bayer + `braun` camera daemon JPEG capture verified |
| **Buttons & Flash LED** | Power / VolDown / VolUp + `pm8550_flash` (`white:flash-0`, `yellow:flash-1`) | Working | Full key events & sysfs LED class |
| **CPUfreq / Thermal / RTC** | `schedutil`, 58 thermal zones, `/dev/rtc0` | Working | — |
| **Main/Tele/Front Cameras, Internal Mic, Haptics, VoLTE** | `OVX9000` / `IMX858` / `OV32B40`, VA DMIC, `cs40l26@43` | WIP / Untested | No mainline sensor drivers yet for OVX9000/IMX858 |

---

## Repository Layout

```text
xiaomi-15-pro-mainline/
├── LICENSE                                    # GPL-2.0-only
├── README.md
├── patches/
│   ├── 0001-arm64-dts-qcom-add-xiaomi-15-pro-haotian.patch
│   ├── 0002-drm-msm-panel-xiaomi-o2-and-dsc-cmd-mode-fixes.patch
│   ├── 0003-input-touchscreen-synaptics-s3910-tcm-spi.patch
│   ├── 0004-asoc-codecs-add-cirrus-cs35l43-driver.patch
│   ├── 0005-asoc-qcom-sm8750-tdm-lane-cfg-and-haotian-audio.patch
│   ├── 0006-net-ipa-add-sm8750-v5.5-support.patch
│   └── 0007-power-supply-xiaomi-mca-haotian-otg-boost.patch
├── dts/
│   ├── sm8750-xiaomi-haotian.dts              # Standalone copy of the haotian board DTS
│   └── sm8750-xiaomi-haotian-camera.dtsi      # Rear ultrawide S5KJN1 + CAMSS + PM8010M DTSI
├── configs/
│   └── haotian.config                         # Tested kernel .config (Image <= 40 MB + Landlock + IKCONFIG)
├── modem/
│   └── qwes-bridge/
│       ├── qwes_bridge.c                      # Open-source QTEE listener + QRTR 1059 QWES license bridge
│       └── Makefile
├── audio/
│   ├── topology/
│   │   ├── Xiaomi-15-Pro.m4                   # AudioReach TDM playback topology source
│   │   ├── subgraph-device-tdm-2ch.m4         # LOG -> MFC (S16->S32) -> TDM_SINK subgraph
│   │   └── Xiaomi-15-Pro-tplg.bin             # Pre-compiled open-source ALSA topology
│   └── ucm2/
│       ├── conf.d/sm8750/Xiaomi-15-Pro.conf
│       └── Qualcomm/sm8750/Xiaomi-15-Pro/
│           ├── Xiaomi-15-Pro.conf
│           └── HiFi.conf
├── rootfs/
│   ├── etc/
│   │   ├── dmail/device.toml                  # dmail IPA v5.5 / QMAP v5 endpoint mapping
│   │   ├── modprobe.d/haotian-battmgr.conf    # Blacklist qcom_battmgr
│   │   ├── modules-load.d/
│   │   │   ├── haotian-bt-hid.conf            # uhid, hidp, rfcomm, bnep
│   │   │   ├── haotian-droidloom.conf         # configs (/proc/config.gz), system_heap
│   │   │   └── haotian-ipa.conf               # Autoload ipa.ko before modem boot
│   │   ├── systemd/system/
│   │   │   ├── qwes-bridge.service
│   │   │   ├── haotian-modem-init.service
│   │   │   ├── haotian-data-watchdog.service
│   │   │   ├── dmail.service.d/haotian.conf
│   │   │   ├── hexagonrpcd-stock-sensors.service.d/haotian.conf
│   │   │   ├── rmtfs.service.d/haotian-log.conf
│   │   │   └── tqftpserv.service.d/haotian-log.conf
│   │   ├── tmpfiles.d/haotian-drm-quiet.conf  # Silence 120Hz DRM debug spam
│   │   └── udev/rules.d/90-haotian-usb-charge.rules
│   └── usr/local/libexec/
│       ├── haotian-data-watchdog              # 3-stage 5G recovery + 4G fallback watchdog
│       ├── haotian-dds-fix                    # QMI DSD 0x004E switch to Primary DDS (1)
│       ├── haotian-modem-init                 # Ordered IPA -> qwes-bridge -> rmtfs -> remoteproc0 -> DDS
│       ├── haotian-stock-sensors              # SunUEFI slot_suffix=_a sensor helper
│       ├── haotian-tee-sync                   # One-time RO copy of persist/data to /var/lib/haotian-tee/
│       └── haotian-usb-charge                 # Xiaomi MCA cc_toggle & per-charger safe current limits
└── scripts/
    ├── apply-patches.sh                       # Apply all 7 patches to linux-sm8750-piano
    ├── build-boot.sh                          # Package Image + DTB + initrd into boot.img via SunUEFI
    ├── build-kernel.sh                        # Build Image, DTB, modules + enforce 40 MB ceiling
    ├── build-topology.sh                      # Recompile AudioReach topology .bin from .m4
    └── extract-firmware.sh                    # Read-only firmware extractor (run on phone)
```

---

## Key Technical Notes & Pitfalls

1. **ABL / SunUEFI ~40 MB Uncompressed `Image` Ceiling**:
   - If uncompressed `arch/arm64/boot/Image` exceeds ~40 MB (`40,000,000` bytes), ABL/SunUEFI bounces back to fastboot before Linux starts.
   - Keep `ath12k`, `mac80211`, `cfg80211`, Bluetooth HID (`uhid`, `hidp`, `rfcomm`, `bnep`), `ipa`, `xiaomi_mca`, `configs`, `qcom-camss`, `s5kjn1`, and ASoC drivers built as loadable modules (`=m`), as in `configs/haotian.config` (`Image` stays ~39.91 MB).

2. **USB OTG 5V Boost (`0007-power-supply-xiaomi-mca-haotian-otg-boost.patch`)**:
   - Unlike `xiaomi,piano` (internal boost `{0, 1, ~0U, ~0U}`), `xiaomi,haotian` uses an external OTG boost (`{1, 1, 0x0709, 0x070a}` = PMIC SID 7 GPIOs 9 & 10). Patch `0007` selects and whitelists this config for `xiaomi,haotian`, enabling 4.99V VBUS in Host mode alongside dual-role `pmic-glink` switching.

3. **Speaker TDM Lane Selection (`0005`, `PARAM_ID_TDM_LANE_CFG` `0x0800106A`)**:
   - `SECONDARY_TDM_RX_0` playback (`DOUT`) uses **Lane 1 (`gpio124`)** and capture (`DIN`) uses **Lane 0 (`gpio122`)**. Patch `0005` sends `PARAM_ID_TDM_LANE_CFG`, and `subgraph-device-tdm-2ch.m4` inserts `AR_MODULE_MFC` (`S16_LE` -> `S32_LE`).
   - On healthy units, uncomment the bottom speaker (`B`) controls in `audio/ucm2/Qualcomm/sm8750/Xiaomi-15-Pro/HiFi.conf` for full stereo.

4. **SM8750 5G Modem, IPA v5.5 (`0006`), and `qwes-bridge`**:
   - Without QTEE `IPFM (UID 119)` + QRTR Service `1059` (`0x423`), the modem crashes ~2–5 minutes after boot with `Smart Transmit License Check Failure`.
   - `modem/qwes-bridge/qwes_bridge.c` is a standalone open-source C daemon (standard Linux UAPI only) providing strictly **Read-Only UFS RPMB** (`0x2000`, write opcodes hard-blocked) and sandboxed FS/GPFS under `/var/lib/haotian-tee/`.

5. **Rear Ultrawide Camera (`sm8750-xiaomi-haotian-camera.dtsi`)**:
   - Uses `qcom,sm8750-camss`, `&gpi_dma2`, `&i2c8` (`0x3d`), `PM8010M` (`L2M/L4M/L6M/L7M`), and `PM8550 BOB2` (`3.0V–3.296V`, `regulator-always-on`). `vreg_l3i_0p88` is kept `regulator-always-on` so camera power-down never drops UFS QREF.

---

## Reproducible Build & Installation Guide

### Prerequisites (Host)
- Linux host with `clang`, `lld`, `llvm-strip`, `make`, `bc`, `flex`, `bison`, `m4`, `alsa-utils` (`alsatplg`), `fastboot`.
- Base kernel tree: `linux-sm8750-piano` (`piano-dev`, Linux `7.2.10`).
- Boot image wrapper: `Project-SunUEFI` (SM8750 UEFI/BootShim packager).
- Rootfs: Arch Linux ARM / Phonewave + Denial on your Linux root partition.

### 1. Apply Patches & Build Kernel

```bash
./scripts/apply-patches.sh /path/to/linux-sm8750-piano
./scripts/build-kernel.sh /path/to/linux-sm8750-piano
```

Install modules into your phone rootfs:

```bash
make -C /path/to/linux-sm8750-piano ARCH=arm64 LLVM=1 \
  INSTALL_MOD_PATH=/path/to/phone-rootfs INSTALL_MOD_STRIP=1 modules_install
```

Package `Image` + `sm8750-xiaomi-haotian.dtb` + `denial-initrd.cpio.gz` into `boot.img` using `scripts/build-boot.sh` and `Project-SunUEFI`.

### 2. Install Audio, UCM2, Rootfs Drop-ins & `qwes-bridge`

```bash
install -Dm644 audio/topology/Xiaomi-15-Pro-tplg.bin \
  /usr/lib/firmware/qcom/sm8750/Xiaomi-15-Pro-tplg.bin
cp -a audio/ucm2/* /usr/share/alsa/ucm2/
cp -a rootfs/etc/* /etc/
cp -a rootfs/usr/local/libexec/* /usr/local/libexec/
chmod 755 /usr/local/libexec/haotian-*

make -C modem/qwes-bridge
sudo make -C modem/qwes-bridge install

sudo systemctl daemon-reload
sudo systemctl enable qwes-bridge.service haotian-modem-init.service haotian-data-watchdog.service dmail.service
```

### 3. Extract Device-Signed Firmware (On Phone, Read-Only)

```bash
sudo ./scripts/extract-firmware.sh
```

Copy `cs35l43-dsp1-spk-prot.wmfw`, `B-cs35l43-dsp1-spk-prot.bin`, and `T-cs35l43-dsp1-spk-prot.bin` from your stock `odm_a` partition (mounted read-only) into `/usr/lib/firmware/`.

### 4. Flash & Boot

Always back up your stock `boot_a` before flashing:

```bash
fastboot flash boot_a boot-haotian-7.2.10-piano8.img
fastboot reboot
```

---

## Credits, Upstream References & Licensing

### Licensing
- **Kernel patches (`patches/`), `dts/sm8750-xiaomi-haotian-camera.dtsi`, and `configs/haotian.config`**: Licensed under **GPL-2.0-only** (matching the Linux kernel; see [LICENSE](LICENSE)). Original `SPDX-License-Identifier` and author copyright headers are preserved in all patches.
- **Board DTS (`dts/sm8750-xiaomi-haotian.dts`)**: `BSD-3-Clause` (as marked in its SPDX header).
- **Userspace helper code (`modem/qwes-bridge/`, `scripts/`, `rootfs/`)**: Licensed under **GPL-2.0-only** (see [LICENSE](LICENSE)).
- **Binary releases** (if distributed separately): Compiled from `linux-sm8750-piano` (`piano-dev`, Linux `7.2.10`) with `patches/0001..0007` and `configs/haotian.config` applied, satisfying GPL-2.0 corresponding source requirements.

### Upstream Credits & References
- **`linux-sm8750-piano` & `xiaomipad-8pro-mainline`**: SM8750 mainline base kernel (`7.2.10-piano8`), `xiaomi_mca` charging driver base (adapted for `haotian` OTG boost in `0007`), and AudioReach TDM topology reference.
- **`blu-sharky/debian-piano`**: Original SM8750 `camss` / camera device tree reference (`sm8750-xiaomi-piano-camera.dtsi`, commit `fd6266d73f3442b23362260c3aa0c86782e0b52c`).
- **`map220v/sm8550-mainline` (`sheng-7.2`)**: Cirrus Logic `cs35l43` codec driver port base (`0004`).
- **`Project-SunUEFI`**: SM8750 BootShim / UEFI boot wrapper (`SILICIUM_UEFI.fd`) used to package `boot.img` (not redistributed in this source repository).
- **`kernel_xiaomi_sm8750-devicetrees-lineage-22.2`**: Hardware topology reference (GPIOs, regulator rails, sensor addresses, panel timings).
- **`audioreach-topology`**: M4 macros and build framework for compiling `Xiaomi-15-Pro-tplg.bin`.
- **`Denial` / `Phonewave`**: Arch Linux ARM rootfs, Wayland mobile shell, `dmail` modem manager, and `braun` camera daemon (not redistributed in this repository).
