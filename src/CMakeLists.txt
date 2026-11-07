# 头文件按子目录显式登记。
# 原 file(GLOB ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/*.h) 有两个问题：
#   1. 只收一层目录，新增子目录（如 generated/）收不到；
#   2. GLOB 是配置期求值，新增头文件不会触发 CMake 重新配置，IDE 里也就不出现。
# 资产基础设施持续下沉，这里改为逐项登记。
SET(ASSETS_MANAGE_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetAnnotation.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetCPUTask.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetCPUTaskPool.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetCPUTasks.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetDependencyGraph.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetFactory.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetID.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetIndex.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetIODispatcher.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetManifest.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetObject.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetSource.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetStream.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/AssetURI.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFile.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileHandler.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/LoadedFileManager.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/PhysicalVolumeRouter.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/asset/SystemPath.h)

# 自动序列化：反射内核 + 编解码后端三层齐全。
# 内核（Archive/Container/EnumMeta/MetaSchema/Traits/Value）只依赖 CMCore 的 type/utf、
# CMCoreType 的 color、CMMath 的 VectorTypes，与格式无关。
# 后端（TomlCodec/FlatCodec/ArchiveFile）各自引入一个第三方库：
#   TomlCodec  -> toml11（vcpkg，header-only，`<toml.hpp>`）
#   FlatCodec  -> flatbuffers（vcpkg，header-only 的 flatbuffers::flatbuffers）
#   ArchiveFile-> 只用上面两个后端 + CMCore 的文件 IO，无额外第三方依赖
# 因此本库对这两个包是**硬依赖**，在文件末尾以 PUBLIC 链路传播给消费方。
SET(ASSETS_MANAGE_SERIALIZE_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Archive.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/ArchiveFile.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Container.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/EnumMeta.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/FlatCodec.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/MetaSchema.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/TomlCodec.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Traits.h
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/serialize/Value.h)

# 对象生命周期基类独立成组：MetaObject 的路径保持 hgl/object，与 CMCore 的
# Object/RefObject 同一命名空间目录，消费方 include 无需改动。
SET(ASSETS_MANAGE_OBJECT_HEADER
                        ${CMASSETS_MANAGE_ROOT_INCLUDE_PATH}/hgl/object/MetaObject.h)

set(ASSETS_MANAGE_SOURCE    AssetByteSource.cpp
                            AssetCPUTaskPool.cpp
                            AssetCPUTasks.cpp
                            AssetDependencyGraph.cpp
                            AssetFactory.cpp
                            AssetID.cpp
                            AssetIndex.cpp
                            AssetIODispatcher.cpp
                            AssetManifest.cpp
                            AssetSource.cpp
                            AssetSourceFilesystem.cpp
                            AssetStream.cpp
                            AssetURI.cpp
                            LoadedFile.cpp
                            LoadedFileManager.cpp
                            MetaObject.cpp
                            PhysicalVolumeRouter.cpp
                            TomlCodec.cpp
                            FlatCodec.cpp
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

# 编解码后端的第三方依赖 —— 纯实现细节，因此是 PRIVATE。
#
# hgl/serialize/TomlCodec.h 与 hgl/serialize/FlatCodec.h 都是纯 HGL 接口：
# toml11 的 DOM 转换与 flatbuffers 的字节搬运分别下沉在 TomlCodec.cpp /
# FlatCodec.cpp 里。消费方 include 这两个头**不需要**这两个包，
# 于是它们不会出现在任何下游的 include 路径或链接命令里。
#
# 两者都由 vcpkg 提供（经 CMAKE_TOOLCHAIN_FILE 的 CMAKE_PREFIX_PATH 找到），
# 因此本库只能在带 vcpkg 工具链的配置里构建（ULRE 主仓一直是这种配置）。
find_package(toml11 CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
target_link_libraries(CMAssetsManage PRIVATE toml11::toml11)
target_link_libraries(CMAssetsManage PRIVATE flatbuffers::flatbuffers)

# ── 离线工具 ────────────────────────────────────────────────────────────────
#
# 放在 add_cm_library 之后：工具要链 CMAssetsManage，目标必须先存在。
# 开关默认 ON —— 工具不参与运行时，多编一个 exe 的代价远低于「要用的时候
# 发现没编」；需要精简构建时可 -DCMASSETS_BUILD_TOOLS=OFF。
option(CMASSETS_BUILD_TOOLS "构建 CMAssetsManage 的离线工具" ON)

IF(CMASSETS_BUILD_TOOLS)
    add_subdirectory(Tools/AssetManifestTool)
ENDIF()