#!/usr/bin/env bash
# Builds work/text/text_en.bin -- the English dialogue for the port -- from
# your own copy of the European Animal Crossing disc (GAFP01, .iso or .ciso)
# and the Japanese Animal Forest ROM. `gmake install` then copies it next to
# the EBOOT, and runtime/src/english/ uses it when present.
#
#   scripts/make_text_en.sh "Animal Crossing (Europe) (En,Fr,De,Es,It).ciso"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISC="${1:?usage: $0 <Animal Crossing PAL disc image>}"
JP_ROM="${JP_ROM:-$ROOT/work/af/baseroms/jp/baserom.z64}"
OUT="$ROOT/work/text"

mkdir -p "$OUT/gc"
PYTHONPATH="$ROOT/tools" python3 - "$DISC" "$OUT/gc" <<'EOF'
import sys
import gc_disc, rarc
disc, out = sys.argv[1], sys.argv[2]
img = gc_disc.open_image(disc, "tgc/forest_Eng_Final_PAL50.tgc")
rarc.unpack(img.read("forest_msg.arc"), out)
# names, choices, strings and item names (tools/make_names_en.py)
import os
names = os.path.join(out, "names")
os.makedirs(names, exist_ok=True)
for arc, files in (("forest_1st_script.arc", ("select.bin", "string.bin", "mail.bin", "super.bin", "ps.bin",
                                     "maila.bin", "mailb.bin", "mailc.bin", "superz.bin", "psz.bin")),
                   ("forest_2nd.arc", ("npc_name_str_table.bin",))):
    tmp = os.path.join(out, "arc_tmp")
    rarc.unpack(img.read(arc), tmp)
    for root, _, fs in os.walk(tmp):
        for f in fs:
            if f in files:
                os.replace(os.path.join(root, f), os.path.join(names, f))
open(os.path.join(names, "forestd.rel"), "wb").write(rarc.yaz0(img.read("forestd.rel.szs")))
EOF
python3 "$ROOT/tools/make_text_en.py" \
    "$OUT/gc/bin_msg/data/msg.bin" "$OUT/gc/bin_msg/data/msg_color.bmc" \
    "$JP_ROM" "$OUT/text_en.bin" --report "$OUT/report.tsv"
python3 "$ROOT/tools/make_names_en.py" "$OUT/gc/names" "$JP_ROM" "$OUT/names_en.bin" \
    --report "$OUT/names_report.tsv"
echo "Messages left to the ROM, with the reason: $OUT/report.tsv"
