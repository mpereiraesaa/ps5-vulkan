# Building and testing

## Host checks

Install Python 3, Make, a C11 compiler, Git and `glslangValidator`, then prepare the pinned Vulkan
headers, compiler dependencies and run the contract suite:

```sh
make vulkan-headers
make compiler-deps
make check
make check-sanitize
```

The preparation targets fetch exact revisions. `make compiler-deps` uses the
public `mpereiraesaa/opengnm-psbc` GFX1013 fork plus pinned OpenGNM headers.
The regular host suite runs offline after these dependencies are prepared.

## Companion repositories

Native builds currently consume source-level support from `ps5-agc-gears` and
the `ps5log/1` client from `logging_server`. Keep the repositories as siblings:

```text
homebrew_ps5/
  projects/
    logging_server/
    ps5-agc-gears/
    ps5-vulkan/
```

The shared resolver also supports repository worktrees placed beside the
canonical `homebrew_ps5` directory. CI pins known companion revisions so public
pull requests do not silently change contracts underneath the build.

## Native build

Set `PS5_PAYLOAD_SDK` when the SDK is not available at the default sibling path.
Compile an owned graphics pipeline description, then pass the generated control
directory to the public native wrapper:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk/install
python3 tools/compile_graphics_control.py experiments/graphics/scene3d.pipe
make native-graphics GRAPHICS_CONTROL=build/graphics/control-REPLACE
```

The generated package remains under the ignored build/output trees. Deployment,
console credentials, telemetry configuration, captures, compiler worktrees and
proprietary artifacts are intentionally outside this repository.

## Reproducibility and evidence

For the owned runtime-compiled triangle diagnostic, reuse the generated
control directory as a negative offline-lookup reference:

```sh
PS5VK_USE_SDK=1 make native-runtime-graphics GRAPHICS_CONTROL=build/graphics/control-REPLACE
```

This builds the SDK and links its native archive into the diagnostic. Vertex
and fragment ISA are compiled on the console; only GLSL-to-SPIR-V runs on the
host. Set `GLSLANG=/path/to/glslangValidator` if needed. After the bounded test
retires resources, it waits for system Close Game; it is not the continuous
textured demo. Omit `PS5VK_USE_SDK=1` to compile backend objects directly.

`python3 tools/build_sdk.py` independently stages public headers, native and
host archives, the native compiler dependency and link-time import facades in
`dist-sdk/`. It requires the prepared native PSBC archive for native builds.
The generated SDK README documents linking and supported profiles. Native
consumer checks cross-link only; host execution is a mock-backend test.

Use `tools/verify_graphics_runtime.py LOG --artifact MANIFEST` to check the
runtime triangle's TCP receipt, cold/warm cache, GPU readbacks, presentation
and resource retirement. Verified deployment hashes and successful OS close
must be established separately; the log does not attest its executable.

For the bounded multi-set compute acceptance in the public consumer, use
`tools/verify_consumer_resource_abi.py LOG --artifact MANIFEST`. The artifact
manifest must identify the deployed SELF and profile; the verifier requires
three sets, two storage buffers, one uniform buffer, one uniform texel buffer,
two non-default scalar specialization values, one pushed 32-bit word, 64 exact
results, 128 intact guard words, complete queue witnesses and a clean TCP
`BYE`. System Close Game remains a separate lifecycle check.

Host checks validate API state machines, encoder contracts, resource ownership,
negative paths and generated-program invariants. A successful host build alone
does not prove GPU execution. Native evidence additionally requires exact
artifact identity, structured `ps5log/1` telemetry, GPU completion/readback and
VideoOut ownership; screenshots are only supporting visual evidence.

## Vulkan API contract tests (modeled after CTS)

A pinned set of 26 synthetic Vulkan API contract tests modeled after the Khronos
`VK-GL-CTS` mustpass selection is maintained in `cts/case_list.txt` and documented
in `cts/gap_matrix.md`. It remains separate from the focused genuine upstream
integration documented in [UPSTREAM_CTS.md](UPSTREAM_CTS.md).

```sh
# Host contract test execution (part of make check)
./build/tests/test_cts_host --json
./build/tests/test_cts_host --tap

# Contract runner and gap matrix unit tests
python3 -m unittest discover -s tests -p "test_cts*.py"

# Package native contract test title for console execution
python3 tools/build_cts_native.py
```

## Independent native SDK consumer

The standalone native consumer demonstrates SDK usage using strictly public headers
(`<ps5vk/ps5vk.h>`, `<ps5vk/ps5vk_present.h>`):

```sh
# Stage public SDK
python3 tools/build_sdk.py

# Build native consumer, verify zero private includes/symbols, and package
python3 tools/build_consumer.py

# For continuous rendering demonstration (60 FPS looped stream)
python3 tools/build_consumer.py --continuous

# Consumer header and symbol isolation tests
python3 -m unittest tests/test_consumer_isolation.py

# Strict verifier regression tests for the multi-set resource witness
python3 -m unittest tests/test_consumer_resource_abi.py
```
