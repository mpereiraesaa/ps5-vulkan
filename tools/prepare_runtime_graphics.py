"""Generate an owned SPIR-V fixture header; native PSBC compiles its ISA at runtime."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
from tess_patch_fixture import relocate_total_patch
from tess_mode_fixture import swap_owned_modes

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--out",required=True,type=Path)
    args=parser.parse_args()
    compiler=os.environ.get("PS5VK_GLSLANG") or shutil.which("glslangValidator")
    if not compiler:
        raise SystemExit("Set PS5VK_GLSLANG to glslangValidator or install glslang-tools")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    declarations=["/* Generated from owned GLSL. Contains SPIR-V, not GPU machine code. */"]
    # gl_ViewIndex needs the ViewIndex built-in and the MultiView capability,
    # which belong to SPIR-V 1.3: this one module is compiled for the Vulkan 1.1
    # target environment, exactly as the compiler fork's own ViewIndex
    # regression is (opengnm-psbc Makefile's view-index rule). Every other
    # module keeps the default Vulkan 1.0 target this driver has always used.
    module_flags = {"view_index": ("--target-env", "vulkan1.1"),
                    "fragment_store_control": ("-DCONTROL=1",),
                    "tess_joint_envelope_control": ("-DWITH_PATCH_ENVELOPE=1",),
                    "tess_joint_envelope_vertex": ("-DWITH_PATCH_ENVELOPE=1",),
                    "tess_joint_envelope_evaluation": ("-DWITH_PATCH_ENVELOPE=1",),
                    "view_index_instance": ("--target-env", "vulkan1.1"),
                    # The coverage witness declares the distance arrays; the
                    # control is the same source with neither declared.
                    "clip_cull_probe": ("-DWITH_DISTANCES=1",)}
    variant=int(os.environ.get("PS5VK_TESS_VARIANT","35"))
    matrix_index=variant-35 if 35<=variant<=43 else 0
    matrix_flags=(f"-DMATRIX_DOMAIN={matrix_index//3}",f"-DMATRIX_SPACING={matrix_index%3}")
    module_flags["tess_matrix_control"]=matrix_flags
    module_flags["tess_matrix_evaluation"]=matrix_flags
    discard_index=variant-44 if 44<=variant<=52 else 0
    module_flags["tess_discard_evaluation"]=(f"-DMATRIX_DOMAIN={discard_index//3}",f"-DMATRIX_SPACING={discard_index%3}")
    modules = (
        ("experiments/graphics/runtime_tess_discard.tesc", "runtime_tess_discard.tesc.spv", "tess_discard_control"),
        ("experiments/graphics/runtime_tess_discard.tese", "runtime_tess_discard.tese.spv", "tess_discard_evaluation"),
        ("experiments/graphics/runtime_tess_matrix.tesc", "runtime_tess_matrix.tesc.spv", "tess_matrix_control"),
        ("experiments/graphics/runtime_tess_matrix.tese", "runtime_tess_matrix.tese.spv", "tess_matrix_evaluation"),
        ("experiments/graphics/runtime_tess_push_member.vert", "runtime_tess_push_member.vert.spv", "tess_push_member_vertex"),
        ("experiments/graphics/runtime_tess_push_member.tesc", "runtime_tess_push_member.tesc.spv", "tess_push_member_control"),
        ("experiments/graphics/runtime_tess_indexed_instance.vert", "runtime_tess_indexed_instance.vert.spv", "tess_indexed_instance_vertex"),
        ("experiments/graphics/runtime_tess_output_envelope.tese", "runtime_tess_output_envelope.tese.spv", "tess_output_envelope_evaluation"),
        ("experiments/graphics/runtime_tess_output_envelope.frag", "runtime_tess_output_envelope.frag.spv", "tess_output_envelope_fragment"),
        ("experiments/graphics/runtime_tess_dynamic_distance.frag", "runtime_tess_dynamic_distance.frag.spv", "tess_dynamic_distance_fragment"),
        ("experiments/graphics/runtime_tess_mixed.tese", "runtime_tess_mixed.tese.spv", "tess_mixed_evaluation"),
        ("experiments/graphics/runtime_tess_mixed.frag", "runtime_tess_mixed.frag.spv", "tess_mixed_fragment"),
        ("experiments/graphics/runtime_tess_cullonly.tese", "runtime_tess_cullonly.tese.spv", "tess_cullonly_evaluation"),
        ("experiments/graphics/runtime_tess_cullonly.frag", "runtime_tess_cullonly.frag.spv", "tess_cullonly_fragment"),
        ("experiments/graphics/runtime_tess_total.tesc", "runtime_tess_total.tesc.spv", "tess_total_control"),
        ("experiments/graphics/runtime_tess_total.tese", "runtime_tess_total.tese.spv", "tess_total_evaluation"),
        ("experiments/graphics/runtime_tess_envelope.vert", "runtime_tess_joint_envelope.vert.spv", "tess_joint_envelope_vertex"),
        ("experiments/graphics/runtime_tess_envelope.tesc", "runtime_tess_joint_envelope.tesc.spv", "tess_joint_envelope_control"),
        ("experiments/graphics/runtime_tess_envelope.tese", "runtime_tess_joint_envelope.tese.spv", "tess_joint_envelope_evaluation"),
        ("experiments/graphics/runtime_tess_patch_envelope.tesc", "runtime_tess_patch_envelope.tesc.spv", "tess_patch_envelope_control"),
        ("experiments/graphics/runtime_tess_patch_envelope.tese", "runtime_tess_patch_envelope.tese.spv", "tess_patch_envelope_evaluation"),
        ("experiments/graphics/runtime_tess_envelope.vert", "runtime_tess_envelope.vert.spv", "tess_envelope_vertex"),
        ("experiments/graphics/runtime_tess_envelope.tesc", "runtime_tess_envelope.tesc.spv", "tess_envelope_control"),
        ("experiments/graphics/runtime_tess_envelope.tese", "runtime_tess_envelope.tese.spv", "tess_envelope_evaluation"),
        ("experiments/graphics/runtime_vertex_bindings_probe.vert", "runtime_vertex_bindings_probe.vert.spv", "vertex_bindings"),
        ("experiments/graphics/runtime_triangle.vert", "runtime_triangle.vert.spv", "vertex"),
        ("experiments/graphics/runtime_triangle.frag", "runtime_triangle.frag.spv", "fragment"),
        ("experiments/graphics/runtime_dual_source.frag",
         "runtime_dual_source.frag.spv", "dual_source_fragment"),
        ("experiments/graphics/runtime_two_mrt.frag",
         "runtime_two_mrt.frag.spv", "two_mrt_fragment"),
        ("experiments/graphics/runtime_fragment_store.frag",
         "runtime_fragment_store_control.frag.spv", "fragment_store_control"),
        ("experiments/graphics/runtime_fragment_store.frag",
         "runtime_fragment_store.frag.spv", "fragment_store_atomic"),
        ("experiments/graphics/runtime_view_index.vert", "runtime_view_index.vert.spv", "view_index"),
        ("experiments/graphics/runtime_view_index_instance.vert", "runtime_view_index_instance.vert.spv", "view_index_instance"),
        ("experiments/graphics/runtime_vertex_sint.vert", "runtime_vertex_sint.vert.spv", "vertex_sint"),
        ("experiments/graphics/runtime_vertex_uint.vert", "runtime_vertex_uint.vert.spv", "vertex_uint"),
        ("experiments/graphics/runtime_vertex_unorm.vert", "runtime_vertex_unorm.vert.spv", "vertex_unorm"),
        ("experiments/graphics/runtime_vertex_format.frag", "runtime_vertex_format.frag.spv", "vertex_format_fragment"),
        ("experiments/graphics/runtime_texture.frag", "runtime_texture.frag.spv", "texture_fragment"),
        ("experiments/graphics/runtime_mipmap.vert", "runtime_mipmap.vert.spv", "mipmap_vertex"),
        ("experiments/graphics/runtime_input_attachment.vert", "runtime_input_attachment.vert.spv", "input_attachment_vertex"),
        ("experiments/graphics/runtime_input_attachment_pattern.frag", "runtime_input_attachment_pattern.frag.spv", "input_attachment_pattern"),
        ("experiments/graphics/runtime_input_attachment_transform.frag", "runtime_input_attachment_transform.frag.spv", "input_attachment_transform"),
        ("experiments/graphics/runtime_clip_cull_probe.vert", "runtime_clip_cull_probe.vert.spv", "clip_cull_probe"),
        ("experiments/graphics/runtime_clip_cull_probe.vert", "runtime_clip_cull_control.vert.spv", "clip_cull_control"),
        # The pixel end of the clip/cull interface: a fragment stage that reads
        # gl_ClipDistance[0], so the witness measures whether the rasterizer
        # delivers the interpolated distance the pre-raster stage exported.
        ("experiments/graphics/runtime_clip_distance_read.frag",
         "runtime_clip_distance_read.frag.spv", "clip_distance_read_fragment"),
        ("experiments/graphics/runtime_geometry_probe.vert", "runtime_geometry_probe.vert.spv", "geometry_vertex"),
        # Readback-only pre-raster half: a position whose x is unique per vertex
        # index, so the bytes the geometry half reads name the ES item they came
        # from instead of only their sign.
        ("experiments/graphics/runtime_geometry_identity.vert",
         "runtime_geometry_identity.vert.spv", "geometry_identity_vertex"),
        ("experiments/graphics/runtime_geometry_probe.geom", "runtime_geometry_probe.geom.spv", "geometry_stage"),
        # Envelope-only pre-raster half: a 256-vertex emission, the mandatory
        # minimum maxGeometryOutputVertices names. max_vertices is module-level,
        # so the envelope case needs a module of its own.
        ("experiments/graphics/runtime_geometry_envelope.geom",
         "runtime_geometry_envelope.geom.spv", "geometry_envelope_stage"),
        # Invocations-only pre-raster half: 32 invocations, each placing a marker
        # coloured by its invocation id. invocations is module-level too.
        ("experiments/graphics/runtime_geometry_invocations.geom",
         "runtime_geometry_invocations.geom.spv", "geometry_invocations_stage"),
        # Components-only pair: a pre-raster stage exporting 64 components and a
        # geometry stage that declares, reads and writes that many.
        ("experiments/graphics/runtime_geometry_primitive_id.geom",
         "runtime_geometry_primitive_id.geom.spv", "geometry_primitive_id_stage"),
        ("experiments/graphics/runtime_geometry_components.vert",
         "runtime_geometry_components.vert.spv", "geometry_components_vertex"),
        ("experiments/graphics/runtime_geometry_components.geom",
         "runtime_geometry_components.geom.spv", "geometry_components_stage"),
        # Synthetic suppress diagnostic only: an input-less fragment stage, so
        # the geometry half's suppress case (which emits nothing) still forms a
        # legal pipeline instead of being refused for an unmatched input.
        ("experiments/graphics/runtime_geometry_suppress.frag",
         "runtime_geometry_suppress.frag.spv", "geometry_suppress_fragment"),
        # The input families a geometryShader device is expected to accept: a
        # pre-raster half whose positions and colours identify the vertex, plus
        # the point-list and line-list geometry stages that read their input
        # primitive's own arity (one vertex per point, two per line).
        ("experiments/graphics/runtime_geometry_family.vert",
         "runtime_geometry_family.vert.spv", "geometry_family_vertex"),
        ("experiments/graphics/runtime_geometry_points.geom",
         "runtime_geometry_points.geom.spv", "geometry_points_stage"),
        ("experiments/graphics/runtime_geometry_lines.geom",
         "runtime_geometry_lines.geom.spv", "geometry_lines_stage"),
        # Diagnostic-only pair for the primitive-restart witness (order-probe
        # payloads): two quads with a gap, an indexed strip whose index list
        # carries a restart index between them, and a vertex stage that colours
        # each quad by its index value.
        ("experiments/graphics/runtime_primitive_restart.vert",
         "runtime_primitive_restart.vert.spv", "primitive_restart_vertex"),
        ("experiments/graphics/runtime_primitive_restart.frag",
         "runtime_primitive_restart.frag.spv", "primitive_restart_fragment"),
        # The component envelope's pixel half: it declares an input for all
        # sixteen vec4 outputs the geometry half writes, so the OUTPUT side of
        # the component minimum is consumed rather than only declared.
        ("experiments/graphics/runtime_geometry_output_components.frag",
         "runtime_geometry_output_components.frag.spv",
         "geometry_output_components_fragment"),
        # The tessellation witness: two triangle patches, the left tessellated
        # at level three and the right at level one, whose evaluation half
        # paints the QUANTISED tessCoord field - the image names the
        # sub-triangle it came from, which is what makes the tessellator's
        # levels observable rather than only the patch's coverage.
        ("experiments/graphics/runtime_tess_witness.vert",
         "runtime_tess_witness.vert.spv", "tess_witness_vertex"),
        ("experiments/graphics/runtime_tess_witness.tesc",
         "runtime_tess_witness.tesc.spv", "tess_witness_control"),
        ("experiments/graphics/runtime_tess_witness.tese",
         "runtime_tess_witness.tese.spv", "tess_witness_evaluation"),
        ("experiments/graphics/runtime_tess_witness.frag",
         "runtime_tess_witness.frag.spv", "tess_witness_fragment"),
        # The TessCoord control: the same pipeline shape with an evaluation
        # half whose position is a pure function of gl_TessCoord and a control
        # half that writes only the levels - no off-chip reads at all, so the
        # draw isolates the tessellator + the domain launch from the ring
        # delivery.
        ("experiments/graphics/runtime_tess_coord.vert",
         "runtime_tess_coord.vert.spv", "tess_coord_vertex"),
        ("experiments/graphics/runtime_tess_delivery.vert",
         "runtime_tess_delivery.vert.spv", "tess_delivery_vertex"),
        ("experiments/graphics/runtime_tess_indexed.vert",
         "runtime_tess_indexed.vert.spv", "tess_indexed_vertex"),
        ("experiments/graphics/runtime_tess_instance.vert",
         "runtime_tess_instance.vert.spv", "tess_instance_vertex"),
        ("experiments/graphics/runtime_tess_two_patch.vert",
         "runtime_tess_two_patch.vert.spv", "tess_two_patch_vertex"),
        ("experiments/graphics/runtime_tess_patch_data.tesc",
         "runtime_tess_patch_data.tesc.spv", "tess_patch_data_control"),
        ("experiments/graphics/runtime_tess_patch_data.tese",
         "runtime_tess_patch_data.tese.spv", "tess_patch_data_evaluation"),
        ("experiments/graphics/runtime_tess_quad.tesc",
         "runtime_tess_quad.tesc.spv", "tess_quad_control"),
        ("experiments/graphics/runtime_tess_quad.tese",
         "runtime_tess_quad.tese.spv", "tess_quad_evaluation"),
        ("experiments/graphics/runtime_tess_points.tese",
         "runtime_tess_points.tese.spv", "tess_points_evaluation"),
        ("experiments/graphics/runtime_tess_level64.tesc",
         "runtime_tess_level64.tesc.spv", "tess_level64_control"),
        ("experiments/graphics/runtime_tess_level64.tese",
         "runtime_tess_level64.tese.spv", "tess_level64_evaluation"),
        ("experiments/graphics/runtime_tess_points.geom",
         "runtime_tess_points.geom.spv", "tess_points_geometry"),
        ("experiments/graphics/tess_specialization.vert",
         "tess_specialization.vert.spv", "tess_spec_vertex"),
        ("experiments/graphics/tess_specialization.tesc",
         "tess_specialization.tesc.spv", "tess_spec_control"),
        ("experiments/graphics/tess_specialization.tese",
         "tess_specialization.tese.spv", "tess_spec_evaluation"),
        ("experiments/graphics/runtime_tess_isoline.tesc",
         "runtime_tess_isoline.tesc.spv", "tess_isoline_control"),
        ("experiments/graphics/runtime_tess_isoline.tese",
         "runtime_tess_isoline.tese.spv", "tess_isoline_evaluation"),
        ("experiments/graphics/runtime_tess_patch32.vert",
         "runtime_tess_patch32.vert.spv", "tess_patch32_vertex"),
        ("experiments/graphics/runtime_tess_unused_output.vert",
         "runtime_tess_unused_output.vert.spv", "tess_unused_output_vertex"),
        ("experiments/graphics/runtime_tess_patch32.tesc",
         "runtime_tess_patch32.tesc.spv", "tess_patch32_control"),
        ("experiments/graphics/runtime_tess_expand32.tesc",
         "runtime_tess_expand32.tesc.spv", "tess_expand32_control"),
        ("experiments/graphics/runtime_tess_patch32.tese",
         "runtime_tess_patch32.tese.spv", "tess_patch32_evaluation"),
        ("experiments/graphics/runtime_tess_delivery.tesc",
         "runtime_tess_delivery.tesc.spv", "tess_delivery_control"),
        ("experiments/graphics/runtime_tess_delivery.tese",
         "runtime_tess_delivery.tese.spv", "tess_delivery_evaluation"),
        ("experiments/graphics/runtime_tess_coord.tesc",
         "runtime_tess_coord.tesc.spv", "tess_coord_control"),
        ("experiments/graphics/runtime_tess_coord.tese",
         "runtime_tess_coord.tese.spv", "tess_coord_evaluation"),
        ("experiments/graphics/runtime_tess_coord.frag",
         "runtime_tess_coord.frag.spv", "tess_coord_fragment"),
        ("experiments/graphics/runtime_tess_blend.frag",
         "runtime_tess_blend.frag.spv", "tess_blend_fragment"),
        ("experiments/graphics/runtime_tess_dense.tese",
         "runtime_tess_dense.tese.spv", "tess_dense_evaluation"),
        ("experiments/graphics/runtime_tess_dense.frag",
         "runtime_tess_dense.frag.spv", "tess_dense_fragment"),
        ("experiments/graphics/runtime_tess_coord_zero.tesc",
         "runtime_tess_coord_zero.tesc.spv", "tess_coord_zero_control"),
        # The domain-execution witness: the TessCoord control's evaluation
        # half with one addition, a storage-buffer write. A tessellation
        # evaluation shader has no per-vertex input except gl_TessCoord, so if
        # it runs at all its exports must cover pixels - and the measured
        # image is empty with no foreign pixels either. A memory write is the
        # only observable the stage has that does not depend on
        # rasterisation, which is what separates "the domain never executes"
        # from "it executes and its exports are discarded".
        ("experiments/graphics/runtime_tess_witness_exec.tese",
         "runtime_tess_witness_exec.tese.spv", "tess_witness_exec_evaluation"),
        # The witness's own control: the same vertex half with a write to the
        # same buffer at a different counter. "The domain wrote nothing" and
        # "a storage-buffer write from a graphics stage does not land" read
        # identically, and this half is proven to run because its control
        # half's tessellation factors are in memory.
        ("experiments/graphics/runtime_tess_witness_exec.vert",
         "runtime_tess_witness_exec.vert.spv", "tess_witness_exec_vertex"),
        # The same control half with levels of 16 instead of 2 and 1, to test
        # whether the domain fails to launch because the tessellated work
        # never reaches a batching threshold rather than because something is
        # misconfigured. Four triangles against an eleven-triangle
        # accumulator is not a comparison anyone has made yet.
        ("experiments/graphics/runtime_tess_coord_high.tesc",
         "runtime_tess_coord_high.tesc.spv", "tess_coord_high_control"),
        # The descriptor-free domain-execution witness: an evaluation half
        # that costs time instead of writing memory, so "did the domain run"
        # is answered by the bounded fence wait the harness already has,
        # without a storage buffer whose own delivery cannot be validated.
        ("experiments/graphics/runtime_tess_spin.tese",
         "runtime_tess_spin.tese.spv", "tess_spin_evaluation"),
        # The same witness on the ISOLINE domain: two outer levels instead of
        # three plus inner, a two-dword factor layout instead of four, line
        # output instead of triangles. A materially different tessellator
        # configuration measured by the same descriptor-free mechanism.
        ("experiments/graphics/runtime_tess_spin_iso.tese",
         "runtime_tess_spin_iso.tese.spv", "tess_spin_iso_evaluation"),
        # The POSITIVE CONTROL for that witness: the same spin in the CONTROL
        # half, which is proven to execute because its factors are in the
        # ring. Reading "no stall" as "did not execute" is only sound if a
        # stage that does execute produces a stall.
        ("experiments/graphics/runtime_tess_spin_hull.tesc",
         "runtime_tess_spin_hull.tesc.spv", "tess_spin_hull_control"),
    )
    for source_name,binary_name,stage in modules:
        binary=args.out.parent/binary_name
        subprocess.run([compiler,"-V",*module_flags.get(stage,()),str(ROOT/source_name),
                        "-o",str(binary)],check=True)
        data=binary.read_bytes()
        if stage in ("tess_total_control", "tess_total_evaluation"):
            data=relocate_total_patch(data)
            binary.write_bytes(data)
        words=struct.unpack(f"<{len(data)//4}I",data)
        declarations.append(f"static const uint32_t ps5vk_runtime_{stage}[]={{"+
                            ",".join(f"0x{word:08x}u" for word in words)+"};")
    swapped = swap_owned_modes(
        (args.out.parent/"runtime_tess_coord.tesc.spv").read_bytes(),
        (args.out.parent/"runtime_tess_output_envelope.tese.spv").read_bytes())
    for stage,data in zip(("tess_swapped_control", "tess_swapped_evaluation"), swapped):
        words=struct.unpack(f"<{len(data)//4}I",data)
        declarations.append(f"static const uint32_t ps5vk_runtime_{stage}[]={{"+
                            ",".join(f"0x{word:08x}u" for word in words)+"};")
    args.out.write_text("\n".join(declarations)+"\n")

if __name__=="__main__":
    main()
