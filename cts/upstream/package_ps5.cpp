#include "package_ps5.hpp"
#include "vktApiSmokeTests.hpp"
#include "vktApiFeatureInfo.hpp"
#include "vktApiBufferViewAccessTests.hpp"
#include "vktApiPipelineTests.hpp"
#include "vktApiCopiesAndBlittingTests.hpp"
#include "vktApiFillBufferTests.hpp"
#include "vktBindingShaderAccessTests.hpp"
#include "vktSynchronizationBasicFenceTests.hpp"
#include "vktSynchronizationBasicEventTests.hpp"
#include "vktSynchronizationBasicSemaphoreTests.hpp"
#include "vktMemoryMappingTests.hpp"
#include "vktComputeBasicComputeShaderTests.hpp"
#include "vktComputeIndirectComputeDispatchTests.hpp"
#include "vktPipelinePushConstantTests.hpp"
#include "vktPipelineCacheTests.hpp"
#include "vktSpvAsmWorkgroupMemoryTests.hpp"
#include "vktDynamicStateComputeTests.hpp"
#include "vktRobustnessBufferAccessTests.hpp"
#include "vktDrawShaderDrawParametersTests.hpp"
#include "vktDrawIndirectTest.hpp"
#include "vktMultiViewTests.hpp"
#include "vktClippingTests.hpp"
#include "vktTestGroupUtil.hpp"
#include "storage_width_focus.hpp"
#include "tcuTestPackage.hpp"
#include "deUniquePtr.hpp"

