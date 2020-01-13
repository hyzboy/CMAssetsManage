file(GLOB ASSETS_MANAGE_HEADER ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/assets/*.h)

set(ASSETS_MANAGE_SOURCE    AssetsSource.cpp
                            AssetsSourceFilesystem.cpp)

add_cm_library(CMAssetsManage   "CM"    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})
