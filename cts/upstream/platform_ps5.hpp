#ifndef _CTS_UPSTREAM_PLATFORM_PS5_HPP
#define _CTS_UPSTREAM_PLATFORM_PS5_HPP

#include "tcuPlatform.hpp"
#include "vkPlatform.hpp"

namespace cts
{
namespace ps5
{

class Ps5Platform : public tcu::Platform
{
public:
    Ps5Platform(void);
    virtual ~Ps5Platform(void) override;

    virtual const vk::Platform &getVulkanPlatform(void) const override;
    virtual void getMemoryLimits(tcu::PlatformMemoryLimits &limits) const override;
    virtual bool processEvents(void) override;

private:
    class Ps5VulkanPlatform : public vk::Platform
    {
    public:
        Ps5VulkanPlatform(void);
        virtual ~Ps5VulkanPlatform(void) override;

        virtual vk::Library *createLibrary(vk::Platform::LibraryType libraryType = vk::Platform::LIBRARY_TYPE_VULKAN,
                                           const char *libraryPath = nullptr) const override;
        virtual void describePlatform(std::ostream &dst) const override;
    };

    Ps5VulkanPlatform m_vkPlatform;
};

} // namespace ps5
} // namespace cts

#endif // _CTS_UPSTREAM_PLATFORM_PS5_HPP
