#!/usr/bin/env bash
# Put one of the CA79 images on the USB stick as CAx9_P06.SYS (the name the boot loader looks for).
#
#   tools/usb_put.sh stock    <- original CAx9_P06.SYS, the recovery image
#   tools/usb_put.sh install  <- opengrand_install.SYS, the one-file install (docs/FLASHING.md)
#   tools/usb_put.sh <path>   <- any other .SYS
#
# Copies through a Windows staging directory (WSL cannot write the removable drive
# directly without mounting it), then reads the hash back from the stick.
# DRIVE=G: tools/usb_put.sh stock   overrides the target drive.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DRIVE="${DRIVE:-F:}"
STAGE_WIN='C:\Temp\ca79'
STAGE_WSL='/mnt/c/Temp/ca79'

case "${1:-}" in
  stock)  SRC="$ROOT/target/CAx9_P06.SYS" ;;
  install) SRC="$ROOT/result/stockfw/build/opengrand_install.SYS" ;;
  "")     echo "usage: $0 {stock|install|<path.SYS>}" >&2; exit 2 ;;
  *)      SRC="$1" ;;
esac

[ -f "$SRC" ] || { echo "no such image: $SRC" >&2; exit 1; }
want=$(sha256sum "$SRC" | cut -d' ' -f1)
size=$(stat -c%s "$SRC")
[ "$size" -eq 3801088 ] || { echo "refusing: $SRC is $size bytes, expected 3801088" >&2; exit 1; }

mkdir -p "$STAGE_WSL"
cp "$SRC" "$STAGE_WSL/CAx9_P06.SYS"
powershell.exe -NoProfile -Command \
  "Copy-Item -Force '$STAGE_WIN\\CAx9_P06.SYS' '$DRIVE\\CAx9_P06.SYS'; Start-Sleep -Milliseconds 500; (Get-FileHash -Algorithm SHA256 '$DRIVE\\CAx9_P06.SYS').Hash.ToLower()" \
  | tr -d '\r' | grep -E '^[0-9a-f]{64}$' > /tmp/.ca79_usb_hash
rm -rf "$STAGE_WSL"
got=$(cat /tmp/.ca79_usb_hash); rm -f /tmp/.ca79_usb_hash

echo "source : $SRC"
echo "on $DRIVE : $got"
if [ "$got" = "$want" ]; then
  echo "OK - matches. Eject the stick from Windows before unplugging it."
else
  echo "MISMATCH - expected $want" >&2
  exit 1
fi
