# CMake cross-compilation description for the PlayStation 5 payload SDK.
#
# Used to build third-party CMake libraries (currently SPIRV-Tools) for the same
# x86_64-sie-ps5 target as the rest of ps5vk. The SDK is located through
# PS5_PAYLOAD_SDK, which tools/build_upstream_cts.py already requires.
#
# Compilation only: the produced static archives are linked into the payload by
# tools/build_upstream_cts.py, so no CMake linker probing is needed.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PS5_SDK "$ENV{PS5_PAYLOAD_SDK}")
if(NOT PS5_SDK)
  message(FATAL_ERROR "PS5_PAYLOAD_SDK is not set")
endif()

set(CMAKE_C_COMPILER "${PS5_SDK}/bin/clang")
set(CMAKE_CXX_COMPILER "${PS5_SDK}/bin/clang++")
set(CMAKE_AR "${PS5_SDK}/bin/llvm-ar")
set(CMAKE_RANLIB "${PS5_SDK}/bin/llvm-ranlib")

# The SDK libc/libc++ live under <sdk>/target, so probe there instead of the host.
set(_ps5_target_flags "-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -isysroot ${PS5_SDK} -isystem ${PS5_SDK}/target/include/c++/v1 -isystem ${PS5_SDK}/target/include -fno-stack-protector -fno-plt -femulated-tls")
set(CMAKE_C_FLAGS_INIT "${_ps5_target_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_ps5_target_flags}")

# Only compile static libraries from these third-party sources; skip link probes.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

set(CMAKE_FIND_ROOT_PATH "${PS5_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
