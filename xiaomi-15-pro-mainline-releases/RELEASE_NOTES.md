# Xiaomi 15 Pro (`haotian`) Mainline Linux `7.2.10-piano8` Release

Pre-built boot image and out-of-tree/updated kernel modules for **Xiaomi 15 Pro (`haotian`, `2410DPN6CC`)** running mainline Linux `7.2.10-piano8` + Denial / Phonewave Wayland.

## Included Files
- `boot-haotian-7.2.10-piano8.img`: Packaged Android v4 boot image (`Image` + `sm8750-xiaomi-haotian.dtb` + `denial-initrd.cpio.gz` wrapped with Project-SunUEFI `SILICIUM_UEFI.fd`).
- `ipa-7.2.10-piano8.ko`: SM8750 IPA v5.5 driver module (`qcom,sm8750-ipa`).
- `snd-q6apm-7.2.10-piano8.ko`: Q6APM module with `PARAM_ID_TDM_LANE_CFG` (`0x0800106A`).
- `xiaomi-mca-7.2.10-piano8.ko`: Xiaomi MCA charging & `haotian` external 5V OTG boost module (`{1, 1, 0x0709, 0x070a}`).
- `SHA256SUMS.txt`: SHA-256 verification checksums.

## Highlights
- 1440×3200 @ 120Hz 10bpc DSC display + Adreno 830 GPU + Synaptics S3910 touch
- Wi-Fi 7 (`ath12k_wifi7`) + Bluetooth + BT HID keyboard (`uhid`/`hidp`)
- 5G SA/4G mobile data (`ipa` v5.5 + `qwes-bridge` + `dmail` + watchdog)
- USB OTG Host (4.99V VBUS, mouse/HID verified) + USB Gadget + Xiaomi MCA charging
- Rear Ultrawide Camera (Samsung `S5KJN1`, 12.5MP RAW + `braun` JPEG)
- Boot/shutdown framebuffer console (`TER16x32`) & Droidloom `/proc/config.gz`

## Corresponding Source & Licensing (GPL-2.0)
- **Kernel base**: `linux-sm8750-piano` (`piano-dev`, Linux `7.2.10`)
- **Board patches & config**: `patches/0001..0007` and `configs/haotian.config` in the main source repository (`xiaomi-15-pro-mainline`)
- **Boot wrapper**: `Project-SunUEFI` (`SILICIUM_UEFI.fd`)
- **Initramfs**: `Denial` / `Phonewave` (`denial-initrd.cpio.gz`)
- **Proprietary firmware**: Not included. Extract read-only from your own device with `scripts/extract-firmware.sh`.

## Quick Flash
Back up your stock `boot_a` first:
```bash
fastboot flash boot_a boot-haotian-7.2.10-piano8.img
fastboot reboot
```
