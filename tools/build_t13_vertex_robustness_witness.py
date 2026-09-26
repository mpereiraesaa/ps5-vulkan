#!/usr/bin/env python3
"""Build the bounded public-SDK robustness2 vertex-input witness (null vertex
buffer and a draw entirely past a vertex buffer).

VK_EXT_robustness2 ships on the ordinary SDK, so ROBUSTNESS2_SWITCH is None and
the witness is the route's native regression check."""

import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t11_kill_depth_witness import CAP_SHADER, SPIRV_1_0, build_witness  # noqa: E402

ROBUSTNESS2_SWITCH: str | None = None
WIDTH, HEIGHT, COLUMNS = 48, 16, 3
# array -> (source, glslang arguments, SPIR-V version, capabilities, removal
# opcode, strip OpExtension)
SHADERS = {
    "t13_vertex_robustness_vert_spirv": ("experiments/graphics/t13_vertex_robustness.vert", (),
                                         SPIRV_1_0, {CAP_SHADER}, None, False),
    "t13_vertex_robustness_frag_spirv": ("experiments/graphics/t13_vertex_robustness.frag", (),
                                         SPIRV_1_0, {CAP_SHADER}, None, False),
}


def main() -> None:
    if ROBUSTNESS2_SWITCH:
        os.environ[ROBUSTNESS2_SWITCH] = "1"
    build_witness(
        name="t13_vertex_robustness_witness", shaders=SHADERS,
        header="t13_vertex_robustness_shaders.h",
        profile="t13-vertex-robustness-public-sdk-witness",
        content_id="UP9000-PPSA99994_00-PS5VKVR000000001",
        title="PS5 Vulkan Vertex Robustness Witness",
        extra={"width": WIDTH, "height": HEIGHT, "columns": COLUMNS,
               "sdk_switch": ROBUSTNESS2_SWITCH})


if __name__ == "__main__":
    main()
