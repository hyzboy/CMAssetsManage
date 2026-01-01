#include<hgl/asset/SystemPath.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE && !TARGET_IPHONE_SIMULATOR

#include<mach-o/dyld.h>
#include<limits.h>
#include<pwd.h>
#include<sys/types.h>
#include<unistd.h>
#include<cstdlib>

namespace hgl::asset
{
    OSString GetSystemPathMacOS(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            {
                char buffer[PATH_MAX];
                uint32_t size = sizeof(buffer);
                
                if(_NSGetExecutablePath(buffer, &size) == 0)
                {
                    // Resolve any symbolic links
                    char realPath[PATH_MAX];
                    if(realpath(buffer, realPath))
                    {
                        OSString path(realPath);
                        // Remove executable filename to get directory
                        int pos = path.FindRightChar('/');
                        if(pos > 0)
                            return path.SubString(0, pos);
                        return path;
                    }
                }
                return OSString();
            }

            case SystemPathType::PrivateAssets:
                // Desktop platforms don't have a separate private assets path
                return OSString();

            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            {
                // Use ~/Library/Application Support
                const char* home = std::getenv("HOME");
                if(!home)
                {
                    struct passwd* pw = getpwuid(getuid());
                    if(pw)
                        home = pw->pw_dir;
                }
                
                if(home)
                {
                    OSString path(home);
                    path += "/Library/Application Support";
                    return path;
                }
                return OSString();
            }

            case SystemPathType::AppTemp:
            {
                // Use ~/Library/Caches
                const char* home = std::getenv("HOME");
                if(!home)
                {
                    struct passwd* pw = getpwuid(getuid());
                    if(pw)
                        home = pw->pw_dir;
                }
                
                if(home)
                {
                    OSString path(home);
                    path += "/Library/Caches";
                    return path;
                }
                
                // Fallback to TMPDIR or /tmp
                const char* tmpdir = std::getenv("TMPDIR");
                if(tmpdir && tmpdir[0] != '\0')
                {
                    return OSString(tmpdir);
                }
                return OSString("/tmp");
            }

            case SystemPathType::AppPublic:
            {
                // Use ~/Documents
                const char* home = std::getenv("HOME");
                if(!home)
                {
                    struct passwd* pw = getpwuid(getuid());
                    if(pw)
                        home = pw->pw_dir;
                }
                
                if(home)
                {
                    OSString path(home);
                    path += "/Documents";
                    return path;
                }
                return OSString();
            }

            default:
                return OSString();
        }
    }

    bool IsSystemPathAvailableMacOS(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            case SystemPathType::AppTemp:
            case SystemPathType::AppPublic:
                return true;

            case SystemPathType::PrivateAssets:
                // Not available on macOS desktop
                return false;

            default:
                return false;
        }
    }
}//namespace hgl::asset

#endif // __APPLE__ && !TARGET_OS_IPHONE
