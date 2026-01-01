#pragma once

#include<hgl/type/String.h>

namespace hgl::asset
{
    /**
     * 系统路径类型枚举
     * System path type enumeration for various application paths
     */
    enum class SystemPathType
    {
        Executable=0,           ///< 当前可执行程序路径 (Current executable program path)
        PrivateAssets,          ///< 当前程序私有资产包路径 (Private asset package path - Android: assets, Desktop: empty)
        ExternalAssets,         ///< 外部资产包路径 (External asset package path - Android: external storage, Desktop: AppData)
        AppTemp,                ///< 应用程序临时文件路径 (Application temporary files path)
        AppData,                ///< 应用程序长期文件路径 (Application long-term data path)
        AppPublic,              ///< 应用程序公开文件路径 (Application public files path - My Documents, My Files, etc.)
        Documents,              ///< 文档路径 (Documents directory)
        Music,                  ///< 音频/音乐路径 (Music/Audio directory)
        Pictures,               ///< 照片/图片路径 (Pictures/Photos directory)
        Videos,                 ///< 视频路径 (Videos directory)

        ENUM_CLASS_RANGE(Executable,Videos)
    };//enum class SystemPathType

    /**
     * 获取系统路径
     * Get system path by type
     * @param type 路径类型 (Path type)
     * @return 路径字符串，如果不支持则返回空字符串 (Path string, empty if not supported)
     */
    OSString GetSystemPath(SystemPathType type);

    /**
     * 判断指定路径类型是否在当前平台可用
     * Check if the specified path type is available on current platform
     * @param type 路径类型 (Path type)
     * @return 是否可用 (Whether available)
     */
    bool IsSystemPathAvailable(SystemPathType type);

#if defined(__ANDROID__)
    /**
     * Android平台专用：从Java/Kotlin层设置外部存储路径
     * Android-specific: Set external storage path from Java/Kotlin layer
     */
    void SetAndroidExternalStoragePath(const OSString& path);

    /**
     * Android平台专用：从Java/Kotlin层设置缓存路径
     * Android-specific: Set cache path from Java/Kotlin layer
     */
    void SetAndroidCachePath(const OSString& path);

    /**
     * Android平台专用：从Java/Kotlin层设置应用文件路径
     * Android-specific: Set app files path from Java/Kotlin layer
     */
    void SetAndroidFilesPath(const OSString& path);
#endif
}//namespace hgl::asset
