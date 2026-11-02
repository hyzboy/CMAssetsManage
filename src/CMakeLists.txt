# 头文件按子目录显式登记。
# 原 file(GLOB ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/*.h) 有两个问题：
#   1. 只收一层目录，新增子目录（如 generated/）收不到；
#   2. GLOB 是配置期求值，新增头文件不会触发 CMake 重新配置，IDE 里也就不出现。
# 资产基础设施持续下沉，这里改为逐项登记。
SET(ASSETS_MANAGE_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetSource.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFile.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileHandler.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileManager.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/SystemPath.h)

set(ASSETS_MANAGE_SOURCE    AssetSource.cpp
                            AssetSourceFilesystem.cpp
                            LoadedFile.cpp
                            LoadedFileManager.cpp
)

# Add platform-specific SystemPath implementations
IF(ANDROID)
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                AssetSourceAndroid.cpp
                                SystemPathAndroid.cpp)
ELSEIF(WIN32)
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                SystemPathWindows.cpp)
ELSEIF(APPLE)
    if(IOS)
        set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                    SystemPathIOS.mm)
    else()
        set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                    SystemPathMacOS.cpp)
    endif()
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD|NetBSD|DragonFly")
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                SystemPathBSD.cpp)
ELSEIF(UNIX AND NOT APPLE)
    # Linux
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                SystemPathLinux.cpp)
ENDIF()

SOURCE_GROUP("hgl\\asset" FILES ${ASSETS_MANAGE_HEADER})

add_cm_library(CMAssetsManage   "CM"    ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})

# add_cm_library 只 PRIVATE 链接 ${HGL_MATH_LIB}，不链接 CMCore。
# 资产基础设施（MetaObject / LoadedFile / 异步加载管线）直接使用 CMCore 的
# RefObject、MemoryBlock、线程池与 Log，非 Windows 的 SHARED 构建必须在库一级
# 显式声明依赖，否则链接期找不到符号。Windows 静态库虽靠最终 exe 兜底，但把问题
# 延迟到那一步定位代价更高，这里统一声明。
target_link_libraries(CMAssetsManage PUBLIC CMCore)