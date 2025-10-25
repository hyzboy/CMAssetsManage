cmake_minimum_required(VERSION 3.28)

project(CMAssetsManage)

include(path_config.cmake)
CMAssetsManageSetup(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${CMASSETS_MANAGE_ROOT_SOURCE_PATH})
