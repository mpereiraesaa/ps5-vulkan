#ifndef PS5VK_SAMPLE_RATE_PROBE_H
#define PS5VK_SAMPLE_RATE_PROBE_H
#include <vulkan/vulkan.h>

/* DXVK262-T06 multisampled colour target probe.
 *
 * One multisampled colour attachment is created, cleared through a render pass
 * the front end and the native queue both accept, submitted, and then read
 * back from its OWN storage - no resolve, no copy, no second target. The
 * verdict is the storage contract this profile claims for a multisampled
 * surface: a whole-surface clear leaves one distinct 32-bit value across a
 * span that is the sample count times the single-sample footprint, which a
 * surface sized for one sample could not satisfy. */
struct ps5vk_sample_rate_probe_params {
    VkSampleCountFlagBits samples;
    uint32_t extent;
    float clear[4];
    /* The modules the shaded phase draws with: a vertex module whose triangle
     * covers the target and a fragment module whose output varies with
     * gl_SampleID. */
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *fragment;
    size_t fragment_words;
    /* DIAGNOSTIC (PS5VK_SAMPLE_RATE_DIAGNOSTIC, and ignored without it): the
     * pixel-context words every draw this probe records publishes instead of
     * the compiled ones, as {cx index, value} pairs. The sample-rate line uses
     * it to measure what the hardware does with the position-input enables and
     * the barycentric control in a single payload run; see
     * native/sample_rate_diagnostic.h. */
    uint32_t diagnostic_cx_count;
    uint32_t diagnostic_cx_index[24];
    uint32_t diagnostic_cx_value[24];
    /* What the shaded phase's census is compared against. Zero (the shipping
     * witness) requires the module's value per sample index, which is the
     * sample-id module's contract. One requires only that the draw left as many
     * DISTINCT values as the sample count, each of them a shaded word (alpha
     * one, blue zero) - the contract of the fragment-coordinate module, whose
     * values are positions and not sample indices. */
    uint32_t coordinate_oracle;
};

VkResult ps5vk_sample_rate_probe(VkDevice, const struct ps5vk_sample_rate_probe_params *);

/* DXVK262-T06 shape walk: the CTS oracle's render pass, one step at a time.
 *
 * The focused upstream selection renders the multisampled colour target in a
 * pass whose subpass 0 resolves it and whose subpass 1 reads it back once per
 * sample as an input attachment, writing a single-sample target of its own.
 * That shape is what the measurement reached before the payload died without
 * closing its log, so this walk performs exactly those steps in that order and
 * logs each one before it runs: the last step in the log is the step that
 * killed the process, which is what turns "the payload died" into a named
 * defect. Every refusal is logged with its own step and the walk stops there
 * instead of continuing into a shape nothing described. */
struct ps5vk_sample_rate_shape_params {
    VkSampleCountFlagBits samples;
    uint32_t extent;
    /* The fraction the subpass-0 pipeline asks for when per-sample shading is
     * enabled. 1.0 is one invocation per sample (what the witness measures);
     * 0.0 is what the pinned min_sample_shading leaves ask for, and it is the
     * state the payload died in, so the walk has to be able to name it. */
    float sample_shading_min;
    /* The vertex module whose triangle covers the target, the subpass-0
     * fragment module and the per-sample fetch module (input attachment plus
     * the uniform sample index the upstream stage declares). */
    const uint32_t *vertex;
    size_t vertex_words;
    const uint32_t *write_fragment;
    size_t write_fragment_words;
    /* The subpass-0 module the RESOLVE oracle draws with: the same per-sample
     * shape as write_fragment, but writing a value per sample whose average no
     * sample can hold (R = 4 * (gl_SampleID + 1) / 255, so the samples hold 4,
     * 8, 12, 16 and their average is 10). With the plain 0,1,2,3 pattern the
     * average (1.5, rounded) IS a sample value, so a target that received one
     * sample's plane cannot be told from one that received the average; and
     * starting the pattern at 4 keeps every sample word clear of the 0..3
     * patterns earlier phases of the payload leave in reused memory. */
    const uint32_t *spread_fragment;
    size_t spread_fragment_words;
    const uint32_t *fetch_fragment;
    size_t fetch_fragment_words;
    /* The fragment module whose output varies with gl_SampleID, used by the
     * per-sample fetch phase so each sample plane of the multisampled target
     * holds a value only that sample can have produced. */
    const uint32_t *sample_fragment;
    size_t sample_fragment_words;
    /* The same fetch stage with the sample index baked in, for telling "the
     * index never reached the shader" from "the read ignores the index". */
    const uint32_t *fetch_const_fragment;
    size_t fetch_const_fragment_words;
    /* The fragment stage that reads EVERY sample and writes their average: the
     * arithmetic a resolve is made of. */
    const uint32_t *resolve_fragment;
    size_t resolve_fragment_words;
};

/* Returns VK_SUCCESS when every step ran or stopped at a named refusal; the
 * log, not the return code, is the evidence. */
VkResult ps5vk_sample_rate_shape_probe(VkDevice, const struct ps5vk_sample_rate_shape_params *);
#endif
