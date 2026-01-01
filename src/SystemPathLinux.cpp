#include<hgl/asset/SystemPath.h>

#include<unistd.h>
#include<limits.h>
#include<pwd.h>
#include<sys/types.h>
#include<cstdlib>

namespace hgl::asset
{
    OSString GetSystemPath(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            {
                char buffer[PATH_MAX];
                ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
                if(len != -1)
                {
                    buffer[len] = '\0';
                    OSString path(buffer);
                    // Remove executable filename to get directory
                    int pos = path.FindRightChar('/');
                    if(pos > 0)
                        return path.SubString(0, pos);
                    return path;
                }
                return OSString();
            }

            case SystemPathType::PrivateAssets:
                // Desktop platforms don't have a separate private assets path
                return OSString();

            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            {
                // Use XDG_DATA_HOME or ~/.local/share
                const char* xdg_data = std::getenv("XDG_DATA_HOME");
                if(xdg_data && xdg_data[0] != '\0')
                {
                    return OSString(xdg_data);
                }
                
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
                    path += "/.local/share";
                    return path;
                }
                return OSString();
            }

            case SystemPathType::AppTemp:
            {
                // Use XDG_CACHE_HOME or ~/.cache
                const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
                if(xdg_cache && xdg_cache[0] != '\0')
                {
                    return OSString(xdg_cache);
                }
                
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
                    path += "/.cache";
                    return path;
                }
                
                // Fallback to /tmp
                return OSString("/tmp");
            }

            case SystemPathType::AppPublic:
            case SystemPathType::Documents:
            {
                // Use XDG_DOCUMENTS_DIR or ~/Documents
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

            case SystemPathType::Music:
            {
                // Use ~/Music
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
                    path += "/Music";
                    return path;
                }
                return OSString();
            }

            case SystemPathType::Pictures:
            {
                // Use ~/Pictures
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
                    path += "/Pictures";
                    return path;
                }
                return OSString();
            }

            case SystemPathType::Videos:
            {
                // Use ~/Videos
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
                    path += "/Videos";
                    return path;
                }
                return OSString();
            }

            default:
                return OSString();
        }
    }

    bool IsSystemPathAvailable(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            case SystemPathType::AppTemp:
            case SystemPathType::AppPublic:
            case SystemPathType::Documents:
            case SystemPathType::Music:
            case SystemPathType::Pictures:
            case SystemPathType::Videos:
                return true;

            case SystemPathType::PrivateAssets:
                // Not available on Linux desktop
                return false;

            default:
                return false;
        }
    }

    // Android-specific functions (empty implementations on Linux)
    void SetAndroidExternalStoragePath(const OSString& path) {}
    void SetAndroidCachePath(const OSString& path) {}
    void SetAndroidFilesPath(const OSString& path) {}
}//namespace hgl::asset
