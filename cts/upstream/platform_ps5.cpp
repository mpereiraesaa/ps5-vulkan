#include "platform_ps5.hpp"
#include "tcuFunctionLibrary.hpp"
#include "vkPlatform.hpp"
#include "vkDefs.hpp"
#include <iostream>

extern "C" {
void *vkGetInstanceProcAddr(void *instance, const char *pName);
void *vkGetDeviceProcAddr(void *device, const char *pName);
}

namespace cts
{
namespace ps5
{

namespace
{

static const tcu::StaticFunctionLibrary::Entry s_entries[] =
{
    { "vkGetInstanceProcAddr", (deFunctionPtr)vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr",   (deFunctionPtr)vkGetDeviceProcAddr }
};

class Ps5VulkanLibrary : public vk::Library
{
public:
    Ps5VulkanLibrary(void)
        : m_library(s_entries, DE_LENGTH_OF_ARRAY(s_entries))
        , m_driver(m_library)
    {
    }

    virtual ~Ps5VulkanLibrary(void) override
    {
    }

    virtual const vk::PlatformInterface &getPlatformInterface(void) const override
    {
        return m_driver;
    }

    virtual const tcu::FunctionLibrary &getFunctionLibrary(void) const override
    {
        return m_library;
    }

private:
    const tcu::StaticFunctionLibrary m_library;
    const vk::PlatformDriver m_driver;
};

} // anonymous namespace

Ps5Platform::Ps5VulkanPlatform::Ps5VulkanPlatform(void)
{
}

Ps5Platform::Ps5VulkanPlatform::~Ps5VulkanPlatform(void)
{
}

vk::Library *Ps5Platform::Ps5VulkanPlatform::createLibrary(vk::Platform::LibraryType libraryType, const char *libraryPath) const
{
    DE_UNREF(libraryType);
    DE_UNREF(libraryPath);
    return new Ps5VulkanLibrary();
}

void Ps5Platform::Ps5VulkanPlatform::describePlatform(std::ostream &dst) const
{
    dst << "Target: Sony PlayStation 5 (x86_64-sie-ps5)\n";
    dst << "Firmware: 12.02\n";
    dst << "GPU Architecture: AMD GFX1013 (RDNA2)\n";
    dst << "Driver: ps5vk (focused Vulkan 1.0 API subset)\n";
}

Ps5Platform::Ps5Platform(void)
{
}

Ps5Platform::~Ps5Platform(void)
{
}

const vk::Platform &Ps5Platform::getVulkanPlatform(void) const
{
    return m_vkPlatform;
}

void Ps5Platform::getMemoryLimits(tcu::PlatformMemoryLimits &limits) const
{
    limits.totalSystemMemory = (size_t)3ULL * 1024 * 1024 * 1024;
    limits.totalDeviceLocalMemory = (std::uint64_t)3ULL * 1024 * 1024 * 1024;
    limits.deviceMemoryAllocationGranularity = 64 * 1024;
    limits.devicePageSize = 4096;
    limits.devicePageTableEntrySize = 8;
    limits.devicePageTableHierarchyLevels = 3;
}

bool Ps5Platform::processEvents(void)
{
    return true;
}

} // namespace ps5
} // namespace cts
