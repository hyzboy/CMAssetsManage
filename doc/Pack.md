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
| `geo:pack(res):models/hero` | **首选**：类型 + 包裹 + 无扩展名逻辑路径 |
| `geo:pack(res):id:0190f2c1…` | 按资产身份寻址，与路径层次无关 |
| `geo:pack(res):models/hero.geo.toml` | 历史写法（带物理扩展名），仍可解析 |
| `pack(res):models/hero` | 省略类型，由调用方 `default_type` 补齐 |
| `geo:pack:foo` | **不是**包裹（`pack` 单独一段非法），整段落回 target |

拼写规则：大小写不敏感的 `pack(` … `)`，名字为 1..32 个 `[A-Za-z0-9_-]`。
名字保留原拼写并由挂载表区分大小写。

### 为什么定位段**不该**带扩展名

`target` 里的 `.geo.toml` 是**物理编码格式**，不是资产名。写死它意味着：

- 发行期把 TOML 换成 FlatBuffers，所有引用该资产的场景与材质都要重写；
- 上层被迫知道「这个资产盘上是文本还是二进制」，而这本该是实现细节。

首选形态是 `models/hero`：类型由第一段声明，格式由索引表给出。
带扩展名的写法仍然能解析 —— 包裹分支会把它推导成同一个逻辑路径
（详见「索引与清单」）。

### 按身份寻址

`id:<32 位十六进制>` 直接指向资产身份（见 `AssetID.h`），绕过路径层次。
它存在的理由是**重命名/挪目录不该影响引用**：跨资产引用一律记身份，
只有在人需要「看/写某个具体文件」时才用逻辑路径。

判据刻意收窄到定长 35 字符，因此一个叫 `id:xxx` 的目录不会被误判。
这条路**不做任何回落** —— ID 查不到就是不存在，否则「引用失效」会被
悄悄替换成「读到了另一个资产」。

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

// 目录包裹（缺省：递归扫描建索引 + 读写 .asset_manifest.toml）
AssetSource *src = CreateSourceByFilesystem("res", OS_TEXT("e:/ulre/res"), true);

// 只要纯路径访问、不建索引（挂载原始素材目录时用）
AssetsSourceMountOptions plain;
plain.build_index   = false;
plain.load_manifest = false;
plain.save_manifest = false;
AssetSource *raw = CreateSourceByFilesystem("raw", OS_TEXT("e:/ulre/raw"), true, plain);

// 取流：所有 loader 都必须走这里，不要直接碰 filesystem
AnsiString error;
io::InputStream *is = OpenAssetStream(OS_TEXT("geo:pack(res):models/hero"),
                                     AssetType::Unknown, &error);
if (is) { /* ... */ delete is; }

UnregisterAssetsSource("res");   // 注销并释放
```

挂载表由读写锁保护，`GetSource()` 可在任意线程调用；`AssetSource` 实例本身
是否线程安全由各后端声明。

挂载是**先建索引、再登记**的顺序：登记之后并发的 `GetSource()` 立刻就能查到，
若索引还没建完，那一瞬间的「按逻辑路径取资产」会全部落空。

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

## 索引与清单

### 为什么要索引

在引入无扩展名逻辑寻址之前，每一层都在猜：先试 `.geo.toml`，不行再试
`.geo.fb`，再不行试 `.bin`…… 这套「试探」有三条无法回避的代价：

1. **每一个未命中的试探都是一次真实 IO**。十万级资产上冷启动直接变成秒级；
2. **开发期与发行期行为分叉**。真实包裹里没有目录树、也不允许逐个试探，
   于是「开发期能跑、发行期炸」的缺陷只在出包后暴露；
3. **物理格式泄漏进上层契约**。URI 里写死 `.geo.toml`，发行换格式就要
   重写所有引用方。

索引表把这三点一次性解决：挂载时**一次**扫完（目录后端）或**一次**读完包头
（打包后端），此后所有定位都是内存查表。

```
  AssetID          --\
                     +--> AssetIndexEntry --> physical_path --> 字节流
  (type, logical)  --/
```

- **按 ID**：引用关系的锚点。改名、挪目录、换格式都不会断链；
- **按 (类型, 逻辑路径)**：给人用的入口。类型必须进键 —— `models/hero`
  同时可能是几何与材质，只有路径不足以定位。

写入收拢在唯一的 `AssetIndexTable::AddOrUpdate()` 里：调用方拿不到裸容器，
也就没机会只写一半。三种陈旧映射在这里一并拆除：同 ID 换路径、
同路径换 ID、同路径换类型。

### 逻辑路径推导

`DeriveLogical()` 负责 **物理文件名 → (类型, 逻辑路径)**，先剥格式后缀
（`toml` / `fb` / `flat` / `bin`），再剥类型后缀：

| 物理文件 | 推导结果 |
|---|---|
| `models/hero.geo.toml` | `geo` : `models/hero` |
| `models/hero.geo.fb` | `geo` : `models/hero` |
| `tex/brick.tex` | `tex` : `tex/brick` |
| `models/hero.v2.geo.toml` | `geo` : `models/hero.v2` |
| `data/blob.bin` | 失败（不是资产） |

「名叫 `geo` 的目录」（`models/geo`）会被误剥成 `models` —— 这是纯文本层面
无法消除的歧义。可接受的理由：查表未命中时取流入口会回落按调用方给的
原始路径直通，故仍可打开；反过来（不剥类型后缀）则会让所有常规资产全部失配。

### `.asset_manifest.toml`

目录包裹的物理载体是「一堆散落的文件」，天然缺少打包文件那种自带索引头。
清单就是**目录包裹版的包头**，不是某个后端的私有格式。

```toml
manifest_version = 1
pack_name        = "res"
hash_algorithm   = "BLAKE3"     # 包级统一的内容哈希算法，作用于所有条目
updated_at       = 1730000000