namespace cts
{
namespace ps5
{

namespace
{

void createDynamicStateMonolithicChildren(tcu::TestCaseGroup *group,
                                          vk::PipelineConstructionType pipelineConstructionType)
{
    group->addChild(vkt::DynamicState::createDynamicStateComputeTests(
        group->getTestContext(), pipelineConstructionType));
}

void cleanupDynamicStateGroup(tcu::TestCaseGroup *)
{
    // The upstream module holds device helpers in file-local singletons.  Keep
    // the same outer-group cleanup lifetime as vktDynamicStateTests.cpp.
    vkt::DynamicState::cleanupDevice();
}

void initDynamicStateGroup(tcu::TestCaseGroup *group)
{
    group->addChild(vkt::createTestGroup(
        group->getTestContext(), "monolithic", createDynamicStateMonolithicChildren,
        vk::PIPELINE_CONSTRUCTION_TYPE_MONOLITHIC));
}

} // namespace

FocusedVkTestPackage::FocusedVkTestPackage(tcu::TestContext &testCtx)
    : BaseTestPackage(testCtx, "dEQP-VK")
{
}

FocusedVkTestPackage::~FocusedVkTestPackage(void)
{
}

void FocusedVkTestPackage::init(void)
{
    // dynamic_state.monolithic.compute_transfer: unchanged upstream compute /
    // transfer non-interference bodies and oracles.  The outer cleanup callback
    // releases the module's singleton device helpers at the upstream lifetime.
    addChild(vkt::createTestGroup(m_testCtx, "dynamic_state", initDynamicStateGroup,
                                  cleanupDynamicStateGroup));

    // info group: original upstream enumeration and physical-device query
    // bodies. cases.txt remains the execution filter; registering these
    // factories does not replace their result oracles.
    {
        de::MovePtr<tcu::TestCaseGroup> infoGroup(
            new tcu::TestCaseGroup(m_testCtx, "info"));
        vkt::api::createFeatureInfoInstanceTests(infoGroup.get());
        vkt::api::createFeatureInfoDeviceTests(infoGroup.get());
        addChild(infoGroup.release());
    }

    // api.smoke group
    {
        de::MovePtr<tcu::TestCaseGroup> apiGroup(new tcu::TestCaseGroup(m_testCtx, "api"));
        apiGroup->addChild(vkt::api::createSmokeTests(m_testCtx));
        // api.buffer_view.access: upstream buffer-view resource tests. The
        // focused selection only runs the compute ones; the group is registered
        // whole so the packaged case list stays the single source of selection.
        de::MovePtr<tcu::TestCaseGroup> bufferViewGroup(new tcu::TestCaseGroup(m_testCtx, "buffer_view"));
        bufferViewGroup->addChild(vkt::api::createBufferViewAccessTests(m_testCtx));
        apiGroup->addChild(bufferViewGroup.release());
        apiGroup->addChild(vkt::api::createPipelineTests(m_testCtx));
        // Original upstream buffer-copy and fill/update cases. The build-time
        // focused copy module prunes only unrelated image/blit/resolve
        // registration so the package stays within the PS5 application heap.
        apiGroup->addChild(vkt::api::createCopiesAndBlittingTests(m_testCtx));
        apiGroup->addChild(vkt::api::createFillAndUpdateBufferTests(m_testCtx));
        addChild(apiGroup.release());
    }

    // binding_model.shader_access group
    {
        de::MovePtr<tcu::TestCaseGroup> bindingModelGroup(new tcu::TestCaseGroup(m_testCtx, "binding_model"));
        bindingModelGroup->addChild(vkt::BindingModel::createShaderAccessTests(m_testCtx));
        addChild(bindingModelGroup.release());
    }

    // synchronization.basic: original legacy event, fence and binary-semaphore
    // factories. cases.txt remains the only leaf filter, so timeline,
    // synchronization2, secondary-command-buffer and multi-queue variants are
    // not silently substituted for the audited Vulkan 1.0 cases.
    {
        de::MovePtr<tcu::TestCaseGroup> syncGroup(new tcu::TestCaseGroup(m_testCtx, "synchronization"));
        de::MovePtr<tcu::TestCaseGroup> syncBasicGroup(new tcu::TestCaseGroup(m_testCtx, "basic"));
        syncBasicGroup->addChild(vkt::synchronization::createBasicEventTests(m_testCtx, 0));
        syncBasicGroup->addChild(vkt::synchronization::createBasicFenceTests(m_testCtx, 0));
        syncBasicGroup->addChild(vkt::synchronization::createBasicBinarySemaphoreTests(
            m_testCtx, vkt::synchronization::SynchronizationType::LEGACY, 0));
        syncGroup->addChild(syncBasicGroup.release());
        addChild(syncGroup.release());
    }

    // memory.mapping group
    {
        de::MovePtr<tcu::TestCaseGroup> memGroup(new tcu::TestCaseGroup(m_testCtx, "memory"));
        memGroup->addChild(vkt::memory::createMappingTests(m_testCtx));
        addChild(memGroup.release());
    }

    // robustness.buffer_access: original upstream Vulkan 1.0 robust-buffer
    // bodies and result oracles.  The generated build copy prunes registration
    // to compute/scalar_copy/R32_UINT; cases.txt remains the leaf filter.
    {
        de::MovePtr<tcu::TestCaseGroup> robustnessGroup(
            new tcu::TestCaseGroup(m_testCtx, "robustness"));
        robustnessGroup->addChild(vkt::robustness::createBufferAccessTests(m_testCtx));
        addChild(robustnessGroup.release());
    }

    // draw.renderpass.shader_draw_parameters: the original upstream
    // draw-parameter bodies and their reference-rasterizer image oracle,
    // registered under the render-pass group parameters only. The
    // dynamic-rendering variants need VK_KHR_dynamic_rendering, which this
    // profile does not advertise, and cases.txt remains the leaf filter.
    {
        de::MovePtr<tcu::TestCaseGroup> drawGroup(new tcu::TestCaseGroup(m_testCtx, "draw"));
        de::MovePtr<tcu::TestCaseGroup> renderPassGroup(
            new tcu::TestCaseGroup(m_testCtx, "renderpass"));
        renderPassGroup->addChild(new vkt::Draw::ShaderDrawParametersTests(
            m_testCtx,
            vkt::Draw::SharedGroupParams(new vkt::Draw::GroupParams{
                false, // useDynamicRendering
                false, // useSecondaryCmdBuffer
                false, // secondaryCmdBufferCompletelyContainsDynamicRenderpass
                false, // nestedSecondaryCmdBuffer
            })));
        // draw.renderpass.indirect_draw: the original upstream indirect-draw
        // module under the same render-pass group parameters. The module
        // registers every variant (compute-generated arguments, draw-count
        // extension, multiview); cases.txt remains the leaf filter, so only the
        // selected sequential/indexed, first-instance and instanced leaves run
        // and every unselected variant stays out without a modified oracle.
        renderPassGroup->addChild(new vkt::Draw::IndirectDrawTests(
            m_testCtx,
            vkt::Draw::SharedGroupParams(new vkt::Draw::GroupParams{
                false, // useDynamicRendering
                false, // useSecondaryCmdBuffer
                false, // secondaryCmdBufferCompletelyContainsDynamicRenderpass
                false, // nestedSecondaryCmdBuffer
            })));
        drawGroup->addChild(renderPassGroup.release());
        addChild(drawGroup.release());
    }

    // multiview group: the original upstream multiview module, registered
    // whole under its own name; cases.txt remains the only leaf filter, so this
    // selects the 48 legacy render-pass leaves this device can run - the
    // clear_attachments, masks, index.vertex_shader and index.fragment_shader
    // families - and nothing else. Its own support gate and per-view oracle are
    // untouched.
    addChild(vkt::MultiView::createTests(m_testCtx, "multiview"));

    // clipping group: the original upstream user-defined clip/cull distance
    // module, registered whole under its own name. No leaf of it is in the
    // acceptance list: the feature flag that gates the whole family (including
    // the fragment-shader-read and dynamic-index variants this profile refuses)
    // is not advertised, so the measured static-index vertex-only subset is
    // recorded as diagnostics in cts/upstream/manifest.json until both refused
    // modes are implemented. Registration makes the group available for that
    // promotion without another packaging change.
    addChild(vkt::clipping::createTests(m_testCtx, "clipping"));

    // compute.basic group
    {
        de::MovePtr<tcu::TestCaseGroup> computeGroup(new tcu::TestCaseGroup(m_testCtx, "compute"));
        computeGroup->addChild(vkt::compute::createBasicComputeShaderTests(m_testCtx, vk::COMPUTE_PIPELINE_CONSTRUCTION_TYPE_PIPELINE));
        computeGroup->addChild(vkt::compute::createIndirectComputeDispatchTests(
            m_testCtx, vk::COMPUTE_PIPELINE_CONSTRUCTION_TYPE_PIPELINE));
        addChild(computeGroup.release());
    }

    // pipeline.push_constant group. The upstream factory is registered without
    // replacing or wrapping individual test bodies; cases.txt remains the only
    // execution filter.
    {
        de::MovePtr<tcu::TestCaseGroup> pipelineGroup(new tcu::TestCaseGroup(m_testCtx, "pipeline"));
        pipelineGroup->addChild(vkt::pipeline::createPushConstantTests(
            m_testCtx, vk::PIPELINE_CONSTRUCTION_TYPE_MONOLITHIC));
        // pipeline.cache: the upstream factory registers its whole family; the
        // packaged case list is the only execution filter, so the graphics cache
        // cases stay unselected while the D16_UNORM prerequisite is missing.
        pipelineGroup->addChild(vkt::pipeline::createCacheTests(
            m_testCtx, vk::PIPELINE_CONSTRUCTION_TYPE_MONOLITHIC));
        addChild(pipelineGroup.release());
    }

    // spirv_assembly.instruction.compute: register generated focused groups.
    // The generator prunes only leaf registration from pinned upstream source;
    // selected shader bodies, support checks and result oracles stay upstream.
    {
        de::MovePtr<tcu::TestCaseGroup> spirvGroup(
            new tcu::TestCaseGroup(m_testCtx, "spirv_assembly"));
        de::MovePtr<tcu::TestCaseGroup> instructionGroup(
            new tcu::TestCaseGroup(m_testCtx, "instruction"));
        de::MovePtr<tcu::TestCaseGroup> computeGroup(
            new tcu::TestCaseGroup(m_testCtx, "compute"));
        computeGroup->addChild(vkt::SpirVAssembly::createFocused8BitStorageComputeGroup(m_testCtx));
        computeGroup->addChild(vkt::SpirVAssembly::createFocused16BitStorageComputeGroup(m_testCtx));
        computeGroup->addChild(vkt::SpirVAssembly::createWorkgroupMemoryComputeGroup(m_testCtx));
        instructionGroup->addChild(computeGroup.release());
        spirvGroup->addChild(instructionGroup.release());
        addChild(spirvGroup.release());
    }
}

static tcu::TestPackage *createTestPackage(tcu::TestContext &testCtx)
{
    return new FocusedVkTestPackage(testCtx);
}

static tcu::TestPackageDescriptor g_vktPackageDescriptor("dEQP-VK", createTestPackage);

} // namespace ps5
} // namespace cts
