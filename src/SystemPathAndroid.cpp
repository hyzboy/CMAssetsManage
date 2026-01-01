#include<hgl/asset/SystemPath.h>

#if defined(__ANDROID__)

#include<unistd.h>
#include<limits.h>
#include<cstdlib>

// Note: For full Android functionality, this requires JNI context to access proper paths
// The implementation below provides basic functionality

namespace hgl::asset
{
    // These can be set by the application through JNI
    static OSString android_external_storage_path;
    static OSString android_cache_path;
    static OSString android_files_path;

    /**
     * Set Android-specific paths from Java/Kotlin layer
     * Call these from your Android Activity/Application
     */
    void SetAndroidExternalStoragePath(const OSString& path)
    {
        android_external_storage_path = path;
    }

    void SetAndroidCachePath(const OSString& path)
    {
        android_cache_path = path;
    }

    void SetAndroidFilesPath(const OSString& path)
    {
        android_files_path = path;
    }

    OSString GetSystemPathAndroid(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            {
                // On Android, read the executable path from /proc/self/exe
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
            {
                // Android assets are accessed through AssetManager
                // Return "assets" as a marker - actual access requires AssetManager
                return OSString("assets");
            }

            case SystemPathType::ExternalAssets:
            {
                // External storage path (should be set from Java layer)
                if(!android_external_storage_path.IsEmpty())
                    return android_external_storage_path;
                
                // Fallback: try to construct default path
                // Note: This may not work on all Android versions
                const char* external = std::getenv("EXTERNAL_STORAGE");
                if(external && external[0] != '\0')
                {
                    return OSString(external);
                }
                return OSString("/sdcard");
            }

            case SystemPathType::AppData:
            {
                // Internal app files directory (should be set from Java layer)
                if(!android_files_path.IsEmpty())
                    return android_files_path;
                
                return OSString();
            }

            case SystemPathType::AppTemp:
            {
                // App cache directory (should be set from Java layer)
                if(!android_cache_path.IsEmpty())
                    return android_cache_path;
                
                return OSString();
            }

            case SystemPathType::AppPublic:
            {
                // Public documents directory
                if(!android_external_storage_path.IsEmpty())
                {
                    OSString path = android_external_storage_path;
                    path += "/Documents";
                    return path;
                }
                return OSString("/sdcard/Documents");
            }

            default:
                return OSString();
        }
    }

    bool IsSystemPathAvailableAndroid(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            case SystemPathType::PrivateAssets:
            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            case SystemPathType::AppTemp:
            case SystemPathType::AppPublic:
                return true;

            default:
                return false;
        }
    }
}//namespace hgl::asset

#endif // __ANDROID__
