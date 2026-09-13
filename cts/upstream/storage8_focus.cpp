/* Focused registration adapter; the included CTS implementation is unmodified. */
#include "vktSpvAsm8bitStorageTests.cpp"

#include "storage_width_focus.hpp"

namespace vkt
{
namespace SpirVAssembly
{

tcu::TestCaseGroup *createFocused8BitStorageComputeGroup(tcu::TestContext &testCtx)
{
    de::MovePtr<tcu::TestCaseGroup> group(new tcu::TestCaseGroup(testCtx, "8bit_storage"));
    addTestGroup(group.get(), "storagebuffer_32_to_8", addCompute8bitStorage32To8Group);
    addTestGroup(group.get(), "uniform_8_to_8", addCompute8bitStorageBuffer8To8Group);
    return group.release();
}

} // namespace SpirVAssembly
} // namespace vkt
