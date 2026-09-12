"""Report regional differences against the analytical reference; no pass threshold.

This diagnostic does not certify artifact identity, transport or lifecycle.
Keep raw input telemetry private and audit those separately.
"""
import argparse
import json
from pathlib import Path

try:
    from .scene_reference import tile
except ImportError:
    from scene_reference import tile

COLORS = ("black", "red", "green", "blue")
EXPECTED = {(f,x,y) for f in (0,45,90,135) for y in range(2,6) for x in range(6,10)}


def parse_regions(text):
    found={}
    for line in text.splitlines():
        fields=line.split("\t",3)
        if len(fields)!=4 or not fields[3].startswith("PS5VK_SCENE_REGION "):
            continue
        parts=dict(w.split("=",1) for w in fields[3].split()[1:])
        key=tuple(int(parts[k]) for k in ("frame","tile_x","tile_y"))
        if key not in EXPECTED or key in found or int(parts["unexpected"])!=0:
            raise ValueError("unexpected or duplicate region")
        counts={k:int(parts[k]) for k in COLORS}
        if any(v<0 for v in counts.values()) or sum(counts.values())!=16384:
            raise ValueError("region counts")
        found[key]=counts
    if set(found)!=EXPECTED:
        raise ValueError("missing regions")
    return found


def compare(text, texel_fraction_bits=None):
    rows=[]
    for (f,x,y),actual in sorted(parse_regions(text).items()):
        reference=tile(x,y,f,texel_fraction_bits)
        delta={c:actual[c]-reference["counts"][c] for c in COLORS}
        rows.append(dict(frame=f,tile_x=x,tile_y=y,actual=actual,
                         expected=reference["counts"],delta=delta,
                         overlap_nearest_cube=reference["overlap_nearest_cube"]))
    return dict(regions=len(rows),max_channel_error=max(abs(v) for r in rows for v in r["delta"].values()),
                acceptance_defined=False,texel_fraction_bits_hypothesis=texel_fraction_bits,rows=rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("log",type=Path)
    p.add_argument("--texel-fraction-bits",type=int,choices=range(4,17),
                   help="diagnostic fixed-point rounding hypothesis; does not redefine acceptance")
    a=p.parse_args()
    print(json.dumps(compare(a.log.read_text(),a.texel_fraction_bits),indent=2))


if __name__=="__main__":
    main()
