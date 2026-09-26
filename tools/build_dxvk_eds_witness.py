#!/usr/bin/env python3
"""Build the bounded public-SDK witness for VK_EXT_extended_dynamic_state's
dynamic primitive topology and dynamic vertex input binding stride.

EDS_SWITCH names the measurement build that reports the extension before it
ships; after promotion it is None and the witness is the regression witness on
the ordinary SDK."""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_t11_kill_depth_witness import CAP_SHADER, SPIRV_1_0, build_witness  # noqa: E402

EDS_SWITCH: str | None = None
WIDTH, HEIGHT, COLUMNS = 128, 16, 8
CAP_GEOMETRY = 2
# array -> (source, glslang arguments, SPIR-V version, capabilities, removal
# opcode, strip OpExtension)
SHADERS = {
    "dxvk_eds_vert_spirv": ("experiments/graphics/dxvk_eds.vert", (),
                            SPIRV_1_0, {CAP_SHADER}, None, False),
    "dxvk_eds_frag_spirv": ("experiments/graphics/dxvk_eds.frag", (),
                            SPIRV_1_0, {CAP_SHADER}, None, False),
    "dxvk_eds_geom_spirv": ("experiments/graphics/dxvk_eds.geom", (),
                            SPIRV_1_0, {CAP_GEOMETRY}, None, False),
}


def main() -> None:
    if EDS_SWITCH:
        os.environ[EDS_SWITCH] = "1"
    build_witness(
        name="dxvk_eds_witness", shaders=SHADERS, header="dxvk_eds_shaders.h",
        profile="dxvk-eds-public-sdk-witness",
        content_id="UP9000-PPSA99994_00-PS5VKEDS00000001",
        title="PS5 Vulkan Dynamic State Witness",
        extra={"width": WIDTH, "height": HEIGHT, "columns": COLUMNS,
               "sdk_switch": EDS_SWITCH})


if __name__ == "__main__":
    main()
