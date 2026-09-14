# Vertex input implementation

The runtime graphics profile supports and reports 16 vertex bindings. The
runtime compiler and native queue use the optimized descriptor mask described
below. This is a qualified per-vertex, non-indexed path, not complete Vulkan
vertex-input semantics.

## Descriptor order

The pinned PSBC compiler lowers vertex inputs through RADV:
`src/amd/vulkan/nir/radv_nir_lower_vs_inputs.c` computes the descriptor index as
the population count of the used-binding mask below the requested binding.
For bindings 3 and 15, the native SRDs therefore occupy slots 0 and 1. If
optimization removes binding 3, binding 15 occupies slot 0 instead.

`PsbcShaderMetadata` version 11 exports the vertex-table user SGPR,
`vertex_buffer_usage_mask` (from RADV `vs.vb_desc_usage_mask`) and
`vertex_buffer_per_attribute` (from `vs.use_per_attribute_vb_descs`).
The pinned compiler revision is
`7ee039881a1e4a1ffc434be7249da562345e7dcb`. Rebuild callers and compiler together:
the metadata structure grew. Inferring the mask from declarations is incorrect
when optimization removes inputs. The native adapter rejects per-attribute
indexing and inconsistent or older metadata instead of guessing its mapping.

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

The production native queue now calls masked preparation using the compiled
shader ABI, not the declaration count. Pipeline objects own copies of all
binding descriptions, independently of the application's storage lifetime.

## Shader cache identity

The runtime pair cache includes the complete vertex layout, serialized by
binding number and attribute location. Stride, rate, attribute format, binding
and offset all affect identity because PSBC specializes vertex loads for them.
Previously these fields were absent: two pipelines with identical shader
modules but different vertex layouts could reuse the wrong compiled code.
Compiler integration tests verify separate entries for stride, offset and
format changes, followed by a warm hit for each unchanged layout.
The cache key also includes metadata version 11 (adapter version 5); metadata
and the optimized mask survive cold and warm pair acquisition.

## Remaining validation

The owned diagnostic (probe 13) checks 16 distinct buffers, reversed
location-to-binding order, differing strides/attribute offsets and odd binding
offsets. Specialization reduces live inputs to binding 15 or bindings 3/15;
unused buffers are deliberately not created or bound. Repeating the full
variant must hit the cache. Every draw requires exactly 471,744 white pixels
and no unexpected pixels, with compute regression before and after it.

`tools/verify_vertex_bindings.py` checks complete TCP streams, masks,
alignment-copy sizes, serial ordering, pixel results, cache reuse and cleanup.
Deployment readback and OS process termination are separate checks; the parser
explicitly does not certify them. See VALIDATION.md for the measured artifact.

Indexed draws with runtime shaders, per-attribute descriptor mode and exhaustive
upstream CTS input permutations remain pending. Allocation failure is covered
by host preparation/rollback tests, not by injected console failures.

Instance-rate input and zero-stride bindings remain separate implementation
gaps. The host preparation tests do not establish Vulkan vertex-input coverage
or GPU correctness.
