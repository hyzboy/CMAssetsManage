file(GLOB ASSETS_MANAGE_HEADER ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/assets/*.h)

set(ASSETS_MANAGE_SOURCE    AssetsSource.cpp
                            AssetsSourceFilesystem.cpp)

IF(ANDROID)
    set(ASSETS_MANAGE_SOURCE    ${ASSETS_MANAGE_SOURCE}
                                AssetsSourceAndroid.cpp)
ENDIF()

add_cm_library(CMAssetsManage   "CM"    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})
