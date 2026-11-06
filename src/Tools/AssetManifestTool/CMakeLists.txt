# AssetManifestTool —— 目录包裹清单的离线校验与内容哈希工具。
#
# 它是一个**可独立运行的命令行程序**，不属于引擎运行时：
#   · 运行期挂载只做浅校验（size/mtime），绝不读文件内容；
#   · 内容指纹在「能容忍慢」的时机算：进版本库前、出包前、CI 闸门。
#
# ── 依赖 ────────────────────────────────────────────────────────────────────
# 除了 CMAssetsManage（清单 / 索引 / 扫描与运行期同源）之外还需要 CMUtil：
# 内容哈希（BLAKE3 / MD5 / SHA256 …）的实现都在那里。CMUtil 不依赖
# CMAssetsManage，因此这条依赖不会成环。
#
# 用 `if(TARGET CMUtil)` 而不是硬 require：CMAssetsManage 也可能被单独配置
# 用来跑单测，那时 CMUtil 不在工程里，工具就静默不构建 —— 让「跑不了工具」
# 表现为「目标不存在」，而不是「配置阶段就失败」。
IF(NOT TARGET CMUtil)
    MESSAGE(STATUS "AssetManifestTool: CMUtil 不在本工程内，跳过工具构建")
    RETURN()
ENDIF()

add_executable(AssetManifestTool main.cpp)

target_link_libraries(AssetManifestTool PRIVATE CMAssetsManage)
target_link_libraries(AssetManifestTool PRIVATE CMUtil)
target_link_libraries(AssetManifestTool PRIVATE CMCore)

IF(WIN32)
    # add_cm_library 只 PRIVATE 链接 ${HGL_MATH_LIB}，静态库消费者需自行认领。
    target_link_libraries(AssetManifestTool PRIVATE ${HGL_MATH_LIB})
ENDIF()

set_target_properties(AssetManifestTool PROPERTIES FOLDER "CM/Tools")

IF(MSVC)
    # 双击或在 IDE 里按 F5 时把工作目录放到源码目录，便于对照仓库里的样例。
    set_target_properties(AssetManifestTool PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
ENDIF()
