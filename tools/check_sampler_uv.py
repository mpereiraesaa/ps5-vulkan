"""Verify the scoped constant-U ladder; not general sampler/graphics profile acceptance."""
import argparse
import json
from pathlib import Path

NUMERATORS = (4080,4084,4087,4088,4089,4091,4092,4093,4095,4096,4097,4104,4112)


def check(text):
    inputs, results = {}, {}
    for line in text.splitlines():
        fields = line.split("\t", 3)
        if len(fields) != 4:
            continue
        words = fields[3].split()
        if not words or words[0] not in ("PS5VK_SAMPLER_PROBE_INPUT", "PS5VK_TEXTURE_READBACK"):
            continue
        values = dict(item.split("=", 1) for item in words[1:])
        frame = int(values["frame"])
        destination = inputs if words[0].endswith("INPUT") else results
        if frame in destination or not 0 <= frame < len(NUMERATORS):
            raise ValueError("duplicate or unexpected frame")
        destination[frame] = values
    if set(inputs) != set(range(13)) or set(results) != set(inputs):
        raise ValueError("missing UV input/readback pairs")
    for frame, numerator in enumerate(NUMERATORS):
        v = inputs[frame]
        if tuple(int(v[k]) for k in ("u_numerator", "denominator", "v_numerator")) != (numerator,8192,2048):
            raise ValueError("unexpected UV fixture")
        # floor(round(2*u*256)/256) crosses at u=.5-1/1024.
        expected = (373248,0,0,0) if numerator < 4088 else (0,373248,0,0)
        if tuple(int(results[frame][k]) for k in ("red","green","blue","unexpected")) != expected:
            raise ValueError(f"UV selection or coverage mismatch at frame {frame}")
    return {"cases":13, "constant_u_boundary_matches_8_fractional_bit_model":True,
            "scope":"this U boundary at V=0.25 only; not general precision or graphics profile acceptance"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    print(json.dumps(check(parser.parse_args().log.read_text()),indent=2))
