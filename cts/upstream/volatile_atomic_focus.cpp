/* Expose focused original upstream sub-factories without registering the entire
 * instruction suite. The pinned source supplies the shader, support gate,
 * execution and expected-result oracle unchanged. */
#include "vktSpvAsmInstructionTests.cpp"

namespace vkt
{
namespace SpirVAssembly
{
tcu::TestCaseGroup *createFocusedVolatileAtomicComputeGroup(tcu::TestContext &testCtx)
{
    return createOpAtomicGroup(testCtx, true, 65535, false, true);
}
tcu::TestCaseGroup *createFocusedLocalSizeIdGroup(tcu::TestContext &testCtx)
{
    return createLocalSizeGroup(testCtx, true);
}

} // namespace SpirVAssembly
} // namespace vkt
