file(GLOB ASSETS_MANAGE_HEADER ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/*.h)

set(ASSETS_MANAGE_SOURCE    AssetSource.cpp
                            AssetSourceFilesystem.cpp)

IF(ANDROID)
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                AssetSourceAndroid.cpp)
ENDIF()

add_cm_library(CMAssetsManage   "CM"    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})
