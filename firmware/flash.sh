#!/usr/bin/env bash
# Copy build/zephyr/zephyr.uf2 onto the bootloader's USB drive.
#
# Put the board into the bootloader first: short RST to GND twice quickly, the
# LED starts breathing and a NICENANO (or similar) volume mounts. The board
# reboots into the new firmware as soon as the copy finishes.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd ~/ncs/v3.3.0
exec nrfutil toolchain-manager launch --ncs-version v3.3.0 -- \
  west flash -d "$here/build" "$@"
