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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lab import lab_root

SELECTION_MANIFEST = ROOT / "cts/upstream/manifest.json"

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


def compile_worker(args):
    cmd, src, obj, env = args
    obj.parent.mkdir(parents=True, exist_ok=True)
    # Check if up to date
    if obj.is_file() and obj.stat().st_mtime >= src.stat().st_mtime:
        return src.name, True, None
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if res.returncode != 0:
            return src.name, False, res.stderr + "\n" + res.stdout
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
    if not all(p.is_file() for p in spirv_libs):
        print("[build_upstream_cts] Building SPIRV-Tools for x86_64-sie-ps5 (one-off)...")
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
                        "-j", str(os.cpu_count())],
                       check=True, env=cmake_env)

    # 1. Build SDK archives if missing
    libps5vk = ROOT / "dist-sdk/lib/libps5vk.a"
    libpsbc = ROOT / "dist-sdk/lib/libpsbc.a"
    if not (libps5vk.is_file() and libpsbc.is_file()):
        print("[build_upstream_cts] Staging ps5vk SDK archives...")
        subprocess.run([sys.executable, str(ROOT / "tools/build_sdk.py")], check=True)

    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk),
               PATH=f"{sdk}/bin:{os.environ.get('PATH', '')}")

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
        cts_root / "external/vulkancts/modules/vulkan/api/vktApiSmokeTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationBasicFenceTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/synchronization/vktSynchronizationUtil.cpp",
        cts_root / "external/vulkancts/modules/vulkan/memory/vktMemoryMappingTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/compute/vktComputeBasicComputeShaderTests.cpp",
        cts_root / "external/vulkancts/modules/vulkan/compute/vktComputeTestsUtil.cpp",
    ]
    for src in test_cpp:
        obj = obj_dir / "modules" / (src.stem + ".o")
        compile_tasks.append((cxx_flags + [str(src), "-o", str(obj)], src, obj, env))

    # 9. PS5 custom files
    ps5_custom = [
        ROOT / "cts/upstream/thread_atexit_ps5.cpp",
        ROOT / "cts/upstream/dladdr_ps5.cpp",
        ROOT / "cts/upstream/platform_ps5.cpp",
        ROOT / "cts/upstream/log_sink_ps5.cpp",
        ROOT / "cts/upstream/package_ps5.cpp",
        ROOT / "cts/upstream/main_ps5.cpp",
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
            "tag": "vulkan-cts-1.3.8.4",
            "commit": "a0270c1897597e6c77679870e10415398a13001c"
        },
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
