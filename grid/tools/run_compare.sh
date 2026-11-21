#!/usr/bin/env bash
# Build the grid module, run both scales through the C demo and the Python
# reference tool, and cross-check every shared metric.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/data"
BUILD="$ROOT/build"
export TMPDIR="${TMPDIR:-$ROOT/../.tmp}"
mkdir -p "$TMPDIR"

make -C "$ROOT" clean >/dev/null
make -C "$ROOT" build/test >/dev/null
"$BUILD/test"
make -C "$ROOT" build/demo >/dev/null

python3 -I "$ROOT/tools/gen_data.py" "$DATA"

# scale -> input, cols, rows
declare -A COLS=( [small]=25 [large]=80 )
declare -A ROWS=( [small]=20 [large]=63 )

rc=0
for scale in small large; do
    in="$DATA/$scale.txt"
    cols="${COLS[$scale]}"
    rows="${ROWS[$scale]}"
    echo "=================================================================="
    echo "scale=$scale  input=$in  grid=${cols}x${rows}"
    echo "------------------------------------------------------------------"
    echo "[C demo]"
    c_out="$("$BUILD/demo" "$in" "$cols" "$rows")"
    echo "$c_out"
    echo "[python reference]"
    py_out="$(python3 -I "$ROOT/tools/ref_tool.py" "$in" "$cols" "$rows")"
    echo "$py_out"

    python3 - "$c_out" "$py_out" <<'PYEOF'
import re
import sys

c_lines, py_line = sys.argv[1], sys.argv[2]

# parse C output structurally (its keys repeat across the two pass lines)
c = {}
m = re.search(r"n=(\d+)", c_lines); c["n"] = int(m.group(1))
m = re.search(r"groups=(\d+).*pairs=(\d+)", c_lines)
c["groups"], c["merge_pairs"] = int(m.group(1)), int(m.group(2))
p1 = re.search(r"pass1.*dup=(-?\d+) cand=(-?\d+)", c_lines)
c["dup1"], c["cand1"] = int(p1.group(1)), int(p1.group(2))
p2 = re.search(r"pass2.*dup=(-?\d+) cand=(-?\d+)", c_lines)
c["dup2"], c["cand2"] = int(p2.group(1)), int(p2.group(2))

py = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", py_line)}

ok = True
for key in ("n", "groups", "merge_pairs", "dup1", "cand1", "dup2", "cand2"):
    match = c[key] == py[key]
    ok &= match
    print(f"  cross-check {key:12s}: C={c[key]:>8d} py={py[key]:>8d} "
          f"{'OK' if match else 'MISMATCH'}")
print("RESULT:", "MATCH" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
PYEOF
done

echo "=================================================================="
echo "limit enforcement check"
"$BUILD/demo" "$DATA/small.txt" 2000 2000 && rc=1 || echo "  -> rejected as expected (cells cap), exit=$?"
exit "$rc"
