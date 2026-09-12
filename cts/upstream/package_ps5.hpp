#ifndef _CTS_UPSTREAM_PACKAGE_PS5_HPP
#define _CTS_UPSTREAM_PACKAGE_PS5_HPP

#include "vktTestPackage.hpp"

namespace cts
{
namespace ps5
{

class FocusedVkTestPackage : public vkt::BaseTestPackage
{
public:
    FocusedVkTestPackage(tcu::TestContext &testCtx);
    virtual ~FocusedVkTestPackage(void) override;

    virtual void init(void) override;
};

} // namespace ps5
} // namespace cts

#endif // _CTS_UPSTREAM_PACKAGE_PS5_HPP
