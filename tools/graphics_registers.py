"""Pack PAL graphics fields using the pinned public Mesa GFX10 register schema.

No GPU register positions are inferred from dictionary insertion order. This
produces shader-context words, not a complete pipeline or AGC object header.
"""
import hashlib
import json

SCHEMA_SHA256 = "f3435695c47c3c3b1d8329a96b098452d5909944ca2c99995520c22614617d48"


class RegisterSchema:
    def __init__(self, data):
        if hashlib.sha256(data).hexdigest() != SCHEMA_SHA256:
            raise ValueError("Register schema changed: audit required")
        schema = json.loads(data)
        self.types = schema["register_types"]
        self.registers = {r["name"]: r for r in schema["register_mappings"]}

    def pack(self, name, values):
        register = self.registers[name]
        fields = {f["name"]: f for f in self.types[register["type_ref"]]["fields"]}
        word = 0
        for key, value in values.items():
            if key not in fields:
                raise ValueError(f"Unsupported {name}.{key}")
            low, high = fields[key]["bits"]
            if not isinstance(value, int) or not 0 <= value < (1 << (high - low + 1)):
                raise ValueError(f"Out-of-range {name}.{key}")
            word |= value << low
        address = register["map"]["at"]
        if register["map"]["to"] != "mm" or address % 4:
            raise ValueError("Unsupported register address space")
        return dict(name=name, byte_address=address, value=word)


def shader_context(requirements, schema):
    g = requirements["graphics_register_metadata"]
    aliases = {"THREADS_PER_SUBGROUP": "THDS_PER_SUBGRP",
        "ES_VERTS_PER_SUBGROUP": "ES_VERTS_PER_SUBGRP",
        "GS_PRIMS_PER_SUBGROUP": "GS_PRIMS_PER_SUBGRP",
        "GS_INST_PRIMS_PER_SUBGRP": "GS_INST_PRIMS_IN_SUBGRP",
        "NUM_INTERPS": "NUM_INTERP", "ES_STAGE_EN": "ES_EN",
        "GS_STAGE_EN": "GS_EN", "VS_STAGE_EN": "VS_EN",
        "MAX_PRIMGROUP_IN_WAVE": "MAX_PRIMGRP_IN_WAVE"}
    aliases.update({f"COL_{i}_EXPORT_FORMAT": f"COL{i}_EXPORT_FORMAT" for i in range(8)})
    def fields(key):
        return {aliases.get(k[1:].upper(), k[1:].upper()): v for k, v in g[key].items()}
    def reg(name, values=None):
        return schema.pack(name, fields("." + name.lower()) if values is None else values)
    pre = [
        reg("GE_MAX_OUTPUT_PER_SUBGROUP", {"MAX_VERTS_PER_SUBGROUP": g[".max_verts_per_subgroup"]}),
        reg("GE_NGG_SUBGRP_CNTL"),
        reg("SPI_SHADER_IDX_FORMAT", {"IDX0_EXPORT_FORMAT": g[".spi_shader_idx_format"]}),
        reg("SPI_SHADER_POS_FORMAT", {f"POS{i}_EXPORT_FORMAT": v for i, v in enumerate(g[".spi_shader_pos_format"])}),
        reg("SPI_VS_OUT_CONFIG"),
        reg("VGT_ESGS_RING_ITEMSIZE", {"ITEMSIZE": g[".vgt_esgs_ring_itemsize"]}),
        reg("VGT_GS_MAX_VERT_OUT", {"MAX_VERT_OUT": g[".vgt_gs_max_vert_out"]}),
        reg("VGT_GS_ONCHIP_CNTL"),
    ]
    pixel = [reg(name) for name in ("CB_SHADER_MASK", "DB_SHADER_CONTROL",
        "PA_SC_SHADER_CONTROL", "SPI_BARYC_CNTL", "SPI_PS_INPUT_ADDR",
        "SPI_PS_INPUT_ENA", "SPI_PS_IN_CONTROL", "SPI_SHADER_COL_FORMAT")]
    interpolators = g[".spi_ps_input_cntl"]
    if len(interpolators) != g[".spi_ps_in_control"][".num_interps"] or len(interpolators) > 32:
        raise ValueError("Interpolator count mismatch")
    inputs = []
    for i, item in enumerate(interpolators):
        # PAL can describe a later-generation primitive attribute bit absent
        # from this GFX10 register schema. Only its disabled form is supported.
        if item.get(".prim_attr", False):
            raise ValueError("Primitive attributes not supported")
        inputs.append(reg(f"SPI_PS_INPUT_CNTL_{i}",
            {k[1:].upper(): v for k, v in item.items() if k != ".prim_attr"}))
    return dict(schema_sha256=SCHEMA_SHA256, pre_raster=pre, pixel=pixel,
                vertex_quantization=reg("PA_SU_VTX_CNTL"),
                interpolators=inputs, stages_enable=reg("VGT_SHADER_STAGES_EN"),
                complete_pipeline=False)
