#!/usr/bin/env bash
# Runs the built game in FS-UAE from a hard disk folder (run/<model>/):
# the exe as "dave", dave.ocs, dave.aga and an S:startup-sequence that
# starts it. High scores (DSCORES.DAV) are saved into that folder on quit.
#
#   tools/run_fsuae.sh [ocs|aga]      (make run / make run-aga build first)
#
# ocs: an A500, Kickstart 1.3, 512 KB chip + 512 KB slow RAM (the
#      32-colour version).
# aga: an A4000, Kickstart 3.1 (the 256-colour version). An A1200 model
#      with some Kickstart 3.1 images freezes the game a few seconds into
#      the title, so the A4000 is the tested AGA machine.
#
# Kickstart ROMs are not part of this project. ROMS (default ../roms)
# must hold amiga500.rom (KS 1.3) and, for aga, amiga1200.rom (KS 3.1);
# or set KICK_OCS / KICK_AGA to the files themselves. Extra FS-UAE options
# go after the model, e.g.  tools/run_fsuae.sh ocs --fullscreen=1
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODEL="${1:-ocs}"
shift || true
ROMS="${ROMS:-$ROOT/../roms}"

fail() { echo "ERROR: $*" >&2; exit 1; }

case "$MODEL" in
    ocs) KICK="${KICK_OCS:-$ROMS/amiga500.rom}";  AMIGA=A500 ;;
    aga) KICK="${KICK_AGA:-$ROMS/amiga1200.rom}"; AMIGA=A4000 ;;
    *)   fail "model is ocs or aga, not '$MODEL'" ;;
esac

command -v fs-uae >/dev/null || fail "fs-uae not found (sudo apt install fs-uae)"
[ -f "$KICK" ] || fail "Kickstart ROM $KICK not found (set ROMS, KICK_OCS or KICK_AGA)"
for f in dave.exe dave.ocs dave.aga; do
    [ -f "$ROOT/$f" ] || fail "$f not built (run make)"
done

HD="$ROOT/run/$MODEL"
mkdir -p "$HD/s"
cp "$ROOT/dave.exe" "$HD/dave"
cp "$ROOT/dave.ocs" "$ROOT/dave.aga" "$HD/"
printf 'dave\n' > "$HD/s/startup-sequence"

CONF="$ROOT/run/$MODEL.fs-uae"
{
    echo "[config]"
    echo "amiga_model = $AMIGA"
    echo "kickstart_file = $KICK"
    if [ "$MODEL" = ocs ]; then
        echo "chip_memory = 512"
        echo "slow_memory = 512"
    fi
    echo "hard_drive_0 = $HD"
    echo "hard_drive_0_label = DAVE"
    echo "automatic_input_grab = 0"
    echo "window_width = 960"
    echo "window_height = 600"
} > "$CONF"

exec fs-uae "$CONF" "$@"
