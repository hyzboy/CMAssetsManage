#include<hgl/asset/SystemPath.h>

#include<windows.h>
#include<shlobj.h>
#include<vector>

namespace hgl::asset
{
    OSString GetSystemPath(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            {
                std::vector<wchar_t> buffer(MAX_PATH);
                DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

                while(size == buffer.size())
                {
                    buffer.resize(buffer.size() * 2);
                    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                }

                if(size > 0)
                {
                    OSString path(buffer.data());
                    // Remove executable filename to get directory
                    int pos = path.FindRightChar('\\');
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
                wchar_t* path = nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)))
                {
                    OSString result(path);
                    CoTaskMemFree(path);
                    return result;
                }
                return OSString();
            }

            case SystemPathType::AppTemp:
            {
                std::vector<wchar_t> buffer(MAX_PATH);
                DWORD size = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());

                if(size > 0 && size < buffer.size())
                {
                    return OSString(buffer.data());
                }
                else if(size > buffer.size())
                {
                    buffer.resize(size);
                    size = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
                    if(size > 0)
                        return OSString(buffer.data());
                }
                return OSString();
            }

            case SystemPathType::AppPublic:
            case SystemPathType::Documents:
            {
                wchar_t* path = nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)))
                {
                    OSString result(path);
                    CoTaskMemFree(path);
                    return result;
                }
                return OSString();
            }

            case SystemPathType::Music:
            {
                wchar_t* path = nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path)))
                {
                    OSString result(path);
                    CoTaskMemFree(path);
                    return result;
                }
                return OSString();
            }

            case SystemPathType::Pictures:
            {
                wchar_t* path = nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
                {
                    OSString result(path);
                    CoTaskMemFree(path);
                    return result;
                }
                return OSString();
            }

            case SystemPathType::Videos:
            {
                wchar_t* path = nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path)))
                {
                    OSString result(path);
                    CoTaskMemFree(path);
                    return result;
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
                // Not available on Windows desktop
                return false;

            default:
                return false;
        }
    }

    // Android-specific functions (empty implementations on Windows)
    void SetAndroidExternalStoragePath(const OSString&) {}
    void SetAndroidCachePath(const OSString&) {}
    void SetAndroidFilesPath(const OSString&) {}
}//namespace hgl::asset
