#ifndef PS5VK_CTS_STORAGE_WIDTH_FOCUS_HPP
#define PS5VK_CTS_STORAGE_WIDTH_FOCUS_HPP

#include "tcuTestCase.hpp"

namespace vkt
{
namespace SpirVAssembly
{

tcu::TestCaseGroup *createFocused8BitStorageComputeGroup(tcu::TestContext &testCtx);
tcu::TestCaseGroup *createFocused16BitStorageComputeGroup(tcu::TestContext &testCtx);

} // namespace SpirVAssembly
} // namespace vkt

#endif
