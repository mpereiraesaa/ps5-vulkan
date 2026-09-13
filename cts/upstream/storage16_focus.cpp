/* Focused registration adapter; the included CTS implementation is unmodified. */
#include "vktSpvAsm16bitStorageTests.cpp"

#include "storage_width_focus.hpp"

namespace vkt
{
namespace SpirVAssembly
{

tcu::TestCaseGroup *createFocused16BitStorageComputeGroup(tcu::TestContext &testCtx)
{
    de::MovePtr<tcu::TestCaseGroup> group(new tcu::TestCaseGroup(testCtx, "16bit_storage"));
    addTestGroup(group.get(), "uniform_32_to_16", addCompute16bitStorageUniform32To16Group);
    addTestGroup(group.get(), "uniform_16_to_32", addCompute16bitStorageUniform16To32Group);
    return group.release();
}

} // namespace SpirVAssembly
} // namespace vkt
