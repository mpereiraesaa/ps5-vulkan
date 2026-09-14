#!/usr/bin/env python3
"""Verify the bounded GPU oracle for core sampler address and border modes."""
import argparse
import hashlib
import json
from pathlib import Path


CASES = (
    ("mirrored-repeat", -250, "ffff0000"),
    ("transparent-black-border", -2000, "00000000"),
    ("opaque-black-border", -2000, "ff000000"),
    ("opaque-white-border", -2000, "ffffffff"),
    ("nearest-center-control", 500, "ff000000"),
    ("linear-magnification", 500, "ff808080"),
    ("nearest-minification-control", 500, "ff000000"),
    ("linear-minification", 500, "ff808080"),
)


def require(value, message):
    if not value:
        raise ValueError(message)


def validate(log, metadata, artifact):
    identity = artifact.get("files", {}).get("eboot.bin", "")
    require(len(identity) == 64 and all(c in "0123456789abcdef" for c in identity),
            "artifact identity")
    require(artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("scissor_probe") == 6 and
            artifact.get("geometry_fixture") == "sampler-core-addressing" and
            artifact.get("termination") == "shell-close-after-cleanup", "artifact profile")
    require(hashlib.sha256(log).hexdigest() == metadata.get("sha256"), "log hash")
    require(metadata.get("clean") is True and metadata.get("bye") is True and
            metadata.get("gaps") == [] and metadata.get("transport") == "tcp" and
            metadata.get("protocol") == "ps5log/1", "transport")
    lines = log.decode().splitlines()
    require(lines and lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(word.split("=", 1) for word in lines[0].split()[2:])
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk" and
            all(metadata.get("identity", {}).get(key) == hello.get(key)
                for key in ("title", "app", "boot")), "runtime identity")
    records=[];previous=-1
    for sequence,line in enumerate(lines[1:-1],1):
        fields=line.split("\t",3);require(len(fields)==4,"record shape")
        seq,stamp,level,message=fields
        require(int(seq)==sequence and int(stamp)>=previous and level!="ERR","record integrity")
        previous=int(stamp);words=message.split()
        records.append((words[0],dict(word.split("=",1) for word in words[1:])))
    require(metadata.get("records")==len(records) and
            lines[-1]==f"BYE seq={len(records)} reason=graphics-api-end","bye/count")

    def matching(name):
        return [(index,fields) for index,(record,fields) in enumerate(records) if record==name]

    inputs=matching("PS5VK_SAMPLER_CORE_INPUT")
    results=matching("PS5VK_SAMPLER_CORE_READBACK")
    submits=matching("PS5VK_GRAPHICS_SUBMIT")
    completes=matching("PS5VK_GRAPHICS_COMPLETED")
    presents=matching("PS5VK_VIDEO_PRESENTED")
    ends=matching("PS5VK_GRAPHICS_REUSE_END")
    require(all(len(group)==len(CASES) for group in
                (inputs,results,submits,completes,presents,ends)),"eight GPU cases")
    last=-1
    pixels=[]
    for case,(name,uv,expected) in enumerate(CASES):
        ordered=[group[case][0] for group in (inputs,submits,completes,results,presents,ends)]
        require(last<ordered[0] and ordered==sorted(set(ordered)),"case order")
        last=ordered[-1]
        source=inputs[case][1];result=results[case][1]
        require(source.get("case")==result.get("case")==str(case) and
                source.get("name")==result.get("name")==name and
                int(source.get("uv_milli","999999"))==uv and
                source.get("minification")==str(int(case>=6)) and
                source.get("expected_bgra")==result.get("expected_bgra")==expected,
                "case identity")
        count=int(result.get("expected","0"))
        require(count==(1 if case>=6 else 373248) and int(result.get("other","-1"))==0 and
                result.get("valid")=="1" and submits[case][1].get("rc")=="0",
                "GPU sampler oracle")
        pixels.append(count)
    require(any(index>last and name=="PS5VK_PLATFORM_CLOSE" and
                fields.get("rc")=="0" and fields.get("allocations_bytes")=="0"
                for index,(name,fields) in enumerate(records)),"resource cleanup")
    require(any(name=="PS5VK_GRAPHICS_API_CLEANUP_COMPLETE" for name,_ in records),
            "API cleanup")
    return {"self_sha256":identity,"cases":len(CASES),"pixels":pixels,
            "mirrored_repeat":True,"fixed_border_colors":3,
            "nearest_linear_discriminator":True,
            "minification_discriminator":True,
            "gpu_readback":True,"process_exit_verified":False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log",type=Path);parser.add_argument("metadata",type=Path)
    parser.add_argument("artifact",type=Path)
    args=parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(),json.loads(args.metadata.read_text()),
                              json.loads(args.artifact.read_text())),indent=2))


if __name__=="__main__":
    main()
