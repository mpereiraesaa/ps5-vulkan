"""Check six exact GPU witness results against independently generated values.

Does not attest artifact identity, transport completeness or final cleanup.
"""
import argparse
import json
from pathlib import Path
try:
    from .scene_witnesses import witnesses, DEPTH_OFF_INDICES
except ImportError:
    from scene_witnesses import witnesses, DEPTH_OFF_INDICES


def check(text, depth_off=False):
    expected=witnesses()
    indices=DEPTH_OFF_INDICES if depth_off else tuple(range(len(expected)))
    found=[]
    depths=[]
    for line in text.splitlines():
        fields=line.split("\t",3)
        if len(fields)!=4:continue
        if fields[3].startswith("PS5VK_WITNESS_DEPTH "):
            depths.append(dict(p.split("=",1) for p in fields[3].split()[1:]))
            continue
        if not fields[3].startswith("PS5VK_SCENE_WITNESS "):continue
        parts=dict(p.split("=",1) for p in fields[3].split()[1:])
        index=int(parts["index"])
        if len(found)>=len(indices) or index!=indices[len(found)]:raise ValueError("witness order/duplicate")
        w=expected[index]
        if any(int(parts[k])!=w[k] for k in ("frame","x","y","owner")):
            raise ValueError("witness input")
        if int(parts["expected_bgra"],16)!=w["off_bgra" if depth_off else "bgra"]:raise ValueError("expected color")
        if any(int(parts[k])!=v for k,v in (("expected_count",1),("other",0),("changed",1),("valid",1))):
            raise ValueError("witness readback")
        found.append(index)
    if len(found)!=len(indices):raise ValueError("missing witnesses")
    if depth_off or depths:
        if depths!=[dict(index=str(i),enabled="0" if depth_off else "1") for i in indices]:
            raise ValueError("depth state")
    return dict(exact_witnesses=len(found),tolerance_pixels=0,full_image_proven=False)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("log",type=Path)
    p.add_argument("--depth-off",action="store_true")
    a=p.parse_args();print(json.dumps(check(a.log.read_text(),a.depth_off),indent=2))
