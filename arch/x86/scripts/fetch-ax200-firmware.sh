#!/bin/bash
# Fetch the unmodified diagnostic fixture and its license, not a Windows driver.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/ax200-firmware}
test ! -e "$out"
mkdir -p "$out"
out=$(realpath "$out")
revision=797d34e622b2262ca0777e98fd40b1d29034169d
base="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$revision"
curl --fail --location --retry 2 --max-time 60 \
    "$base/LICENSES/LICENCE.iwlwifi_firmware" -o "$out/LICENCE.iwlwifi_firmware"
curl --fail --location --retry 2 --max-time 60 \
    "$base/intel/iwlwifi/iwlwifi-cc-a0-77.ucode" -o "$out/iwlwifi-cc-a0-77.ucode"
cd "$out"
printf '%s\n' \
    '94f5fb915f2074f98059365e11cca313e25a6fa3b7cd6772e80b3ba5edfb966b  iwlwifi-cc-a0-77.ucode' \
    '16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5  LICENCE.iwlwifi_firmware' \
    | sha256sum --check
printf 'Firmware and license verified in %s\n' "$out"
