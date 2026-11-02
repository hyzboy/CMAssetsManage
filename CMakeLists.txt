cmake_minimum_required(VERSION 3.28)

project(CMAssetsManage)

include(path_config.cmake)
CMAssetsManageSetup(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${CMASSETS_MANAGE_ROOT_SOURCE_PATH})

# 自测台架：只依赖 CMCore / CMMath + 本库，随模块一起构建（与 CMCore/examples 的做法一致）。
# ULRE 主仓通过 use_cm_module(CMAssetsManage) 把它带进构建，产物落在同一个输出目录。
add_subdirectory(test)