[[assets]]
id    = "0190f2c1…"             # 32 位无连字符十六进制（UUIDv7）
type  = "geo"
path  = "models/hero"           # 逻辑路径，无扩展名
file  = "models/hero.geo.toml"  # 包内物理相对路径
size  = 40960
mtime = 1729900000
hash  = "5d41…"                 # 可为缺省（尚未计算）
```

三条刻意的规则：

- **`hash_algorithm` 是包级而不是条目级**。一个包里混用两种算法，等于
  「同一列数字有两套量纲」，排序/去重/比对都多一个维度；真实需求里也没有
  「这十张贴图用 BLAKE3、那十张用 MD5」的场景；
- **`hash` 缺省 = 尚未计算**，不是「算出来是空」。写出时不落键，让
  「只做了挂载扫描、还没跑离线工具」的中间状态保持干净；
- **`id` 只在新文件出现时生成**。已存在的文件必须复用清单里的 ID ——
  这是引用不断链的唯一保证。

### 挂载期对拍

目录挂载时递归扫描并与清单对拍，三条规则缺一都会让身份或指纹失去意义：

- 物理文件已在索引里 ⇒ **复用 ID**（改名、改内容都不该换身份）；
- `size` 与 `mtime` 都没变 ⇒ 连同 `content_hash` 一起复用；任一变化则旧哈希
  **必须作废** —— 清单长期携带一个「看起来有、实际是上一版」的指纹比没有更危险；
- 索引里存在、但本次没扫到的条目 ⇒ 自然消失（新表是重建的）。

以 `.` 开头的目录与文件一律跳过：`.asset_manifest.toml`、`.git`、编辑器临时
文件都不是资产，收进索引会让每次保存都触发一次清单重写。

换入索引用 `AssetIndexTable::CopyFrom()`：一次写锁内整体替换。逐条
`Clear`/`Add` 会让并发读者看到半张索引 —— 那比看到旧索引糟得多。

### 离线工具 `AssetManifestTool`

运行期**不计算**内容哈希（十万个资产每次启动读一遍是分钟级开销），
内容指纹由独立 CLI 在「能容忍慢」的时机产生：

```
AssetManifestTool <目录> [--check-only | --update] [--no-hash] [--algo=NAME]
```

| 选项 | 作用 |
|---|---|
| `--update`（默认） | 校验并原子写回清单 |
| `--check-only` | 只校验；有差异或哈希失败时退出码 1（CI 闸门） |
| `--no-hash` | 跳过内容哈希，只做结构校验 |
| `--algo=<NAME>` | 强制换算法全包重算，并同步改写头部字段 |

退出码：`0` 一致 · `1` 存在差异或哈希失败 · `2` 参数/IO 错误。

工具刻意**复用** `CreateSourceByFilesystem()` 取扫描结果，而不是自己再写一遍：
工具与运行期一旦对「什么算资产」有分歧，清单就会在两者之间反复抖动，
而那种抖动极难定位。

## 迁移到真实包裹文件

1. 新增一个 `AssetSource` 派生类（例如 `AssetsSourceBundle`），实现上表四个方法；
   `Open()` 返回 `MemoryInputStream` 或包内切片流。
2. 覆写 `GetIndexTable()` 返回从包头反序列化出的索引表 —— `Exists(const AssetID&)` /
   `Open(AssetType, logical)` / `ResolvePath(...)` 六个虚函数会自动获得，
   不必逐个覆写（基类实现全部只经过 `GetIndexTable()` 这一条通道）。
3. 新增 `CreateSourceByBundle(name, filename, ...)`，用 `RegisterAssetsSource()`
   登记，替换原来的 `CreateSourceByFilesystem()` 调用。
4. 资产路径、loader、序列化代码**一行都不用改**。

打包器（Cooker）需要做的：把 `.asset_manifest.toml` 的条目编译成紧凑的
二进制包头（`id → offset/size/type`），`physical_path` 可以丢弃或只留
逻辑路径供诊断 —— 打包文件本来就不需要目录层次。

## 相关文件

- `inc/hgl/asset/AssetID.h` —— 128 位资产身份（UUIDv7）
- `inc/hgl/asset/AssetIndex.h` —— 索引项与双主键索引表、逻辑路径推导
- `inc/hgl/asset/AssetManifest.h` —— `.asset_manifest.toml` 读写与格式规约
- `inc/hgl/asset/AssetURI.h` —— URI 解析与 `pack(<name>)` 段、ID 寻址
- `inc/hgl/asset/AssetSource.h` —— 后端抽象、路径规范化、挂载表、索引能力
- `inc/hgl/asset/AssetStream.h` —— 统一取流入口 `OpenAssetStream()`
- `inc/hgl/asset/AssetDependencyGraph.h` —— 资产正反向依赖图
- `src/AssetSourceFilesystem.cpp` —— 目录包裹实现（扫描 + 对拍）
- `src/Tools/AssetManifestTool/` —— 离线清单校验与内容哈希工具
- `doc/AssetDependency.md` —— 依赖机制
- `test/TestAssetPack.cpp` / `TestAssetIndex.cpp` / `TestAssetManifest.cpp` —— 契约回归基线
