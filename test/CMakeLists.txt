# CMAssetsManage 自测台架。
#
# 这些用例只依赖 CMCore / CMMath + CMAssetsManage，不引用任何 ULRE 头文件。
# 之所以单独成台架而不复用 ULRE 的 add_ecs_test：后者链 ULRE::Engine，一旦用例
# 偷偷用上 hgl/graph、hgl/ecs，缺陷会被整个引擎链进来而掩盖掉。这里只链
# CMAssetsManage，边界破了就编译不过 —— 这正是本目录存在的意义。
#
# 三个用例都是纯数据用例（无窗口 / 无设备 / 无文件 IO），因此无显卡机器与 CI 也能跑。
function(add_assets_test target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE CMAssetsManage)

    if(WIN32)
        target_link_libraries(${target_name} PRIVATE ${HGL_MATH_LIB})
    endif()

    set_target_properties(${target_name} PROPERTIES FOLDER "CM/Tests/CMAssetsManage")

    if(MSVC)
        set_target_properties(${target_name} PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endfunction()

# 统一序列化框架：Value 树、Archive 双端遍历、TOML 往返、缺键语义、严格键校验。
add_assets_test(TestSerializeArchive TestSerializeArchive.cpp)

# 统一资产 URI 的解析/拼写契约（纯文本 -> 段，不涉及任何资产实现）。
add_assets_test(TestAssetURI TestAssetURI.cpp)

# 资产唯一身份标识：UUIDv7 生成、文本/字节往返、严格解析、ID 寻址定位段。
add_assets_test(TestAssetID TestAssetID.cpp)

# 资产索引表：逻辑路径推导、双主键一致性、覆盖语义、批量规模。
add_assets_test(TestAssetIndex TestAssetIndex.cpp)

# 清单持久化：TOML 往返、包级算法、缺失/损坏/版本过新的三种失败语义、稳定输出。
add_assets_test(TestAssetManifest TestAssetManifest.cpp)

# 包裹机制：pack 段拼写、路径规范化、目录包裹后端的真实读写与统一取流入口。
add_assets_test(TestAssetPack TestAssetPack.cpp)

# MetaObject: meta/payload 两层生命周期契约（原 CMCore examples/system/MetaObjectTest，
# 随 MetaObject 一起下沉到本库）。
add_assets_test(MetaObjectTest MetaObjectTest.cpp)

# BUILD_TESTING 由 ULRE 主仓根 CMakeLists 的 include(CTest) 定义；本库单独配置时
# 没有这个变量，此时同样注册（add_test 在未 enable_testing() 的目录里也无害）。
# 名称与 LABELS 沿用它们在 src/ecs/CMakeLists.txt 里登记时的值，按 label 过滤的脚本不受影响。
if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_test(NAME TestSerializeArchive COMMAND TestSerializeArchive
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestSerializeArchive PROPERTIES LABELS "serialize;archive")

    add_test(NAME TestAssetURI COMMAND TestAssetURI
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestAssetURI PROPERTIES LABELS "asset;uri")

    add_test(NAME TestAssetID COMMAND TestAssetID
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestAssetID PROPERTIES LABELS "asset;id")

    add_test(NAME TestAssetIndex COMMAND TestAssetIndex
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestAssetIndex PROPERTIES LABELS "asset;index")

    add_test(NAME TestAssetManifest COMMAND TestAssetManifest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestAssetManifest PROPERTIES LABELS "asset;manifest")

    add_test(NAME TestAssetPack COMMAND TestAssetPack
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(TestAssetPack PROPERTIES LABELS "asset;pack")

    add_test(NAME MetaObjectTest COMMAND MetaObjectTest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(MetaObjectTest PROPERTIES LABELS "object;asset")
endif()