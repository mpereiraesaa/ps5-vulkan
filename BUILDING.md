# Building and testing

## Host checks

Install Python 3, Make, a C11 compiler and Git, then prepare the pinned Vulkan
headers and run the contract suite:

```sh
make vulkan-headers
make check
make check-sanitize
```

`make vulkan-headers` is the only header-fetch step. The regular host suite is
designed to run offline after dependencies are prepared.

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

The host suite uses the same relative layout. CI pins known companion revisions
so public pull requests do not silently change contracts underneath the build.

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

Host checks validate API state machines, encoder contracts, resource ownership,
negative paths and generated-program invariants. A successful host build alone
does not prove GPU execution. Native evidence additionally requires exact
artifact identity, structured `ps5log/1` telemetry, GPU completion/readback and
VideoOut ownership; screenshots are only supporting visual evidence.
