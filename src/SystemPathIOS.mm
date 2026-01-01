#include<hgl/asset/SystemPath.h>

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)

#include<Foundation/Foundation.h>

namespace hgl::asset
{
    OSString GetSystemPathIOS(SystemPathType type)
    {
        @autoreleasepool
        {
            switch(type)
            {
                case SystemPathType::Executable:
                {
                    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
                    if(bundlePath)
                    {
                        return OSString([bundlePath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::PrivateAssets:
                {
                    // iOS apps have a Resources directory in the bundle
                    NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
                    if(resourcePath)
                    {
                        return OSString([resourcePath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::ExternalAssets:
                case SystemPathType::AppData:
                {
                    // Use Documents directory for persistent app data
                    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
                    if([paths count] > 0)
                    {
                        NSString* documentsPath = [paths objectAtIndex:0];
                        return OSString([documentsPath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::AppTemp:
                {
                    // Use temporary directory
                    NSString* tempPath = NSTemporaryDirectory();
                    if(tempPath)
                    {
                        return OSString([tempPath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::AppPublic:
                case SystemPathType::Documents:
                {
                    // iOS doesn't have a traditional "My Documents" concept
                    // Use Documents directory as public-ish location
                    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
                    if([paths count] > 0)
                    {
                        NSString* documentsPath = [paths objectAtIndex:0];
                        return OSString([documentsPath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::Music:
                {
                    // Use Music directory
                    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSMusicDirectory, NSUserDomainMask, YES);
                    if([paths count] > 0)
                    {
                        NSString* musicPath = [paths objectAtIndex:0];
                        return OSString([musicPath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::Pictures:
                {
                    // Use Pictures directory
                    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES);
                    if([paths count] > 0)
                    {
                        NSString* picturesPath = [paths objectAtIndex:0];
                        return OSString([picturesPath UTF8String]);
                    }
                    return OSString();
                }

                case SystemPathType::Videos:
                {
                    // Use Movies directory (iOS uses Movies for videos)
                    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSMoviesDirectory, NSUserDomainMask, YES);
                    if([paths count] > 0)
                    {
                        NSString* moviesPath = [paths objectAtIndex:0];
                        return OSString([moviesPath UTF8String]);
                    }
                    return OSString();
                }

                default:
                    return OSString();
            }
        }
    }

    bool IsSystemPathAvailableIOS(SystemPathType type)
    {
        switch(type)
        {
            case SystemPathType::Executable:
            case SystemPathType::PrivateAssets:
            case SystemPathType::ExternalAssets:
            case SystemPathType::AppData:
            case SystemPathType::AppTemp:
            case SystemPathType::AppPublic:
            case SystemPathType::Documents:
            case SystemPathType::Music:
            case SystemPathType::Pictures:
            case SystemPathType::Videos:
                return true;

            default:
                return false;
        }
    }
}//namespace hgl::asset

#endif // __APPLE__ && TARGET_OS_IPHONE
