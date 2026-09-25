#!/usr/bin/env python3
"""Build the bounded public-SDK helper-invocation (demote vs terminate) witness."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t11_kill_depth_witness import (  # noqa: E402
    CAP_DEMOTE, CAP_SHADER, OP_DEMOTE, OP_KILL, OP_TERMINATE, SPIRV_1_0, SPIRV_1_6,
    build_witness)

SPIRV_1_3 = 0x00010300
FRAGMENT = "experiments/graphics/runtime_helper_derivative.frag"
CASES = ("control", "kill", "demote", "demote_dxvk", "demote_ext", "terminate")

# array -> (source, glslang arguments, SPIR-V version, capabilities, removal
# opcode, strip OpExtension)
SHADERS = {
    "t11_quad_vert_spirv": ("experiments/graphics/t09_depth_stencil_quad.vert", (),
                            SPIRV_1_0, {CAP_SHADER}, None, False),
    "t11_helper_control_frag_spirv": (FRAGMENT, ("-DREMOVE=0",), SPIRV_1_0,
                                      {CAP_SHADER}, None, False),
    "t11_helper_kill_frag_spirv": (FRAGMENT, (), SPIRV_1_0, {CAP_SHADER}, OP_KILL, False),
    "t11_helper_demote_frag_spirv": (FRAGMENT, ("--target-env", "vulkan1.3", "-DDEMOTE=1"),
                                     SPIRV_1_6, {CAP_SHADER, CAP_DEMOTE}, OP_DEMOTE, False),
    # DXVK 2.6.2's DXBC spelling: SPIR-V 1.6 core demote, no OpExtension.
    "t11_helper_demote_dxvk_frag_spirv": (FRAGMENT,
                                          ("--target-env", "vulkan1.3", "-DDEMOTE=1"),
                                          SPIRV_1_6, {CAP_SHADER, CAP_DEMOTE}, OP_DEMOTE,
                                          True),
    "t11_helper_demote_ext_frag_spirv": (FRAGMENT,
                                         ("--target-env", "vulkan1.1", "-DDEMOTE=1"),
                                         SPIRV_1_3, {CAP_SHADER, CAP_DEMOTE}, OP_DEMOTE,
                                         False),
    "t11_helper_terminate_frag_spirv": (FRAGMENT, ("--target-env", "vulkan1.3"), SPIRV_1_6,
                                        {CAP_SHADER}, OP_TERMINATE, False),
}


def main() -> None:
    build_witness(
        name="t11_helper_witness", shaders=SHADERS, header="t11_helper_shaders.h",
        profile="t11-helper-public-sdk-witness",
        content_id="UP9000-PPSA99994_00-PS5VKHW000000001",
        title="PS5 Vulkan Helper Invocation Witness",
        extra={"extent": 64, "format": "R8G8B8A8_UNORM", "cases": list(CASES)})


if __name__ == "__main__":
    main()
