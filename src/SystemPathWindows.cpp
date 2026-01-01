#include<hgl/asset/SystemPath.h>

#if defined(_WIN32) || defined(_WIN64)

#include<windows.h>
#include<shlobj.h>
#include<vector>

namespace hgl::asset
{
    OSString GetSystemPathWindows(SystemPathType type)
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
                    size_t pos = path.FindRightChar(OS_TEXT('\\'));
                    if(pos != -1)
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

            default:
                return OSString();
        }
    }

    bool IsSystemPathAvailableWindows(SystemPathType type)
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
                // Not available on Windows desktop
                return false;

            default:
                return false;
        }
    }
}//namespace hgl::asset

#endif // _WIN32 || _WIN64
