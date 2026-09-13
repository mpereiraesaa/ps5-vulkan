#include "package_ps5.hpp"
#include "vktApiSmokeTests.hpp"
#include "vktApiFeatureInfo.hpp"
#include "vktApiBufferViewAccessTests.hpp"
#include "vktApiPipelineTests.hpp"
#include "vktBindingShaderAccessTests.hpp"
#include "vktSynchronizationBasicFenceTests.hpp"
#include "vktMemoryMappingTests.hpp"
#include "vktComputeBasicComputeShaderTests.hpp"
#include "vktPipelinePushConstantTests.hpp"
#include "storage_width_focus.hpp"
#include "tcuTestPackage.hpp"
#include "deUniquePtr.hpp"

namespace cts
{
namespace ps5
{

FocusedVkTestPackage::FocusedVkTestPackage(tcu::TestContext &testCtx)
    : BaseTestPackage(testCtx, "dEQP-VK")
{
}

FocusedVkTestPackage::~FocusedVkTestPackage(void)
{
}

void FocusedVkTestPackage::init(void)
{
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
        addChild(apiGroup.release());
    }

    // binding_model.shader_access group
    {
        de::MovePtr<tcu::TestCaseGroup> bindingModelGroup(new tcu::TestCaseGroup(m_testCtx, "binding_model"));
        bindingModelGroup->addChild(vkt::BindingModel::createShaderAccessTests(m_testCtx));
        addChild(bindingModelGroup.release());
    }

    // synchronization.basic.fence group
    {
        de::MovePtr<tcu::TestCaseGroup> syncGroup(new tcu::TestCaseGroup(m_testCtx, "synchronization"));
        de::MovePtr<tcu::TestCaseGroup> syncBasicGroup(new tcu::TestCaseGroup(m_testCtx, "basic"));
        syncBasicGroup->addChild(vkt::synchronization::createBasicFenceTests(m_testCtx, 0));
        syncGroup->addChild(syncBasicGroup.release());
        addChild(syncGroup.release());
    }

    // memory.mapping group
    {
        de::MovePtr<tcu::TestCaseGroup> memGroup(new tcu::TestCaseGroup(m_testCtx, "memory"));
        memGroup->addChild(vkt::memory::createMappingTests(m_testCtx));
        addChild(memGroup.release());
    }

    // compute.basic group
    {
        de::MovePtr<tcu::TestCaseGroup> computeGroup(new tcu::TestCaseGroup(m_testCtx, "compute"));
        computeGroup->addChild(vkt::compute::createBasicComputeShaderTests(m_testCtx, vk::COMPUTE_PIPELINE_CONSTRUCTION_TYPE_PIPELINE));
        addChild(computeGroup.release());
    }

    // pipeline.push_constant group. The upstream factory is registered without
    // replacing or wrapping individual test bodies; cases.txt remains the only
    // execution filter.
    {
        de::MovePtr<tcu::TestCaseGroup> pipelineGroup(new tcu::TestCaseGroup(m_testCtx, "pipeline"));
        pipelineGroup->addChild(vkt::pipeline::createPushConstantTests(
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
