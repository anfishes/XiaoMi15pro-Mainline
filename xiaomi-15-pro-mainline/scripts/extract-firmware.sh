#!/usr/bin/env bash
# Run on the Xiaomi 15 Pro (as root) to extract device-signed firmware READ-ONLY
# from stock partitions into /usr/lib/firmware/.
# NEVER writes to super or modem_a.
set -euo pipefail

FW_QCOM="/usr/lib/firmware/qcom/sm8750/xiaomi/haotian"
mkdir -p "$FW_QCOM" /usr/lib/firmware/qca /mnt/haotian_modem_ro

echo "==> 1. Mounting modem_a read-only for ADSP / CDSP / Modem / IPA / GPU / BT firmware..."
mount -o ro /dev/disk/by-partlabel/modem_a /mnt/haotian_modem_ro

# ADSP & CDSP (device-signed .mdt + .b* + .jsn)
cp -a /mnt/haotian_modem_ro/image/adsp* "$FW_QCOM/"
cp -a /mnt/haotian_modem_ro/image/cdsp* "$FW_QCOM/"

# MPSS Modem & IPA v5.5 GSI firmware
cp -a /mnt/haotian_modem_ro/image/modem* "$FW_QCOM/"
cp -a /mnt/haotian_modem_ro/image/ipa_fws* "$FW_QCOM/"

# Adreno 830 GPU ZAP shader
cp -a /mnt/haotian_modem_ro/image/gen80000_zap* "$FW_QCOM/" 2>/dev/null || true

# WCN7860 / FastConnect 7900 Bluetooth firmware
cp -a /mnt/haotian_modem_ro/image/gngbtfw20.mbn /usr/lib/firmware/qca/ 2>/dev/null || true
cp -a /mnt/haotian_modem_ro/image/gngbtnv20.*   /usr/lib/firmware/qca/ 2>/dev/null || true

umount /mnt/haotian_modem_ro

echo "==> 2. CS35L43 speaker DSP firmware:"
echo "    Extract the following files from your stock odm_a partition (read-only)"
echo "    into /usr/lib/firmware/:"
echo "      - cs35l43-dsp1-spk-prot.wmfw"
echo "      - B-cs35l43-dsp1-spk-prot.bin"
echo "      - T-cs35l43-dsp1-spk-prot.bin"
echo "    (and optional *-delta-*-spk-prot.bin tuning files)"
echo "==> Done."
