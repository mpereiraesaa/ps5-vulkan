#include "package_ps5.hpp"
#include "vktApiSmokeTests.hpp"
#include "vktSynchronizationBasicFenceTests.hpp"
#include "vktMemoryMappingTests.hpp"
#include "vktComputeBasicComputeShaderTests.hpp"
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
    // api.smoke group
    {
        de::MovePtr<tcu::TestCaseGroup> apiGroup(new tcu::TestCaseGroup(m_testCtx, "api"));
        apiGroup->addChild(vkt::api::createSmokeTests(m_testCtx));
        addChild(apiGroup.release());
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
}

static tcu::TestPackage *createTestPackage(tcu::TestContext &testCtx)
{
    return new FocusedVkTestPackage(testCtx);
}

static tcu::TestPackageDescriptor g_vktPackageDescriptor("dEQP-VK", createTestPackage);

} // namespace ps5
} // namespace cts
