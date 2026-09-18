#!/usr/bin/env python3
"""Build and package genuine upstream VK-GL-CTS for native PS5 execution."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lab import lab_root

SELECTION_MANIFEST = ROOT / "cts/upstream/manifest.json"
# Upstream shader sources the packaged cases load from the /app0 data archive.
DATASET_SHADER_SOURCES = (
    "vulkan/draw/VertexFetchShaderDrawParameters.vert",
    "vulkan/draw/VertexFetchShaderDrawParametersDrawIndex.vert",
    "vulkan/draw/VertexFetch.vert",
    "vulkan/draw/VertexFetchInstanceIndex.vert",
    "vulkan/draw/VertexFetchInstanced.vert",
    "vulkan/draw/VertexFetchInstancedFirstInstance.vert",
    "vulkan/draw/VertexFetch.frag",
    "vulkan/draw/NegateData.comp",
)

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def git_head(path: Path) -> str:
    """Commit id of a checked-out dependency, or 'unknown' when not a git tree."""
    try:
        result = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def check_external_pins(expected: dict, actual: dict) -> None:
    """Refuse to build against dependency revisions that were never validated.

    The CTS external/ manifest and the checkouts present in a working tree can
    drift apart, so the validated revisions are pinned in the selection manifest
    and verified here. Set PS5VK_ALLOW_UNPINNED_DEPS=1 to build against a
    different set knowingly (the build manifest still records what was used).
    """
    mismatches = [(name, expected[name], actual[name])
                  for name in sorted(expected)
                  if name in actual and actual[name] != expected[name]]
    if not mismatches:
        return
    for name, want, got in mismatches:
        print(f"[build_upstream_cts] dependency pin mismatch: {name}: "
              f"expected {want}, found {got}", file=sys.stderr)
    if os.environ.get("PS5VK_ALLOW_UNPINNED_DEPS") == "1":
        print("[build_upstream_cts] PS5VK_ALLOW_UNPINNED_DEPS=1: continuing with "
              "unvalidated dependency revisions", file=sys.stderr)
        return
    raise SystemExit(
        "Refusing to build against unvalidated dependency revisions. "
        "Check out the revisions in cts/upstream/manifest.json:external_pins, "
        "or set PS5VK_ALLOW_UNPINNED_DEPS=1 to build anyway.")


def verify_cts_checkout(cts_root: Path, expected_commit: str,
                        expected_tag: Optional[str] = None) -> Dict[str, Any]:
    """Verify the main CTS checkout really is the declared, unmodified revision.

    The build manifest used to write the CTS commit as a constant without ever
    looking at the tree, so a different or patched checkout could be compiled
    while being reported as the pin. Check the revision and tracked-file
    cleanliness here instead. Set PS5VK_ALLOW_DIRTY_UPSTREAM=1 when a deliberate
    adaptation patch is applied to the CTS tree.
    """
    actual = git_head(cts_root)
    if actual == "unknown":
        raise SystemExit(f"not a git checkout, cannot verify CTS pin: {cts_root}")

    dirty = subprocess.run(
        ["git", "-C", str(cts_root), "status", "--porcelain", "--untracked-files=no"],
        capture_output=True, text=True, check=True).stdout.strip()

    problems = []
    if actual != expected_commit:
        problems.append(f"revision is {actual}, expected {expected_commit}")
    if expected_tag:
        try:
            tag_commit = subprocess.run(
                ["git", "-C", str(cts_root), "rev-parse", f"{expected_tag}^{{commit}}"],
                capture_output=True, text=True, check=True).stdout.strip()
        except subprocess.CalledProcessError:
            tag_commit = ""
        if tag_commit != expected_commit:
            problems.append(
                f"tag {expected_tag} resolves to {tag_commit or '<missing>'}, "
                f"expected {expected_commit}")
    if dirty:
        problems.append("tracked files are locally modified:\n" + dirty)

    if problems:
        for problem in problems:
            print(f"[build_upstream_cts] upstream checkout problem: {problem}",
                  file=sys.stderr)
        if os.environ.get("PS5VK_ALLOW_DIRTY_UPSTREAM") == "1":
            print("[build_upstream_cts] PS5VK_ALLOW_DIRTY_UPSTREAM=1: continuing with "
                  "a non-standard upstream tree", file=sys.stderr)
        else:
            raise SystemExit(
                "Refusing to build from a CTS checkout that does not match the "
                "declared pin and is free of local modifications. Restore the "
                "checkout, or set PS5VK_ALLOW_DIRTY_UPSTREAM=1 to build anyway.")

    return {"commit": actual, "dirty": bool(dirty)}


def parse_depfile(path: Path) -> List[str]:
    """Header dependencies recorded by the compiler (-MMD)."""
    text = path.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
    deps: List[str] = []
    for line in text.splitlines():
        if ":" not in line:
            continue
        _, _, right = line.partition(":")
        deps.extend(token for token in right.split() if token)
    return deps


def write_focused_storage_source(source: Path, destination: Path,
                                 allowed_names: tuple[str, ...],
                                 expected_sites: int) -> None:
    """Generate a registration-pruned copy of one pinned upstream module.

    Shader assembly, resource construction and oracle code remain byte-for-byte
    upstream.  Only the final addChild site is guarded, so cases outside the
    audited storage-only subset are destroyed after construction instead of
    accumulating in the title's small application heap while dEQP enumerates
    the package tree.
    """
    text = source.read_text(encoding="utf-8")
    needle = "group->addChild(new SpvAsmComputeShaderCase(testCtx, testName.c_str(), spec));"
    if text.count(needle) != expected_sites:
        raise SystemExit(
            f"focused storage registration layout drift in {source}: "
            f"expected {expected_sites} addChild sites, found {text.count(needle)}")
    condition = " || ".join(f'testName == "{name}"' for name in allowed_names)
    replacement = f"if ({condition})\n                {needle}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text.replace(needle, replacement), encoding="utf-8")


def write_focused_buffer_copy_source(source: Path, destination: Path) -> None:
    """Keep the original buffer-copy bodies/oracles but prune registration.

    The complete copies/blits module builds a very large test tree before the
    case-list filter runs.  On PS5 that consumes the bounded application heap
    for image/blit/resolve families that are not selected.  Replace only the
    two pinned registration functions; every selected test implementation and
    result oracle remains byte-for-byte upstream.  The image-to-image group is
    registered through upstream's own simple-only factory so the audited RGBA8
    transfer leaves exist in the packaged tree without the all-formats, 3D,
    cube, array or blit/resolve families.
    """
    text = source.read_text(encoding="utf-8")
    old_core = """void addCoreCopiesAndBlittingTests(tcu::TestCaseGroup *group)
{
    uint32_t extensionFlags = 0;
    addCopiesAndBlittingTests(group, ALLOCATION_KIND_SUBALLOCATED, extensionFlags);
    addBufferCopyOffsetTests(group);
}"""
    new_core = """void addCoreCopiesAndBlittingTests(tcu::TestCaseGroup *group)
{
    const uint32_t extensionFlags = 0;
    TestGroupParamsPtr universalGroupParams(new TestGroupParams{
        ALLOCATION_KIND_SUBALLOCATED,
        extensionFlags,
        QueueSelectionOptions::Universal,
        false,
        false,
    });
    addTestGroup(group, \"buffer_to_buffer\", addBufferToBufferTests,
                 universalGroupParams);
    addTestGroup(group, \"image_to_image\", addImageToImageTestsSimpleOnly,
                 universalGroupParams);
}"""
    simple_only = ("void addImageToImageTestsSimpleOnly(tcu::TestCaseGroup *group, "
                   "TestGroupParamsPtr testGroupParams)")
    old_factory = """    copiesAndBlittingTests->addChild(createTestGroup(testCtx, \"core\", addCoreCopiesAndBlittingTests, cleanupGroup));
    copiesAndBlittingTests->addChild(
        createTestGroup(testCtx, \"dedicated_allocation\", addDedicatedAllocationCopiesAndBlittingTests, cleanupGroup));
    copiesAndBlittingTests->addChild(createTestGroup(
        testCtx, \"copy_commands2\",
        [](tcu::TestCaseGroup *group) { addCopiesAndBlittingTests(group, ALLOCATION_KIND_DEDICATED, COPY_COMMANDS_2); },
        cleanupGroup));
    copiesAndBlittingTests->addChild(createTestGroup(
        testCtx, \"sparse\",
        [](tcu::TestCaseGroup *group)
        { addSparseCopyTests(group, ALLOCATION_KIND_DEDICATED, COPY_COMMANDS_2 | SPARSE_BINDING); },
        cleanupGroup));"""
    new_factory = """    copiesAndBlittingTests->addChild(createTestGroup(
        testCtx, \"core\", addCoreCopiesAndBlittingTests, cleanupGroup));"""
    if text.count(old_core) != 1 or text.count(old_factory) != 1:
        raise SystemExit(f"focused buffer-copy registration layout drift in {source}")
    if text.count(simple_only) != 1:
        raise SystemExit(
            f"focused image-to-image factory drift in {source}: "
            f"expected one addImageToImageTestsSimpleOnly definition")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text.replace(old_core, new_core).replace(
        old_factory, new_factory), encoding="utf-8")


def write_focused_robust_buffer_source(source: Path, destination: Path) -> None:
    """Prune robust-buffer registration to the audited Vulkan 1.0 subset.

    The selected cases retain upstream's shaders, resource setup, support
    checks and result oracles.  Only the three registration loops are guarded
    so the PS5 title does not construct the complete robustness tree before
    the command-line case filter is applied.
    """
    text = source.read_text(encoding="utf-8")
    replacements = {
        """        const VkShaderStageFlagBits stage = bufferAccessStages[stageNdx];
        de::MovePtr<tcu::TestCaseGroup> stageTests""":
        """        const VkShaderStageFlagBits stage = bufferAccessStages[stageNdx];
        if (stage != VK_SHADER_STAGE_COMPUTE_BIT)
            continue;
        de::MovePtr<tcu::TestCaseGroup> stageTests""",
        """        for (int shaderTypeNdx = 0; shaderTypeNdx < SHADER_TYPE_COUNT; shaderTypeNdx++)
        {
            const VkFormat *formats;""":
        """        for (int shaderTypeNdx = 0; shaderTypeNdx < SHADER_TYPE_COUNT; shaderTypeNdx++)
        {
            if ((ShaderType)shaderTypeNdx != SHADER_TYPE_SCALAR_COPY)
                continue;
            const VkFormat *formats;""",
        """                const VkFormat bufferFormat = formats[formatNdx];

                rangeMultiplier""":
        """                const VkFormat bufferFormat = formats[formatNdx];
                if (bufferFormat != VK_FORMAT_R32_UINT)
                    continue;

                rangeMultiplier""",
    }
    for old, new in replacements.items():
        if text.count(old) != 1:
            raise SystemExit(
                f"focused robust-buffer registration layout drift in {source}: "
                f"expected one registration site, found {text.count(old)}")
        text = text.replace(old, new)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8")


def object_is_current(obj: Path, dep_file: Path, stamp: Path,
                      fingerprint: str) -> bool:
    """Reuse an object only when its command, sources and headers are unchanged.

    Compiler-generated dependency files replace the old mtime-of-the-.cpp test,
    which silently ignored header and flag changes.
    """
    if not (obj.is_file() and dep_file.is_file() and stamp.is_file()):
        return False
    if stamp.read_text(encoding="utf-8").strip() != fingerprint:
        return False

    try:
        obj_mtime = obj.stat().st_mtime
    except OSError:
        return False

    for dep in parse_depfile(dep_file):
        try:
            if Path(dep).stat().st_mtime >= obj_mtime:
                return False
        except OSError:
            return False
    return True


def compile_worker(task):
    cmd, src, obj, env = task
    obj.parent.mkdir(parents=True, exist_ok=True)
    dep_file = Path(str(obj) + ".d")
    stamp = Path(str(obj) + ".cmd")

    # The fingerprint covers the full compile command (compiler, flags, include
    # paths, source) plus the pinned-input signature, so a flag or pin change
    # invalidates the object even when the .cpp itself is untouched.
    fingerprint = hashlib.sha256(
        ("\x00".join(cmd) + "\x00" + env.get("PS5VK_BUILD_SIGNATURE", "")).encode()
    ).hexdigest()

    if object_is_current(obj, dep_file, stamp, fingerprint):
        return src.name, True, None

    try:
        res = subprocess.run(cmd + ["-MMD", "-MF", str(dep_file)],
                             capture_output=True, text=True, env=env)
        if res.returncode != 0:
            return src.name, False, res.stderr + "\n" + res.stdout
        stamp.write_text(fingerprint + "\n", encoding="utf-8")
        return src.name, True, None
    except Exception as e:
        return src.name, False, str(e)

def main():
    lab = lab_root()
    foundation = lab / "third_party/ps5-native-app-boilerplate"
    sdk_env = os.environ.get("PS5_PAYLOAD_SDK")
    if sdk_env:
        sdk = Path(sdk_env).resolve()
    else:
        sdk = foundation / ".deps/native/ps5-payload-sdk"
    
    if not ((sdk / "bin/prospero-lld").is_file() and (sdk / "target/lib").is_dir()):
        raise SystemExit(f"Invalid PS5 SDK: {sdk}")

    native = foundation / "tooling/native"
    builder = foundation / "build/host/ps5-native-tool"
    gears = lab / "projects/ps5-agc-gears"
    logger = lab / "projects/logging_server/client"
    cts_root = ROOT / "third_party/vk-gl-cts"
    glslang_root = cts_root / "external/glslang/src"
    amber_root = cts_root / "external/amber/src"
    spirv_tools_root = cts_root / "external/spirv-tools/src"
    spirv_headers_root = cts_root / "external/spirv-headers/src"

    selection_manifest = json.loads(SELECTION_MANIFEST.read_text())
    cts_pin = selection_manifest["cts_pin"]
    cts_state = verify_cts_checkout(cts_root, cts_pin["commit"], cts_pin.get("tag"))
    check_external_pins(selection_manifest.get("external_pins", {}), {
        "glslang": git_head(glslang_root),
        "spirv-tools": git_head(spirv_tools_root),
        "spirv-headers": git_head(spirv_headers_root),
        "amber": git_head(amber_root),
    })

    out = ROOT / "build/upstream-cts"
    obj_dir = out / "obj"
    dist = ROOT / "dist-upstream-cts/PPSA99994"
    amber_gen = out / "amber-gen"

    for d in (out, obj_dir, dist / "sce_sys", dist / "sce_module", out / "stubs"):
        d.mkdir(parents=True, exist_ok=True)

    focused_sources = out / "focused-storage-sources"
    storage_module = cts_root / "external/vulkancts/modules/vulkan/spirv_assembly"
    write_focused_storage_source(
        storage_module / "vktSpvAsm8bitStorageTests.cpp",
        focused_sources / "vktSpvAsm8bitStorageTests.cpp",
        ("storage_buffer_scalar_sint", "storage_buffer_scalar_uint",
         "storage_buffer_vector_sint", "storage_buffer_vector_uint"),
        7)
    write_focused_storage_source(
        storage_module / "vktSpvAsm16bitStorageTests.cpp",
        focused_sources / "vktSpvAsm16bitStorageTests.cpp",
        ("uniform_buffer_block_scalar_sint", "uniform_buffer_block_scalar_uint",
         "uniform_buffer_block_vector_sint", "uniform_buffer_block_vector_uint"),
        12)
    write_focused_buffer_copy_source(
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiCopiesAndBlittingTests.cpp",
        focused_sources / "vktApiCopiesAndBlittingTests.cpp")
    write_focused_robust_buffer_source(
        cts_root / "external/vulkancts/modules/vulkan/robustness/vktRobustnessBufferAccessTests.cpp",
        focused_sources / "vktRobustnessBufferAccessTests.cpp")

    # Amber's Vulkan engine includes generated function wrappers. Regenerate them
    # from the pinned Vulkan registry so the amber objects match the CTS headers.
    amber_gen.mkdir(parents=True, exist_ok=True)
    vulkan_headers = ROOT / "third_party/vulkan-headers"
    amber_headers_link = amber_root / "third_party/vulkan-headers"
    if not amber_headers_link.exists():
        amber_headers_link.parent.mkdir(parents=True, exist_ok=True)
        amber_headers_link.symlink_to(vulkan_headers, target_is_directory=True)
    subprocess.run([sys.executable, str(amber_root / "tools/update_vk_wrappers.py"),
                    str(amber_gen), str(amber_root)], check=True)

    # SPIRV-Tools is used by the CTS shader pipeline (SPIR-V assembly/validation and
    # the optimizer used when preprocessing compiled GLSL). Build the two static
    # archives for the target once; the linker consumes them below.
    spirv_tools_build = out / "spirv-tools"
    spirv_libs = [spirv_tools_build / "source/libSPIRV-Tools.a",
                  spirv_tools_build / "source/opt/libSPIRV-Tools-opt.a"]
    spirv_stamp = spirv_tools_build / ".pins"
    spirv_pin_signature = json.dumps({
        "spirv-tools": git_head(spirv_tools_root),
        "spirv-headers": git_head(spirv_headers_root),
    }, sort_keys=True)
    spirv_stale = (
        not all(p.is_file() for p in spirv_libs)
        or not spirv_stamp.is_file()
        or spirv_stamp.read_text(encoding="utf-8").strip() != spirv_pin_signature
    )
    if spirv_stale:
        print("[build_upstream_cts] Building SPIRV-Tools for x86_64-sie-ps5...")
        cmake_env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
        subprocess.run(["cmake", "-S", str(spirv_tools_root), "-B", str(spirv_tools_build),
                        "-G", "Ninja",
                        "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "tools/ps5-cmake-toolchain.cmake"),
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DSPIRV_SKIP_TESTS=ON",
                        "-DSPIRV_SKIP_EXECUTABLES=ON",
                        "-DSPIRV_WERROR=OFF",
                        "-DSPIRV-Headers_SOURCE_DIR=" + str(spirv_headers_root)],
                       check=True, env=cmake_env)
        subprocess.run(["cmake", "--build", str(spirv_tools_build),
                        "--target", "SPIRV-Tools-static", "SPIRV-Tools-opt",
                        "--clean-first",
                        "-j", str(os.cpu_count())],
                       check=True, env=cmake_env)
        spirv_stamp.parent.mkdir(parents=True, exist_ok=True)
        spirv_stamp.write_text(spirv_pin_signature + "\n", encoding="utf-8")

    # 1. Restage the ps5vk SDK from the current sources. The payload links these
    # archives, so a driver fix must never be skipped just because archives with
    # the right names already exist: build_sdk.py recompiles them and re-runs the
    # public SDK consumer validation.
    print("[build_upstream_cts] Restaging ps5vk SDK archives from current sources...")
    subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], check=True)
    libps5vk = ROOT / "dist-sdk/lib/libps5vk.a"
    libpsbc = ROOT / "dist-sdk/lib/libpsbc.a"
    for lib in (libps5vk, libpsbc):
        if not lib.is_file():
            raise SystemExit(f"SDK archive missing after staging: {lib}")
    sdk_lib_hashes = {lib.name: sha256_file(lib) for lib in (libps5vk, libpsbc)}

    # Pinned inputs participate in every object fingerprint: changing the CTS or
    # dependency revision invalidates cached objects instead of silently reusing
    # code compiled against the previous revision.
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk),
               PATH=f"{sdk}/bin:{os.environ.get('PATH', '')}")
    env["PS5VK_BUILD_SIGNATURE"] = hashlib.sha256(json.dumps({
        "cts": git_head(cts_root),
        "glslang": git_head(glslang_root),
        "spirv-tools": git_head(spirv_tools_root),
        "spirv-headers": git_head(spirv_headers_root),
        "amber": git_head(amber_root),
    }, sort_keys=True).encode()).hexdigest()

    clang_sh = foundation / "tooling/prospero-clang18"
    linker = sdk / "bin/prospero-lld"

    # Base flags
    c_flags = [
        "sh", str(clang_sh), "-c", "-O2",
        "-DDE_OS=DE_OS_UNIX", "-DDE_COMPILER=DE_COMPILER_CLANG",
        "-DDE_CPU=DE_CPU_X86_64", "-DDE_PTR_SIZE=8",
        "-DDEQP_TARGET_NAME=\"ps5\"",
        "-D_XOPEN_SOURCE=600", "-D_GNU_SOURCE=1",
        "-include", "malloc_np.h",
        "-DDEQP_SUPPORT_VULKAN=1", "-DCTS_USES_VULKAN=1",
        "-DDISABLE_SHADERCACHE_IPC=1", "-DENABLE_HLSL=0",
        "-ffunction-sections", "-fdata-sections",
        "-I" + str(ROOT / "cts/upstream"),
        "-I" + str(focused_sources),
        "-I" + str(ROOT / "cts/upstream/glslang"),
        "-I" + str(ROOT / "third_party/vulkan-headers/include"),
        "-I" + str(cts_root / "framework/delibs/debase"),
        "-I" + str(cts_root / "framework/delibs/decpp"),
        "-I" + str(cts_root / "framework/delibs/depool"),
        "-I" + str(cts_root / "framework/delibs/dethread"),
        "-I" + str(cts_root / "framework/delibs/deutil"),
        "-I" + str(cts_root / "framework/qphelper"),
        "-I" + str(cts_root / "framework/common"),
        "-I" + str(cts_root / "framework/xexml"),
        "-I" + str(cts_root / "framework/referencerenderer"),
        "-I" + str(cts_root / "framework/opengl"),
        "-I" + str(cts_root / "framework/opengl/wrapper"),
        "-I" + str(cts_root / "external/vulkancts/framework/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/framework/vulkan/generated/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/api"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/amber"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/synchronization"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/memory"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/compute"),
        "-I" + str(cts_root / "external/spirv-tools/src/include"),
        "-I" + str(cts_root / "external/spirv-headers/src/include"),
        "-I" + str(glslang_root),
        "-I" + str(glslang_root / "glslang/Public"),
        "-I" + str(glslang_root / "glslang/Include"),
        "-I" + str(amber_root / "include"),
        "-I" + str(amber_root),
        "-I" + str(amber_gen),
        "-DAMBER_ENABLE_CLSPV=0", "-DAMBER_ENABLE_SHADERC=0", "-DAMBER_ENABLE_SPIRV_TOOLS=0",
        "-I" + str(logger),
        "-I" + str(gears / "include"),
        "-I" + str(ROOT / "src"),
    ]

    cxx_flags = [
        "sh", str(clang_sh), "-c", "-O2",
        "-std=c++17", "-fexceptions", "-frtti",
        "-DDE_OS=DE_OS_UNIX", "-DDE_COMPILER=DE_COMPILER_CLANG",
        "-DDE_CPU=DE_CPU_X86_64", "-DDE_PTR_SIZE=8",
        "-DDEQP_TARGET_NAME=\"ps5\"",
        "-D_XOPEN_SOURCE=600", "-D_GNU_SOURCE=1",
        "-DDEQP_SUPPORT_VULKAN=1", "-DCTS_USES_VULKAN=1",
        "-DDISABLE_SHADERCACHE_IPC=1", "-DENABLE_HLSL=0",
        "-ffunction-sections", "-fdata-sections",
        "-I" + str(ROOT / "cts/upstream"),
        "-I" + str(focused_sources),
        "-I" + str(ROOT / "cts/upstream/glslang"),
        "-I" + str(ROOT / "third_party/vulkan-headers/include"),
        "-I" + str(cts_root / "framework/delibs/debase"),
        "-I" + str(cts_root / "framework/delibs/decpp"),
        "-I" + str(cts_root / "framework/delibs/depool"),
        "-I" + str(cts_root / "framework/delibs/dethread"),
        "-I" + str(cts_root / "framework/delibs/deutil"),
        "-I" + str(cts_root / "framework/qphelper"),
        "-I" + str(cts_root / "framework/common"),
        "-I" + str(cts_root / "framework/xexml"),
        "-I" + str(cts_root / "framework/referencerenderer"),
        "-I" + str(cts_root / "framework/opengl"),
        "-I" + str(cts_root / "framework/opengl/wrapper"),
        "-I" + str(cts_root / "external/vulkancts/framework/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/framework/vulkan/generated/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/api"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/amber"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/synchronization"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/memory"),
        "-I" + str(cts_root / "external/vulkancts/modules/vulkan/compute"),
        "-I" + str(cts_root / "external/spirv-tools/src/include"),
        "-I" + str(cts_root / "external/spirv-headers/src/include"),
        "-I" + str(glslang_root),
        "-I" + str(glslang_root / "glslang/Public"),
        "-I" + str(glslang_root / "glslang/Include"),
        "-I" + str(amber_root / "include"),
        "-I" + str(amber_root),
        "-I" + str(amber_gen),
        "-DAMBER_ENABLE_CLSPV=0", "-DAMBER_ENABLE_SHADERC=0", "-DAMBER_ENABLE_SPIRV_TOOLS=0",
        "-I" + str(logger),
        "-I" + str(gears / "include"),
        "-I" + str(ROOT / "src"),
    ]

    # vktTestPackage.cpp (which provides vkt::BaseTestPackage) includes the umbrella
    # headers of every vulkan module, so the whole module include tree must be visible.
    # Only the reachable functions survive the link (--gc-sections below).
    modules_root = cts_root / "external/vulkancts/modules/vulkan"
    for dirpath in sorted(p for p in modules_root.rglob("*") if p.is_dir()):
        c_flags.append("-I" + str(dirpath))
        cxx_flags.append("-I" + str(dirpath))

    compile_tasks = []

    # 1. delibs C sources
    delibs_c = [
        cts_root / "framework/delibs/debase/deDefs.c",
        cts_root / "framework/delibs/debase/deFloat16.c",
        cts_root / "framework/delibs/debase/deInt32.c",
        cts_root / "framework/delibs/debase/deMath.c",
        cts_root / "framework/delibs/debase/deMemory.c",
        cts_root / "framework/delibs/debase/deRandom.c",
        cts_root / "framework/delibs/debase/deSha1.c",
        cts_root / "framework/delibs/debase/deString.c",
        cts_root / "framework/delibs/depool/dePoolArray.c",
        cts_root / "framework/delibs/depool/dePoolHeap.c",
        cts_root / "framework/delibs/depool/dePoolMultiSet.c",
        cts_root / "framework/delibs/depool/dePoolSet.c",
        cts_root / "framework/delibs/depool/dePoolStringBuilder.c",
        cts_root / "framework/delibs/depool/deMemPool.c",
        cts_root / "framework/delibs/dethread/deAtomic.c",
        cts_root / "framework/delibs/dethread/deSingleton.c",
        cts_root / "framework/delibs/dethread/unix/deMutexUnix.c",
        cts_root / "framework/delibs/dethread/unix/deSemaphoreUnix.c",
        cts_root / "framework/delibs/dethread/unix/deThreadUnix.c",
        cts_root / "framework/delibs/dethread/unix/deThreadLocalUnix.c",
        cts_root / "framework/delibs/deutil/deClock.c",
        cts_root / "framework/delibs/deutil/deCommandLine.c",
        cts_root / "framework/delibs/deutil/deDynamicLibrary.c",
        cts_root / "framework/delibs/deutil/deFile.c",
        cts_root / "framework/delibs/deutil/deProcess.c",
        cts_root / "framework/delibs/deutil/deSocket.c",
    ]
    for src in delibs_c:
        obj = obj_dir / "delibs" / "c" / (src.stem + ".o")
        compile_tasks.append((c_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 2. delibs C++ sources
    delibs_cpp = [
        cts_root / "framework/delibs/decpp/deArrayBuffer.cpp",
        cts_root / "framework/delibs/decpp/deBlockBuffer.cpp",
        cts_root / "framework/delibs/decpp/deCommandLine.cpp",
        cts_root / "framework/delibs/decpp/deDefs.cpp",
        cts_root / "framework/delibs/decpp/deDirectoryIterator.cpp",
        cts_root / "framework/delibs/decpp/deDynamicLibrary.cpp",
        cts_root / "framework/delibs/decpp/deFilePath.cpp",
        cts_root / "framework/delibs/decpp/deMutex.cpp",
        cts_root / "framework/delibs/decpp/deProcess.cpp",
        cts_root / "framework/delibs/decpp/deRandom.cpp",
        cts_root / "framework/delibs/decpp/deRingBuffer.cpp",
        cts_root / "framework/delibs/decpp/deSemaphore.cpp",
        cts_root / "framework/delibs/decpp/deSharedPtr.cpp",
        cts_root / "framework/delibs/decpp/deSocket.cpp",
        cts_root / "framework/delibs/decpp/deStringUtil.cpp",
        cts_root / "framework/delibs/decpp/deThread.cpp",
        cts_root / "framework/delibs/decpp/deThreadSafeRingBuffer.cpp",
        cts_root / "framework/delibs/decpp/deUniquePtr.cpp",
    ]
    for src in delibs_cpp:
        obj = obj_dir / "delibs" / "cxx" / (src.stem + ".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 3. qphelper C sources
    qphelper_c = [
        cts_root / "framework/qphelper/qpCrashHandler.c",
        cts_root / "framework/qphelper/qpDebugOut.c",
        cts_root / "framework/qphelper/qpInfo.c",
        cts_root / "framework/qphelper/qpTestLog.c",
        cts_root / "framework/qphelper/qpWatchDog.c",
        cts_root / "framework/qphelper/qpXmlWriter.c",
    ]
    for src in qphelper_c:
        obj = obj_dir / "qphelper" / (src.stem + ".o")
        compile_tasks.append((c_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 4. tcu C++ sources (whole framework/common, as upstream builds it).
    # tcuImageIO needs libpng, which this focused build does not vendor and the
    # selected cases do not exercise (reference surfaces are produced in memory).
    tcu_cpp = [p for p in sorted((cts_root / "framework/common").glob("*.cpp"))
               if p.name != "tcuImageIO.cpp"] + \
              [cts_root / "framework/xexml/xeXMLParser.cpp"]
    for src in tcu_cpp:
        obj = (obj_dir / "tcu" / src.relative_to(cts_root / "framework")).with_suffix(".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 5. referencerenderer C++ sources
    rr_cpp = sorted((cts_root / "framework/referencerenderer").glob("*.cpp"))
    for src in rr_cpp:
        obj = (obj_dir / "rr" / src.relative_to(cts_root / "framework")).with_suffix(".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 5b. glu / OpenGL wrapper sources used by the Vulkan shader helpers
    # gluRenderConfig needs the generated EGL wrapper (eglw*.hpp), which upstream
    # emits from the EGL registry and which no selected case needs.
    gl_cpp = [p for p in sorted((cts_root / "framework/opengl").glob("*.cpp"))
              if p.name != "gluRenderConfig.cpp"] + \
             sorted((cts_root / "framework/opengl/wrapper").glob("*.cpp"))
    for src in gl_cpp:
        obj = (obj_dir / "gl" / src.relative_to(cts_root / "framework" / "opengl")).with_suffix(".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 6. glslang sources
    glslang_sources = sorted((glslang_root / "glslang/MachineIndependent").glob("*.cpp")) + \
                      sorted((glslang_root / "glslang/MachineIndependent/preprocessor").glob("*.cpp")) + \
                      sorted((glslang_root / "glslang/GenericCodeGen").glob("*.cpp")) + \
                      sorted((glslang_root / "glslang/CInterface").glob("*.cpp")) + \
                      sorted((glslang_root / "SPIRV").glob("*.cpp")) + \
                      sorted((glslang_root / "glslang/HLSL").glob("*.cpp")) + \
                      [glslang_root / "glslang/OSDependent/Unix/ossource.cpp"]
    for src in glslang_sources:
        obj = (obj_dir / "glslang" / src.relative_to(glslang_root)).with_suffix(".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 6b. Amber (vendored at the pin declared by the CTS source manifest). The Vulkan
    # test module references cts_amber::createAmberTestCase from vktAmberTestCaseUtil.cpp,
    # which links against libamber; the selected cases do not use amber themselves.
    amber_core = [
        amber_root / "src/amber.cc",
        amber_root / "src/amberscript/parser.cc",
        amber_root / "src/buffer.cc",
        amber_root / "src/command.cc",
        amber_root / "src/command_data.cc",
        amber_root / "src/descriptor_set_and_binding_parser.cc",
        amber_root / "src/engine.cc",
        amber_root / "src/executor.cc",
        amber_root / "src/float16_helper.cc",
        amber_root / "src/format.cc",
        amber_root / "src/parser.cc",
        amber_root / "src/pipeline.cc",
        amber_root / "src/pipeline_data.cc",
        amber_root / "src/recipe.cc",
        amber_root / "src/result.cc",
        amber_root / "src/sampler.cc",
        amber_root / "src/script.cc",
        amber_root / "src/shader.cc",
        amber_root / "src/shader_compiler.cc",
        amber_root / "src/sleep.cc",
        amber_root / "src/tokenizer.cc",
        amber_root / "src/type.cc",
        amber_root / "src/type_parser.cc",
        amber_root / "src/value.cc",
        amber_root / "src/verifier.cc",
        amber_root / "src/virtual_file_store.cc",
        amber_root / "src/vkscript/command_parser.cc",
        amber_root / "src/vkscript/datum_type_parser.cc",
        amber_root / "src/vkscript/parser.cc",
        amber_root / "src/vkscript/section_parser.cc",
        amber_root / "src/vulkan_engine_config.cc",
    ]
    amber_vulkan = [
        amber_root / "src/vulkan/buffer_backed_descriptor.cc",
        amber_root / "src/vulkan/buffer_descriptor.cc",
        amber_root / "src/vulkan/command_buffer.cc",
        amber_root / "src/vulkan/command_pool.cc",
        amber_root / "src/vulkan/compute_pipeline.cc",
        amber_root / "src/vulkan/descriptor.cc",
        amber_root / "src/vulkan/device.cc",
        amber_root / "src/vulkan/engine_vulkan.cc",
        amber_root / "src/vulkan/frame_buffer.cc",
        amber_root / "src/vulkan/graphics_pipeline.cc",
        amber_root / "src/vulkan/image_descriptor.cc",
        amber_root / "src/vulkan/index_buffer.cc",
        amber_root / "src/vulkan/pipeline.cc",
        amber_root / "src/vulkan/push_constant.cc",
        amber_root / "src/vulkan/resource.cc",
        amber_root / "src/vulkan/sampler.cc",
        amber_root / "src/vulkan/sampler_descriptor.cc",
        amber_root / "src/vulkan/transfer_buffer.cc",
        amber_root / "src/vulkan/transfer_image.cc",
        amber_root / "src/vulkan/vertex_buffer.cc",
    ]
    for src in amber_core:
        obj = (obj_dir / "amber" / "core" / src.relative_to(amber_root / "src")).with_suffix(".o")
        compile_tasks.append((cxx_flags + ["-w", str(src), "-o", str(obj)], src, obj, env))
    for src in amber_vulkan:
        obj = (obj_dir / "amber" / "vulkan" / src.relative_to(amber_root / "src" / "vulkan")).with_suffix(".o")
        compile_tasks.append((cxx_flags + ["-w", str(src), "-o", str(obj)], src, obj, env))

    # 7. vkutil C++ sources
    # Upstream picks vkRenderDocUtil.cpp or vkNoRenderDocUtil.cpp depending on whether
    # the RenderDoc app header is available; this native image has neither the header
    # nor an attachable debugger, so the no-op implementation is used.
    vkutil_cpp = [p for p in sorted((cts_root / "external/vulkancts/framework/vulkan").glob("*.cpp"))
                  if p.name != "vkRenderDocUtil.cpp"]
    for src in vkutil_cpp:
        obj = (obj_dir / "vkutil" / src.relative_to(cts_root / "external/vulkancts/framework/vulkan")).with_suffix(".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 8. test modules
    test_cpp = [
        cts_root / "external/vulkancts/modules/vulkan/vktTestPackage.cpp",
        cts_root / "external/vulkancts/modules/vulkan/vktTestCase.cpp",
        cts_root / "external/vulkancts/modules/vulkan/vktTestCaseUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/vktCustomInstancesDevices.cpp",
        cts_root / "external/vulkancts/modules/vulkan/amber/vktAmberTestCase.cpp",
        cts_root / "external/vulkancts/modules/vulkan/amber/vktAmberTestCaseUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/amber/vktAmberHelper.cpp",
        # vk::createShader lives in the shader_object module but is referenced by the
        # framework's compute pipeline wrapper, so the module util is linked in.
        cts_root / "external/vulkancts/modules/vulkan/shader_object/vktShaderObjectCreateUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiFeatureInfo.cpp",
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiSmokeTests.cpp",
        # Resource-focused modules: buffer views (uniform texel buffers) and the
        # binding-model shader-access family (multi-set binding of buffers).
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiBufferViewAccessTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiBufferAndImageAllocationUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiPipelineTests.cpp",
        focused_sources / "vktApiCopiesAndBlittingTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiFillBufferTests.cpp",
        # Pipeline cache module: only the compute case is selected (the graphics
        # cache cases need a D16_UNORM depth attachment this profile lacks), but
        # the module registers both families. Its helper definitions already
        # exist in the linked synchronization util, so vktPipelineMakeUtil.cpp is
        # deliberately not added a second time (it would duplicate symbols).
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineCacheTests.cpp",
        # vktImageTestsUtil provides the format-qualifier and packed-type helpers
        # the buffer-view access tests use to build their compute shader.
        cts_root / "external/vulkancts/modules/vulkan/image/vktImageTestsUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/binding_model/vktBindingShaderAccessTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationBasicEventTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationBasicFenceTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationBasicSemaphoreTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/memory/vktMemoryMappingTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/compute/vktComputeBasicComputeShaderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/compute/vktComputeIndirectComputeDispatchTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/compute/vktComputeTestsUtil.cpp",
        # Original dynamic-state compute/transfer non-interference module.  It
        # is compiled directly from the pinned checkout; no body or oracle is
        # copied into the integration.
        cts_root / "external/vulkancts/modules/vulkan/dynamic_state/vktDynamicStateComputeTests.cpp",
        # Original multiview module. The package registers the module's own
        # factory, the view-mask support gate and the per-view layer oracle are
        # the upstream ones, and cases.txt selects only the 48 audited legacy
        # render-pass leaves this device can run: clear_attachments, masks,
        # index.vertex_shader and index.fragment_shader.
        cts_root / "external/vulkancts/modules/vulkan/multiview/vktMultiViewTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/multiview/vktMultiViewRenderPassUtil.cpp",
        # Original rasterization module: the only upstream family that exercises
        # LINE and POINT polygon rasterization on a colour attachment, which is
        # what fillModeNonSolid needs an oracle for. Its own factory is
        # registered in package_ps5.cpp and its own support checks and oracles
        # are untouched; cases.txt selects only the culling leaves this profile
        # can run. All six translation units of the module are needed because
        # createTests() calls the sub-factories they define.
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktRasterizationTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktRasterizationProvokingVertexTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktRasterizationDepthBiasControlTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktRasterizationFragShaderSideEffectsTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktRasterizationOrderAttachmentAccessTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/rasterization/vktShaderTileImageTests.cpp",
        # Original user-defined clip/cull distance module. The package registers
        # the module's own factory; none of its leaves is selected for strict
        # acceptance, because the feature flag that gates the whole family also
        # gates the fragment-shader-read and dynamic-index variants this profile
        # refuses. The measured static-index vertex-only subset stays diagnostic
        # in cts/upstream/manifest.json. The module compiles its shaders through
        # its own runtime glslang path, so no dataset binary is added.
        cts_root / "external/vulkancts/modules/vulkan/clipping/vktClippingTests.cpp",
        # The shared draw utility the clipping module renders and reads back
        # through (vkt::drawutil::VulkanDrawContext and its pipeline state).
        cts_root / "external/vulkancts/modules/vulkan/util/vktDrawUtil.cpp",
        # Original geometry shader module, registered whole under its own name.
        # The module's own factory builds the leaves and its shaders go through
        # its runtime glslang path; the reference images it compares against are
        # embedded at build time (see tools/embed_cts_reference_images.py) and
        # served by cts/upstream/image_io_ps5.cpp. cases.txt selects only the
        # leaves whose input primitive, built-ins and envelope this profile
        # compiles and has measured; the rest stay diagnostics in
        # cts/upstream/manifest.json with the gate that refuses them.
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryBasicClass.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryBasicGeometryShaderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryEmitGeometryShaderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryInputGeometryShaderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryInstancedRenderingTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryLayeredRenderingTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryTestsUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/geometry/vktGeometryVaryingGeometryShaderTests.cpp",
        # Original Vulkan 1.0 robustBufferAccess bodies and oracles, with only
        # registration pruned to compute/scalar_copy/R32_UINT.
        focused_sources / "vktRobustnessBufferAccessTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/robustness/vktRobustnessUtil.cpp",
        # Draw-parameter module: the original upstream bodies and their
        # reference-rasterizer image oracle, registered under the render-pass
        # group parameters. The module's base class carries the pipeline,
        # vertex-input and render-pass setup the oracle compares against.
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawShaderDrawParametersTests.cpp",
        # Indirect-draw module: original upstream multi-command, firstInstance
        # and instanced bodies over the same base class and image oracle.
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawIndirectTest.cpp",
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawBaseClass.cpp",
        # The base class builds its buffers, images and render pass through the
        # module's own create-info and object helpers.
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawCreateInfoUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawImageObjectUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/draw/vktDrawBufferObjectUtil.cpp",
        # Genuine upstream push-constant factory. The focused package registers
        # the complete group and cases.txt selects only the audited compute leaf.
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelinePushConstantTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineClearUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineImageUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineVertexUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineReferenceRenderer.cpp",
        cts_root / "external/vulkancts/modules/vulkan/pipeline/vktPipelineMakeUtil.cpp",
        # VK_KHR_8bit_storage / VK_KHR_16bit_storage focused groups. The build
        # generates registration-pruned copies from the pinned modules; selected
        # shader bodies, support checks and oracles remain upstream. Registering
        # the modules' complete trees exceeds the PS5 title heap before the
        # case-list filter runs.
        ROOT / "cts/upstream/storage8_focus.cpp",
        ROOT / "cts/upstream/storage16_focus.cpp",
        cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/vktSpvAsmComputeShaderCase.cpp",
        cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/vktSpvAsmComputeShaderTestUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/vktSpvAsmGraphicsShaderTestUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/vktSpvAsmUtils.cpp",
        cts_root / "external/vulkancts/modules/vulkan/spirv_assembly/vktSpvAsmWorkgroupMemoryTests.cpp",
    ]
    for src in test_cpp:
        obj = obj_dir / "modules" / (src.stem + ".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 9. PS5 custom files
    # The reference images the packaged modules compare against are decoded from
    # the pinned upstream assets at build time, because this integration carries
    # no PNG decoder; cts/upstream/image_io_ps5.cpp serves the bytes back through
    # the same tcu::ImageIO::loadPNG the modules call.
    reference_images = obj_dir.parent / "geometry_reference_images.cpp"
    subprocess.run([sys.executable, str(ROOT / "tools/embed_cts_reference_images.py"),
                    "--source", str(cts_root / "external/vulkancts/data/vulkan/data/geometry"),
                    "--out", str(reference_images)], check=True, cwd=ROOT)
    ps5_custom = [
        ROOT / "cts/upstream/thread_atexit_ps5.cpp",
        ROOT / "cts/upstream/dladdr_ps5.cpp",
        ROOT / "cts/upstream/platform_ps5.cpp",
        ROOT / "cts/upstream/log_sink_ps5.cpp",
        ROOT / "cts/upstream/package_ps5.cpp",
        ROOT / "cts/upstream/main_ps5.cpp",
        ROOT / "cts/upstream/image_io_ps5.cpp",
        reference_images,
    ]
    for src in ps5_custom:
        obj = obj_dir / "ps5" / (src.stem + ".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 10. Logging client
    log_c = [
        logger / "ps5log.c",
        logger / "ps5log_ps5_net.c",
    ]
    for src in log_c:
        obj = obj_dir / "ps5" / (src.stem + ".o")
        extra = ["-include", str(logger / "ps5log_ps5_net.h")] if src.stem == "ps5log" else []
        compile_tasks.append((c_flags + extra + [str(src), "-o", str(obj)], src, obj, env))

    print(f"[build_upstream_cts] Compiling {len(compile_tasks)} translation units with {os.cpu_count()} threads...")
    all_objects = [task[2] for task in compile_tasks]

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as executor:
        futures = {executor.submit(compile_worker, task): task[1] for task in compile_tasks}
        failed = 0
        for f in concurrent.futures.as_completed(futures):
            src_name, ok, err = f.result()
            if not ok:
                print(f"FAILED: {src_name}\n{err}", file=sys.stderr)
                failed += 1

    if failed > 0:
        raise SystemExit(f"{failed} compilation tasks failed.")

    print(f"[build_upstream_cts] All {len(all_objects)} objects compiled successfully.")

    # 11. Stubs and CRT
    crt = out / "crt.o"
    if not crt.is_file():
        subprocess.run(["sh", str(clang_sh), "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti",
                        "-c", str(native / "app_crt.cpp"), "-o", str(crt)], check=True, env=env)

    stub = out / "stubs/libSceAgc.so"
    if not stub.is_file():
        subprocess.run(["sh", str(clang_sh), "-fPIC", "-I" + str(gears / "include"), "-c",
                        str(gears / "native/stubs/libSceAgc.c"), "-o", str(out / "agc.o")], check=True, env=env)
        subprocess.run(["sh", str(clang_sh), "-fPIC", "-c", str(ROOT / "native/index_import_stub.c"),
                        "-o", str(out / "agc_index.o")], check=True, env=env)
        subprocess.run([str(linker), "--shared", "-soname", "libSceAgc.prx", "-o", str(stub),
                        str(out / "agc.o"), str(out / "agc_index.o")], check=True)

    driver = out / "stubs/libSceAgcDriver.so"
    if not driver.is_file():
        subprocess.run(["sh", str(clang_sh), "-fPIC", "-I" + str(gears / "include"), "-c",
                        str(gears / "native/stubs/libSceAgcDriver.c"), "-o", str(out / "driver.o")], check=True, env=env)
        subprocess.run([str(linker), "--shared", "-soname", "libSceAgcDriver.prx", "-o", str(driver),
                        str(out / "driver.o")], check=True)

    # 12. Link PIE ELF with map file
    print("[build_upstream_cts] Linking pie.elf and generating upstream_cts.map...")
    pie_ld = (ROOT / "native/ps5-pie.ld") if (ROOT / "native/ps5-pie.ld").is_file() else (native / "ps5-pie.ld")
    syms_map = (ROOT / "native/app-symbols.map") if (ROOT / "native/app-symbols.map").is_file() else (native / "app-symbols.map")
    map_file = out / "upstream_cts.map"

    extra_libs = [
        str(libps5vk),
        str(libpsbc),
        str(spirv_libs[1]),
        str(spirv_libs[0]),
        str(sdk / "target/lib/libc++.a"),
        str(sdk / "target/lib/libc++abi.a"),
        str(sdk / "target/lib/libunwind.a"),
        str(sdk / "target/lib/libpthread.a"),
        str(sdk / "target/lib/libc.a"),
    ]

    link_cmd = [
        str(linker),
        "-L" + str(sdk / "target/lib"),
        "-T", str(pie_ld),
        "--eh-frame-hdr",
        "--version-script", str(syms_map),
        "--wrap=fopen",
        "--wrap=fprintf",
        "--wrap=fputs",
        "--wrap=fputc",
        "--wrap=fwrite",
        "--wrap=fseek",
        "--wrap=fflush",
        "--wrap=fclose",
        "--gc-sections",
        "-Map=" + str(map_file),
        "-e", "_start",
        "-o", str(out / "pie.elf"),
        str(crt),
        *[str(o) for o in all_objects],
        *extra_libs,
        "--as-needed",
        *[str(p) for p in sorted((sdk / "target/lib").glob("*.so"))],
        str(stub),
        str(driver)
    ]
    subprocess.run(link_cmd, check=True)

    # 13. ps5-native-tool link & sign
    print("[build_upstream_cts] Linking and signing eboot.bin...")
    subprocess.run([
        str(builder), "link",
        "--in", str(out / "pie.elf"),
        "--out", str(out / "eboot.elf"),
        "--stub-dir", str(sdk / "target/lib"),
        "--module-sdk", "0x02000009",
        "--stub", str(stub),
        "--stub", str(driver),
        "--companion-sdk", "0x08050001",
        "--file-name", "eboot.elf"
    ], check=True)

    from cts_heap_parameters import use_application_heap
    linked_elf = out / "eboot.elf"
    linked_elf.write_bytes(use_application_heap(linked_elf.read_bytes()))

    eboot_bin = dist / "eboot.bin"
    subprocess.run([
        str(builder), "self", "--sign",
        "--in", str(out / "eboot.elf"),
        "--out", str(eboot_bin),
        "--magic", "0x1D3D154F"
    ], check=True)

    # 14. Stage package metadata & support files
    param = {
        "titleId": "PPSA99994",
        "conceptId": "99994",
        "contentId": "UP9000-PPSA99994_00-PS5VKUPSTREAMCTS",
        "localizedParameters": {
            "defaultLanguage": "en-US",
            "en-US": {
                "titleName": "PS5 Vulkan Upstream CTS"
            }
        }
    }
    (dist / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", dist / "sce_module/libc.prx")
    shutil.copyfile(foundation / "sce_sys/icon0.png", dist / "sce_sys/icon0.png")

    # The packaged CTS reads its data archive from /app0 (the title directory),
    # so the selected cases that load upstream shader sources need those files
    # beside eboot.bin. Only the draw modules' sources are staged (the
    # registered factories load them by name even for unselected leaves); the
    # rest of the selection builds its shaders in code.
    for relative in DATASET_SHADER_SOURCES:
        source = cts_root / "external/vulkancts/data" / relative
        if not source.is_file():
            raise SystemExit(f"missing upstream CTS data source {source}")
        destination = dist / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)

    if (ROOT / "dev.conf").is_file():
        shutil.copyfile(ROOT / "dev.conf", dist / "dev.conf")

    # Frozen case list, taken from the committed selection manifest so that the
    # packaged list, the selection hash and the verifier can never drift apart.
    manifest_cases = [case["path"] for case in selection_manifest["cases"]]
    case_list_content = "\n".join(manifest_cases) + "\n"
    (dist / "cases.txt").write_text(case_list_content)

    selection_hash = hashlib.sha256(case_list_content.encode("utf-8")).hexdigest()
    (dist / "selection_hash.txt").write_text(selection_hash + "\n")
    (dist / "eboot_sha256.txt").write_text(sha256_file(eboot_bin) + "\n")

    # 15. Record build manifest
    try:
        clang_version = subprocess.run(
            ["sh", str(clang_sh), "--version"], capture_output=True, text=True,
            env=env, check=True).stdout.splitlines()[0].strip()
    except (OSError, subprocess.CalledProcessError, IndexError):
        clang_version = "unknown"

    build_manifest = {
        "title": "PPSA99994",
        "type": "upstream_cts_native",
        "target": "x86_64-sie-ps5",
        "toolchain": {
            "clang": clang_version,
            "linker": "prospero-lld",
            "sdk": sdk.name,
        },
        "upstream_pin": {
            "repo": "third_party/vk-gl-cts",
            "tag": cts_pin.get("tag", ""),
            # Verified against the checkout, not assumed.
            "commit": cts_state["commit"],
            "verified": True,
            "locally_modified": cts_state["dirty"],
        },
        "sdk_archives": sdk_lib_hashes,
        # Commits actually compiled into this payload. These are recorded rather
        # than assumed, because the CTS external/ manifest can lag the checkout.
        "dependency_commits": {
            "glslang": git_head(glslang_root),
            "spirv-tools": git_head(spirv_tools_root),
            "spirv-headers": git_head(spirv_headers_root),
            "amber": git_head(amber_root),
        },
        "selection_hash": selection_hash,
        "selected_cases": manifest_cases,
        "eboot_sha256": sha256_file(eboot_bin),
        "map_file": str(map_file.relative_to(ROOT)),
        "total_objects_linked": len(all_objects),
    }
    (out / "build_manifest.json").write_text(json.dumps(build_manifest, indent=2) + "\n")
    (dist / "build_manifest.json").write_text(json.dumps(build_manifest, indent=2) + "\n")

    print(f"[build_upstream_cts] Build complete!")
    print(f"  eboot.bin: {eboot_bin} ({eboot_bin.stat().st_size} bytes, sha256: {build_manifest['eboot_sha256']})")
    print(f"  map: {map_file} ({map_file.stat().st_size} bytes)")
    print(f"  selection hash: {selection_hash}")

if __name__ == "__main__":
    main()
