"""Adapt the bounded procedural graphics ABI to the reusable lab header builder.

This does not claim general descriptor/vertex input support. The explicit user
SGPR contract must be extended before textured/indexed pipeline acceptance.
"""


def validate_user_abi(stages):
    expected = {"gs": [0x10000000, 0x10000003, 0x10000004], "ps": [0x10000000]}
    if stages["gs"]["user_map"][:4] == [0x10000000, 0x1000000f, 0x10000003, 0x10000004]:
        expected["gs"] = [0x10000000, 0x1000000f, 0x10000003, 0x10000004]
    if stages.get("ps", {}).get("user_map", [])[:2] == [0x10000000, 0]:
        expected["ps"] = [0x10000000, 0]
    for name, mapping in expected.items():
        stage = stages[name]
        if stage["user_sgprs"] != len(mapping) or stage["user_map"] != mapping + [0xffffffff] * (32 - len(mapping)):
            raise ValueError("Shader user-data ABI requires a new header adapter")


def header_values(manifest):
    requirements = manifest["graphics_requirements"]
    stages = requirements["stages"]
    validate_user_abi(stages)
    graphics = requirements["graphics_register_metadata"]
    if graphics[".vgt_gs_out_prim_type"] != {".outprim_type": "TriStrip"}:
        raise ValueError("Only triangle primitive output audited")
    if graphics[".db_shader_control"].get(".z_export_enable", 0):
        raise ValueError("Depth export header not implemented")
    context = manifest["shader_context"]
    def convert(rows):
        result = {}
        for row in rows:
            address = row["byte_address"]
            if address % 4 or not 0x28000 <= address < 0x29000:
                raise ValueError("Not a context register")
            offset = (address - 0x28000) // 4
            if offset in result:
                raise ValueError("Duplicate context register")
            result[offset] = row["value"]
        return result
    pre = convert(context["pre_raster"])
    pixel = convert(context["pixel"])
    # Reference header has two reserved context entries and a zero Z-export
    # format entry. Keep them explicit rather than reporting these as PAL data.
    pre.update({0x207: 0, 0x2e4: 0})
    pixel[0x1c4] = 0
    pre_order = (0x1ff, 0x2d3, 0x207, 0x1c2, 0x1c3, 0x1b1, 0x2ab, 0x2e4, 0x2ce, 0x291)
    pixel_order = (0x08f, 0x203, 0x310, 0x1b8, 0x1b4, 0x1b3, 0x1b6, 0x1c5, 0x1c4)
    if set(pre) != set(pre_order) or set(pixel) != set(pixel_order):
        raise ValueError("Shader header context contract changed")
    group = graphics[".vgt_gs_onchip_cntl"]
    prims, vertices = group[".gs_prims_per_subgroup"], group[".es_verts_per_subgroup"]
    if not 0 <= prims <= 511 or not 0 <= vertices <= 511:
        raise ValueError("GE group count out of range")
    return dict(pre=[(o, pre[o]) for o in pre_order], pixel=[(o, pixel[o]) for o in pixel_order],
        gs_rsrc1=stages["gs"]["rsrc1"], gs_rsrc2=stages["gs"]["rsrc2"],
        ps_rsrc1=stages["ps"]["rsrc1"], ps_rsrc2=stages["ps"]["rsrc2"],
        ge_cntl=prims | vertices << 9, stages_enable=context["stages_enable"]["value"],
        draw_modifier=5, out_prim_type=2)


def render_header(values):
    def array(name, rows):
        return "static const ps5_agc_register " + name + "[] = {\n" + ",\n".join(
            f"    {{{offset:#x}u, {value:#x}u}}" for offset, value in rows) + "\n};\n"
    return ("/* Generated from owned shader PAL metadata; host preparation only. */\n"
        "#ifndef PS5VK_GRAPHICS_HEADER_GENERATED\n#define PS5VK_GRAPHICS_HEADER_GENERATED\n"
        '#include "ps5_shader_header.h"\n' + array("ps5vk_pre_cx", values["pre"]) +
        array("ps5vk_pixel_cx", values["pixel"]) +
        "static const struct ps5_shader_metadata ps5vk_graphics_metadata = {\n" +
        "".join(f"    .{field} = {values[key]:#x}u,\n" for field, key in (
            ("gs_rsrc1", "gs_rsrc1"), ("gs_rsrc2", "gs_rsrc2"),
            ("ps_rsrc1", "ps_rsrc1"), ("ps_rsrc2", "ps_rsrc2"),
            ("ge_cntl", "ge_cntl"), ("shader_stages_en", "stages_enable"),
            ("gs_out_prim_type", "out_prim_type"), ("draw_modifier", "draw_modifier"))) +
        "    .pre_raster_cx = ps5vk_pre_cx, .pre_raster_cx_count = 10,\n"
        "    .pixel_cx = ps5vk_pixel_cx, .pixel_cx_count = 9,\n};\n#endif\n")
