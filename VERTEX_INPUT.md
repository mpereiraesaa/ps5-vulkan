# Vertex input implementation

The runtime currently advertises one vertex binding. Multiple-binding fetch
preparation is implemented and host-tested, but is not enabled in production.
The remaining prerequisite is compiler metadata describing the optimized
vertex-buffer descriptor usage, followed by PS5 validation.

## Descriptor order

The pinned PSBC compiler lowers vertex inputs through RADV:
`src/amd/vulkan/nir/radv_nir_lower_vs_inputs.c` computes the descriptor index as
the population count of the used-binding mask below the requested binding.
For bindings 3 and 15, the native SRDs therefore occupy slots 0 and 1. If
optimization removes binding 3, binding 15 occupies slot 0 instead.

`libpsbc/psbc_compile.c` currently exports the vertex-table user SGPR through
`PsbcShaderMetadata`, but does not export `vs.vb_desc_usage_mask` or
`vs.use_per_attribute_vb_descs`. Inferring this mask from pipeline declarations
would be incorrect when shader optimization removes inputs. Per-attribute
descriptors also require a different mapping and must be identified explicitly.

## Implemented preparation

`src/vertex_fetch.c` resolves up to 16 bindings by binding number, independently
of declaration order. It checks each referenced buffer, offset, stride and
attribute extent, supports sparse binding numbers, and publishes its result
only after every check succeeds. An explicit usage mask permits optimized-away
inputs to remain unbound.

`ps5vk_vertex_fetch_compact` converts these spans into the dense SRD order
specified by a compiler-provided mask. `native/draw_prepare_ps5.c` allocates the
descriptor table and any alignment copies in one draw-owned allocation. Failure
releases that allocation; successful preparation flushes the complete allocation
before publication. Copies remain alive until the draw retires.

The internal masked preparation entry point is tested with an explicit mask.
The production entry point and compiler adapter retain the binding-zero gate.
The physical-device report and conformance inventory have not been promoted.

## Shader cache identity

The runtime pair cache includes the complete vertex layout, serialized by
binding number and attribute location. Stride, rate, attribute format, binding
and offset all affect identity because PSBC specializes vertex loads for them.
Previously these fields were absent: two pipelines with identical shader
modules but different vertex layouts could reuse the wrong compiled code.
Compiler integration tests verify separate entries for stride, offset and
format changes, followed by a warm hit for each unchanged layout.

## Remaining validation

Export and version the optimized descriptor mask and indexing mode in PSBC;
propagate them through the runtime shader ABI and cache; then validate separate
vertex buffers, sparse bindings, all 16 bindings, unused shader inputs, odd
binding offsets, indexed draws and allocation failure. Hardware acceptance must
include deterministic readback, artifact identity, TCP telemetry and cleanup.

Instance-rate input and zero-stride bindings remain separate implementation
gaps. The host preparation tests do not establish Vulkan vertex-input coverage
or GPU correctness.
