# AssetManifestTool —— 目录包裹清单的离线校验与内容哈希工具。
#
# 它是一个**可独立运行的命令行程序**，不属于引擎运行时：
#   · 运行期挂载只做浅校验（size/mtime），绝不读文件内容；
#   · 内容指纹在「能容忍慢」的时机算：进版本库前、出包前、CI 闸门。
#
# ── 依赖 ────────────────────────────────────────────────────────────────────
# 除了 CMAssetsManage（清单 / 索引 / 扫描与运行期同源）之外还需要 CMHash 与 CMCore：
# 散列算法本体在 CMHash，文件/流的哈希封装（HashFile）在 CMCore。
# 算法名单与名称全部来自 CMHash 的 Clang AST 派生注册表，本工具不做任何
# 本地白名单 —— 引擎支持哪些算法，工具就支持哪些。
#
# 用 `if(TARGET CMHash)` 而不是硬 require：CMAssetsManage 也可能被单独配置
# 用来跑单测，那时 CMHash 不在工程里，工具就静默不构建 —— 让「跑不了工具」
# 表现为「目标不存在」，而不是「配置阶段就失败」。
IF(NOT TARGET CMHash)
    MESSAGE(STATUS "AssetManifestTool: CMHash is not in this build, skipping the tool")
    RETURN()
ENDIF()

add_executable(AssetManifestTool main.cpp)

target_link_libraries(AssetManifestTool PRIVATE CMAssetsManage)
target_link_libraries(AssetManifestTool PRIVATE CMHash)
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
