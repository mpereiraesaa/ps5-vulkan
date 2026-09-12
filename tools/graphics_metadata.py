"""Decode explicit graphics stage requirements; not a complete AGC header.

Resource bit locations follow pinned Mesa gfx10.json. DX10_CLAMP is an explicit
initial backend policy shared with the lab graphics reference, not PAL metadata.
Unknown execution requirements fail instead of inheriting the compute ABI.
"""
import re
import yaml


class PalLoader(yaml.SafeLoader):
    pass


PalLoader.add_constructor("!str", lambda loader, node: loader.construct_scalar(node))


def decode_graphics_metadata(notes):
    match = re.search(r"AMDGPU Metadata: ---\n(.*?)\n\.\.\.", notes, re.S)
    if not match or "amdgcn--amdpal--gfx1013" not in notes:
        raise ValueError("Missing gfx1013 PAL metadata")
    metadata = yaml.load(match[1], Loader=PalLoader)
    pipelines = metadata.get("amdpal.pipelines", [])
    if len(pipelines) != 1 or pipelines[0].get(".type") != "Ngg":
        raise ValueError("Expected one NGG pipeline")
    pipeline = pipelines[0]
    hardware = pipeline[".hardware_stages"]
    if set(hardware) != {".gs", ".ps"}:
        raise ValueError("Unsupported graphics stage combination")
    graphics = pipeline[".graphics_registers"]
    result = {}
    def integer(value, low, high):
        if not isinstance(value, int) or not low <= value <= high:
            raise ValueError("Metadata integer outside supported bounds")
        return value
    def boolean(value):
        return integer(value, 0, 1)
    for name in (".gs", ".ps"):
        stage = hardware[name]
        for key in (".scratch_en", ".scratch_memory_size", ".lds_size", ".trap_present",
                    ".debug_mode", ".offchip_lds_en"):
            if stage.get(key, 0):
                raise ValueError(f"Unsupported {name} execution requirement {key}")
        wave = stage[".wavefront_size"]
        if wave not in (32, 64) or (name == ".gs" and wave != 32):
            raise ValueError("Unsupported stage wave size")
        vgprs = integer(stage[".vgpr_count"], 1, 256)
        sgprs = integer(stage[".sgpr_count"], 1, 106)
        users = integer(stage[".user_sgprs"], 1, 32)
        user_map = stage[".user_data_reg_map"]
        if len(user_map) != 32 or any(not isinstance(x, int) or not 0 <= x <= 0xffffffff for x in user_map):
            raise ValueError("Malformed user SGPR map")
        if any(x != 0xffffffff for x in user_map[users:]):
            raise ValueError("User map exceeds allocated registers")
        expected_symbol = "_amdgpu_gs_main" if name == ".gs" else "_amdgpu_ps_main"
        if stage[".entry_point_symbol"] != expected_symbol:
            raise ValueError("Unexpected graphics entry symbol")
        rsrc1 = (vgprs - 1) // (8 if wave == 32 else 4)
        rsrc1 |= ((sgprs - 1) // 8) << 6
        rsrc1 |= integer(stage[".float_mode"], 0, 255) << 12
        rsrc1 |= 1 << 21  # Explicit DX10_CLAMP backend policy.
        rsrc1 |= boolean(stage[".ieee_mode"]) << 23
        rsrc1 |= boolean(stage[".mem_ordered"]) << 25
        rsrc2 = (users & 31) << 1 | (users >> 5) << 27
        if name == ".gs":
            rsrc1 |= boolean(stage.get(".wgp_mode", False)) << 27
            rsrc1 |= integer(graphics[".gs_vgpr_comp_cnt"], 0, 3) << 29
            rsrc2 |= integer(graphics[".es_vgpr_comp_cnt"], 0, 3) << 16
        elif stage.get(".wgp_mode", False) or graphics.get(".ps_extra_lds_size", 0):
            raise ValueError("Unsupported pixel execution requirements")
        result[name[1:]] = dict(symbol=expected_symbol, wave_size=wave, vgprs=vgprs, sgprs=sgprs,
            user_sgprs=users, user_map=user_map, rsrc1=rsrc1, rsrc2=rsrc2)
    if boolean(graphics[".vgt_shader_stages_en"][".gs_w32_en"]) != 1:
        raise ValueError("Pre-raster wave metadata mismatch")
    if boolean(graphics[".spi_ps_in_control"][".ps_w32_en"]) != (result["ps"]["wave_size"] == 32):
        raise ValueError("Pixel wave metadata mismatch")
    return dict(pipeline_type="Ngg", stages=result, graphics_register_metadata=graphics,
                register_policy="gfx10-dx10-clamp", complete_agc_metadata=False)
