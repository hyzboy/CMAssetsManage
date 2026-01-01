# SystemPath API Documentation

## 概述 (Overview)

SystemPath模块提供了跨平台的系统路径获取功能，支持所有主要操作系统：Windows、Linux、BSD、macOS、iOS和Android。

The SystemPath module provides cross-platform system path retrieval functionality, supporting all major operating systems: Windows, Linux, BSD, macOS, iOS, and Android.

## 路径类型 (Path Types)

### SystemPathType 枚举

```cpp
enum class SystemPathType
{
    Executable,      // 当前可执行程序路径 (Current executable program path)
    PrivateAssets,   // 当前程序私有资产包路径 (Private asset package path)
    ExternalAssets,  // 外部资产包路径 (External asset package path)
    AppTemp,         // 应用程序临时文件路径 (Application temporary files path)
    AppData,         // 应用程序长期文件路径 (Application long-term data path)
    AppPublic,       // 应用程序公开文件路径 (Application public files path)
    Documents,       // 文档路径 (Documents directory)
    Music,           // 音频/音乐路径 (Music/Audio directory)
    Pictures,        // 照片/图片路径 (Pictures/Photos directory)
    Videos,          // 视频路径 (Videos directory)
};
```

## 平台实现 (Platform Implementation)

### Windows
- **Executable**: 可执行文件所在目录
- **PrivateAssets**: 不可用（返回空）
- **ExternalAssets/AppData**: `%APPDATA%` (Roaming)
- **AppTemp**: `%TEMP%`
- **AppPublic/Documents**: 我的文档 (My Documents)
- **Music**: 我的音乐 (My Music)
- **Pictures**: 我的图片 (My Pictures)
- **Videos**: 我的视频 (My Videos)

### Linux
- **Executable**: 通过 `/proc/self/exe` 获取
- **PrivateAssets**: 不可用（返回空）
- **ExternalAssets/AppData**: `$XDG_DATA_HOME` 或 `~/.local/share`
- **AppTemp**: `$XDG_CACHE_HOME` 或 `~/.cache`
- **AppPublic/Documents**: `~/Documents`
- **Music**: `~/Music`
- **Pictures**: `~/Pictures`
- **Videos**: `~/Videos`

### BSD (FreeBSD, OpenBSD, NetBSD, DragonFly)
- **Executable**: 通过 sysctl 或 `/proc/curproc/exe` 获取
- **PrivateAssets**: 不可用（返回空）
- **ExternalAssets/AppData**: `$XDG_DATA_HOME` 或 `~/.local/share`
- **AppTemp**: `$XDG_CACHE_HOME` 或 `~/.cache`
- **AppPublic/Documents**: `~/Documents`
- **Music**: `~/Music`
- **Pictures**: `~/Pictures`
- **Videos**: `~/Videos`

### macOS
- **Executable**: 通过 `_NSGetExecutablePath` 获取
- **PrivateAssets**: 不可用（返回空）
- **ExternalAssets/AppData**: `~/Library/Application Support`
- **AppTemp**: `~/Library/Caches`
- **AppPublic/Documents**: `~/Documents`
- **Music**: `~/Music`
- **Pictures**: `~/Pictures`
- **Videos**: `~/Movies` (macOS使用Movies而非Videos)

### iOS
- **Executable**: App Bundle路径
- **PrivateAssets**: App Bundle中的Resources目录
- **ExternalAssets/AppData**: Documents目录
- **AppTemp**: 临时目录 (NSTemporaryDirectory)
- **AppPublic/Documents**: Documents目录
- **Music**: Music目录
- **Pictures**: Pictures目录
- **Videos**: Movies目录

### Android
- **Executable**: 通过 `/proc/self/exe` 获取
- **PrivateAssets**: "assets" (需要通过AssetManager访问)
- **ExternalAssets**: 外部存储路径 (需要从Java层设置)
- **AppData**: 应用文件目录 (需要从Java层设置)
- **AppTemp**: 应用缓存目录 (需要从Java层设置)
- **AppPublic/Documents**: 外部存储的Documents目录
- **Music**: 外部存储的Music目录
- **Pictures**: 外部存储的Pictures目录
- **Videos**: 外部存储的Movies目录

## API 使用 (API Usage)

### 获取路径 (Get Path)

```cpp
#include<hgl/asset/SystemPath.h>

// 获取可执行程序路径
OSString exePath = hgl::asset::GetSystemPath(hgl::asset::SystemPathType::Executable);

// 获取应用数据路径
OSString dataPath = hgl::asset::GetSystemPath(hgl::asset::SystemPathType::AppData);

// 获取临时文件路径
OSString tempPath = hgl::asset::GetSystemPath(hgl::asset::SystemPathType::AppTemp);
```

### 检查路径是否可用 (Check Path Availability)

```cpp
if(hgl::asset::IsSystemPathAvailable(hgl::asset::SystemPathType::PrivateAssets))
{
    OSString assetsPath = hgl::asset::GetSystemPath(hgl::asset::SystemPathType::PrivateAssets);
    // 使用路径...
}
```

### Android特殊处理 (Android Special Handling)

在Android上，某些路径需要从Java/Kotlin层通过JNI设置：

```cpp
#if defined(__ANDROID__)
// 在JNI初始化时调用
void InitAndroidPaths(JNIEnv* env, jobject context)
{
    // 从Context获取路径并设置
    // context.getExternalFilesDir(null).getAbsolutePath()
    hgl::asset::SetAndroidExternalStoragePath(externalPath);
    
    // context.getCacheDir().getAbsolutePath()
    hgl::asset::SetAndroidCachePath(cachePath);
    
    // context.getFilesDir().getAbsolutePath()
    hgl::asset::SetAndroidFilesPath(filesPath);
}
#endif
```

## 注意事项 (Notes)

1. **桌面平台**: PrivateAssets路径返回空字符串，因为桌面应用通常没有单独的私有资产包概念
2. **Android**: 需要从Java层设置某些路径才能正常工作
3. **iOS**: 使用Objective-C API实现，需要链接Foundation框架
4. **路径权限**: 确保应用有相应的权限访问返回的路径
5. **空返回值**: 如果路径不可用或获取失败，返回空字符串

## 示例代码 (Example Code)

```cpp
#include<hgl/asset/SystemPath.h>
#include<iostream>

void PrintAllPaths()
{
    using namespace hgl::asset;
    
    const SystemPathType types[] = {
        SystemPathType::Executable,
        SystemPathType::PrivateAssets,
        SystemPathType::ExternalAssets,
        SystemPathType::AppTemp,
        SystemPathType::AppData,
        SystemPathType::AppPublic,
        SystemPathType::Documents,
        SystemPathType::Music,
        SystemPathType::Pictures,
        SystemPathType::Videos
    };
    
    const char* names[] = {
        "Executable",
        "PrivateAssets",
        "ExternalAssets",
        "AppTemp",
        "AppData",
        "AppPublic",
        "Documents",
        "Music",
        "Pictures",
        "Videos"
    };
    
    for(int i = 0; i < 10; i++)
    {
        if(IsSystemPathAvailable(types[i]))
        {
            OSString path = GetSystemPath(types[i]);
            std::cout << names[i] << ": " << path.c_str() << std::endl;
        }
        else
        {
            std::cout << names[i] << ": Not available" << std::endl;
        }
    }
}
```
