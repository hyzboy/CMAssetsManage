file(GLOB ASSETS_MANAGE_HEADER ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/*.h)

set(ASSETS_MANAGE_SOURCE    AssetSource.cpp
                            AssetSourceFilesystem.cpp
                            AssetIDManager.cpp
                            SystemPath.cpp
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
                                    SystemPathIOS.cpp)
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

add_cm_library(CMAssetsManage   "CM"    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})
