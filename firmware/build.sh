#!/usr/bin/env bash
# Build the nRF52840 firmware with the nRF Connect SDK toolchain.
#
#   ./build.sh                    release: radio + panel, USB off
#   ./build.sh debug              console on USB CDC ACM (holds the crystal on)
#   ./build.sh [debug] --pristine after switching between the two, or on Kconfig/DT changes
#
# Any other arguments go to `west build`. Output: build/zephyr/zephyr.uf2,
# copied to ./epaper.uf2.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Which variant depends on what the bootloader's INFO_UF2.TXT says:
#   "SoftDevice: not found"  -> promicro_nrf52840/nrf52840      (app at 0x1000, the default here)
#   "SoftDevice: S140 6.x"   -> promicro_nrf52840/nrf52840/uf2  (app at 0x26000)
board="${BOARD:-promicro_nrf52840/nrf52840}"
extra=()
if [[ "${1:-}" == "debug" ]]; then
  shift
  extra=(-- -DEXTRA_CONF_FILE=debug.conf)
fi

# west needs to run inside the workspace; the app lives outside it.
cd ~/ncs/v3.3.0
nrfutil toolchain-manager launch --ncs-version v3.3.0 -- \
  west build --no-sysbuild -b "$board" \
  -d "$here/build" "$here" "$@" "${extra[@]}"

# The flashable image always sits next to this script, from the last build.
cp "$here/build/zephyr/zephyr.uf2" "$here/epaper.uf2"
echo "-> $here/epaper.uf2"
