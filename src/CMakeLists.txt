# 头文件按子目录显式登记。
# 原 file(GLOB ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/*.h) 有两个问题：
#   1. 只收一层目录，新增子目录（如 generated/）收不到；
#   2. GLOB 是配置期求值，新增头文件不会触发 CMake 重新配置，IDE 里也就不出现。
# 资产基础设施持续下沉，这里改为逐项登记。
SET(ASSETS_MANAGE_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetAnnotation.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetFactory.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetObject.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetSource.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetURI.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFile.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileHandler.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileManager.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/SystemPath.h)

# 自动序列化内核（格式无关）：字段遍历 + 反射元表 + 归档值模型。
# 只依赖 CMCore 的 type/utf、CMCoreType 的 color、CMMath 的 VectorTypes，**不依赖任何编解码后端**。
# TomlCodec / FlatCodec / ArchiveFile 三个后端留在 ULRE：它们分别引入 toml++ 与 flatbuffers，
# 而 toml++ 是 vendored 在 ULRE 的 inc/toml 下，搬进来会让本库反向依赖主仓头文件搜索路径。
SET(ASSETS_MANAGE_SERIALIZE_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Archive.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Container.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/EnumMeta.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/MetaSchema.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Traits.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Value.h)

# 对象生命周期基类独立成组：MetaObject 的路径保持 hgl/object，与 CMCore 的
# Object/RefObject 同一命名空间目录，消费方 include 无需改动。
SET(ASSETS_MANAGE_OBJECT_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/object/MetaObject.h)

set(ASSETS_MANAGE_SOURCE    AssetFactory.cpp
                            AssetSource.cpp
                            AssetSourceFilesystem.cpp
                            AssetURI.cpp
                            LoadedFile.cpp
                            LoadedFileManager.cpp
                            MetaObject.cpp
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
SOURCE_GROUP("hgl\\object" FILES ${ASSETS_MANAGE_OBJECT_HEADER})
SOURCE_GROUP("hgl\\serialize" FILES ${ASSETS_MANAGE_SERIALIZE_HEADER})

add_cm_library(CMAssetsManage   "CM"    ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}    ${ASSETS_MANAGE_HEADER}
                                        ${ASSETS_MANAGE_OBJECT_HEADER}
                                        ${ASSETS_MANAGE_SERIALIZE_HEADER}
                                        ${ASSETS_MANAGE_SOURCE})

# add_cm_library 只 PRIVATE 链接 ${HGL_MATH_LIB}，不链接 CMCore。
# 资产基础设施（MetaObject / LoadedFile / 异步加载管线）直接使用 CMCore 的
# RefObject、MemoryBlock、线程池与 Log，非 Windows 的 SHARED 构建必须在库一级
# 显式声明依赖，否则链接期找不到符号。Windows 静态库虽靠最终 exe 兜底，但把问题
# 延迟到那一步定位代价更高，这里统一声明。
#
# hgl/serialize 下沉后新增两条依赖：Traits.h 用 CMMath 的 VectorTypes、CMCoreType
# 的 Color3f/Color4f（CMCoreType 已由 CMCore 传递，CMMath 不是，必须显式声明）。
# 这几个头全是模板/内联，本库自身没有实例化它们的 TU，声明是为了让消费方拿到正确
# 的头文件搜索与链接顺序，而不是为了解析本库的符号。
target_link_libraries(CMAssetsManage PUBLIC CMCore)
target_link_libraries(CMAssetsManage PUBLIC CMMath)