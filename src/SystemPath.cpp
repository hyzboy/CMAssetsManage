#include<hgl/asset/SystemPath.h>

namespace hgl::asset
{
    // Platform-specific implementations
    // These functions are implemented in platform-specific files

#if defined(__ANDROID__)
    extern OSString GetSystemPathAndroid(SystemPathType type);
    extern bool IsSystemPathAvailableAndroid(SystemPathType type);
#elif defined(_WIN32) || defined(_WIN64)
    extern OSString GetSystemPathWindows(SystemPathType type);
    extern bool IsSystemPathAvailableWindows(SystemPathType type);
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        extern OSString GetSystemPathIOS(SystemPathType type);
        extern bool IsSystemPathAvailableIOS(SystemPathType type);
    #else
        extern OSString GetSystemPathMacOS(SystemPathType type);
        extern bool IsSystemPathAvailableMacOS(SystemPathType type);
    #endif
#elif defined(__linux__)
    extern OSString GetSystemPathLinux(SystemPathType type);
    extern bool IsSystemPathAvailableLinux(SystemPathType type);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    extern OSString GetSystemPathBSD(SystemPathType type);
    extern bool IsSystemPathAvailableBSD(SystemPathType type);
#endif

    OSString GetSystemPath(SystemPathType type)
    {
#if defined(__ANDROID__)
        return GetSystemPathAndroid(type);
#elif defined(_WIN32) || defined(_WIN64)
        return GetSystemPathWindows(type);
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        return GetSystemPathIOS(type);
    #else
        return GetSystemPathMacOS(type);
    #endif
#elif defined(__linux__)
        return GetSystemPathLinux(type);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        return GetSystemPathBSD(type);
#else
        return OSString();
#endif
    }

    bool IsSystemPathAvailable(SystemPathType type)
    {
#if defined(__ANDROID__)
        return IsSystemPathAvailableAndroid(type);
#elif defined(_WIN32) || defined(_WIN64)
        return IsSystemPathAvailableWindows(type);
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        return IsSystemPathAvailableIOS(type);
    #else
        return IsSystemPathAvailableMacOS(type);
    #endif
#elif defined(__linux__)
        return IsSystemPathAvailableLinux(type);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        return IsSystemPathAvailableBSD(type);
#else
        return false;
#endif
    }
}//namespace hgl::asset
