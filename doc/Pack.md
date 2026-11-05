# 包裹（Pack）机制

## 概念

「包裹」是一个**命名挂载点**。资产 URI 里只出现名字，真实位置由运行时挂载表决定：

```
geo:pack(res):geo/unit_cube.geo.toml
        ^^^^^^^^
        pack(<name>)  ->  AssetSource("res")
                              ├── 目录后端： e:\ulre\res\geo\unit_cube.geo.toml   （当前）
                              └── 打包文件后端： res.pak 内索引表查表               （以后）
```

上层资产路径**永远不含真实位置**，因此从目录切换到真实包裹文件时，URI 与资产
文件都不需要改。这就是「位置透明」。

## URI 语法

沿用现有 `type:source:target` 三段式，来源段扩展为 `pack(<name>)`：

| 写法 | 含义 |
|---|---|
| `geo:pack(res):geo/a.geo.toml` | 类型 + 包裹 + 包裹内相对路径 |
| `pack(res):geo/a.geo.toml` | 省略类型，由调用方 `default_type` 补齐 |
| `geo:pack:foo` | **不是**包裹（`pack` 单独一段非法），整段落回 target |

拼写规则：大小写不敏感的 `pack(` … `)`，名字为 1..32 个 `[A-Za-z0-9_-]`。
名字保留原拼写并由挂载表区分大小写。

## 路径规则

包裹内路径由 `NormalizePackPath()` 统一处理：

- `\` 与 `/` 都视为分隔符，输出统一为 `/`；
- 合并连续分隔符，忽略 `.` 段；
- **拒绝** `..`（不允许越出挂载根）；
- **拒绝**绝对路径与 `C:` 盘符路径；
- 不做大小写折叠（由各后端的查表键策略决定）。

## 挂载

```cpp
using namespace hgl::asset;

// 目录包裹
AssetSource *src = CreateSourceByFilesystem("res", OS_TEXT("e:/ulre/res"), true);

// 取流：所有 loader 都必须走这里，不要直接碰 filesystem
AnsiString error;
io::InputStream *is = OpenAssetStream(OS_TEXT("geo:pack(res):geo/a.geo.toml"),
                                     AssetType::Unknown, &error);
if (is) { /* ... */ delete is; }

UnregisterAssetsSource("res");   // 注销并释放
```

挂载表由读写锁保护，`GetSource()` 可在任意线程调用；`AssetSource` 实例本身
是否线程安全由各后端声明。

## 后端能力契约

目录后端与打包文件后端必须实现同一组语义：

| 方法 | 说明 |
|---|---|
| `Exists(path)` | 路径是否存在 |
| `Stat(path, out)` | `name / path / size / mtime / is_directory` |
| `Open(path)` | 返回只读字节流，调用方负责 `delete` |
| `ListDirectory(path, out)` | **不递归**列出直接子项，含子目录；空串表示挂载根 |

`ListDirectory` 不是可选装饰：真实包裹文件必须在包内索引里回答「这个目录下
有什么」，否则将来切换后端时，所有依赖枚举的调用点都会失效。

## 迁移到真实包裹文件

1. 新增一个 `AssetSource` 派生类（例如 `AssetsSourceBundle`），实现上表四个方法；
   `Open()` 返回 `MemoryInputStream` 或包内切片流。
2. 新增 `CreateSourceByBundle(name, filename, ...)`，用 `RegisterAssetsSource()`
   登记，替换原来的 `CreateSourceByFilesystem()` 调用。
3. 资产路径、loader、序列化代码**一行都不用改**。

## 相关文件

- `inc/hgl/asset/AssetURI.h` —— URI 解析与 `pack(<name>)` 段
- `inc/hgl/asset/AssetSource.h` —— 后端抽象、路径规范化、挂载表
- `inc/hgl/asset/AssetStream.h` —— 统一取流入口 `OpenAssetStream()`
- `src/AssetSourceFilesystem.cpp` —— 目录包裹实现
- `test/TestAssetPack.cpp` —— 契约回归基线
