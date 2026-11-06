# 资产身份与依赖机制

本文说明 ULRE 资产管线的两个基础约定：

1. **身份（`AssetID`）与内容指纹（ContentHash）是两个东西**；
2. **资产之间构成有向图，且正反两个方向都必须显式维护**。

---

## 一、身份 vs 内容指纹

一个资产在管线里要回答两个完全不同的问题：

| 问题 | 答案载体 | 什么时候变 |
|---|---|---|
| 「**这一个**资产是谁？」 | `AssetID`（128 位 UUIDv7） | 只有删除/重建资产时才变 |
| 「它的内容还是上次那份吗？」 | `content_hash`（包级算法，默认 BLAKE3） | 内容一改就变 |

把两者合成一个值看着省事，实际会同时废掉两边：

- **用内容哈希当身份** ⇒ 作者改一个数值，哈希就变，所有引用该资产的场景/材质
  全部断链。编辑器里改一行数据能炸掉半个工程；
- **用身份当内容指纹** ⇒ 内容改了身份不变，增量编译无法判断「要不要重导」。

因此二者永不互相替代：`AssetID` 是引用关系的锚点，`content_hash` 只是索引表里的
一列，且**只由离线工具产生**（见下文）。

### 为什么是 128 位、为什么是 UUIDv7

- 128 位是「碰撞概率可以当不存在」与「本地存储开销」的唯一平衡点：
  64 位在十万级资产量下生日碰撞已不可忽略，256 位让索引表膨胀一倍而无收益；
- **v7 而非 v4**：v7 = 48 位毫秒时间戳 + 版本位 + 62 位随机数。它同时是
  **可排序**的 —— 实现上同一毫秒内 `rand_b` 逐次 `+1`，因此
  **严格单调递增**。「按 ID 排序 = 按创建时间排序」让索引表与打包文件天然有序，
  二分查找与磁盘局部性直接受益；v4 则完全随机，只能整表哈希。

系统时钟回拨时实现会**冻结在最后见过的时间戳**上：宁可让若干毫秒「挤在一起」，
也不让身份序倒退 —— 倒退会让有序索引表与追加写的打包文件同时失效。

### 文本形态

| 方法 | 形态 | 用在哪 |
|---|---|---|
| `ToString()` | 32 位小写十六进制，无连字符 | 清单、URI、日志（定长、无分隔符） |
| `ToUUIDString()` | RFC 9562 的 `8-4-4-4-12` | 只给「人要看」的场合 |

`Parse()` 两种形态都收，但**严格拒绝**非法字符、长度不足与全零 ——
全零在身份语义里是「未分配」，不允许从外部文本注入。

### 内容哈希是**包级**属性

`.asset_manifest.toml` 头部的 `hash_algorithm` 作用于该包的**所有**条目。
一个包里混用两种算法，等于「同一列数字有两套量纲」：比对时不单要知道值还要
知道各自算法，任何一次排序/去重/比对都多一个维度。真实需求里也没有
「这十张贴图用 BLAKE3、那十张用 MD5」的场景。

缺省 `BLAKE3`（`CMUtil` 的 `hgl::util::hash::Algorithm::BLAKE3`，256 位）。

### 谁产生、谁消费

```
   ┌───────────────────────┐   产生（挂载，只读元信息）
   │ AssetSourceFilesystem │──────────────► id / logical_path / physical_path / size / mtime
   └───────────────────────┘
   ┌───────────────────────┐   产生（离线，读全部内容）
   │ AssetManifestTool     │──────────────► content_hash
   └───────────────────────┘
   ┌───────────────────────┐   只读
   │ 运行时（引擎 / 编辑器）│◄────────────── 索引表
   └───────────────────────┘
```

运行期**绝不**计算内容哈希：十万级资产每次启动读一遍是分钟级开销。
需要指纹的地方（一致性校验、出包、CI 闸门）都走 `AssetManifestTool`。

---

## 二、依赖图

资产之间构成有向图：场景引用材质，材质引用贴图。两个方向回答的问题互不重叠：

| | 正向（Dependencies） | 反向（Dependents） |
|---|---|---|
| 装载 | 级联装载与拓扑排序：装 A 之前必须先把 B 准备好 | 引用计数卸载：没有任何反向引用才可从内存释放 |
| 热更 | 自身依赖是否已就绪 | **波及追踪**：贴图改了，顺着反向边找到所有材质并重建 |
| 打包 | 从关卡出发求可达闭包，决定哪些资产进同一个包 | 剔除没有任何反向引用的孤儿资产 |
| 编辑器 | 列出「本资产引用了谁」 | **安全删除**：删/挪一个资产前警告「被 N 个资产引用」 |

### 为什么必须集中管理

反向边**不**能靠「扫描全表临时算」：那需要遍历所有资产的依赖列表，
在热更这种毫秒级路径上完全不可接受。因此必须显式维护，而且必须在**同一处**
维护 —— 一旦允许各资产自己记反向引用，「A 卸载了但 B 的反向表里还留着 A」
这类不一致就会以最难复现的方式出现（悬空引用、内存泄漏、删不掉的资产）。

也刻意**不**做成 `AssetObject` 的成员：

1. 反向边要跨对象同步修改，多线程下必然出现两把锁的顺序问题；
2. 反向引用是**弱**语义（B 不拥有 A），放进成员就变成强持有，卸载时得先遍历
   所有引用方改字段 —— 那正是崩溃的温床；
3. `AssetObject` 是**可选**的多态外壳（见 `AssetObject.h`），绝大多数纯数据
   规约不继承它，图若挂在基类上，这些规约就永远进不了图。

### 三条不变量

- 每条 `forward[A]` 项都必然在 `reverse[b]` 里有一条对应的 `A`，反之亦然；
- 依赖列表**去重**且**不含自身**（自依赖既无意义，又会让拓扑排序必然失败）；
- `RegisterDependencies` 是**整体替换**而非追加：资产的依赖集合由它的内容决定，
  重载一次就该整体刷新。局部追加会让「删掉了一行引用」永远从图里消失不掉。

### 用法

```cpp
#include <hgl/asset/AssetDependencyGraph.h>

using namespace hgl::asset;

AssetDependencyGraph graph;

// 材质引用了贴图与着色器
ValueArray<AssetID> mat_deps;
mat_deps.Add(texture_id);
mat_deps.Add(shader_id);
graph.RegisterDependencies(material_id, mat_deps);

// 场景引用了材质
ValueArray<AssetID> scene_deps;
scene_deps.Add(material_id);
graph.RegisterDependencies(scene_id, scene_deps);

// 正向：本资产引用了谁（递归到传递闭包）
ValueArray<AssetID> deps;
graph.CollectDependencies(scene_id, deps, true);

// 反向：谁（间接）引用了这张贴图 —— 热更波及
ValueArray<AssetID> users;
graph.CollectDependents(texture_id, users, true);

// 安全删除判据
if (graph.HasDependents(texture_id)) { /* 拒绝删除并列出 users */ }

// 装载顺序：被依赖者在前
ValueArray<AssetID> order;
if (!graph.TopologicalOrder(order)) { /* 存在环，报错 */ }
```

### 图算法

- **传递闭包**：广度优先，结果不含起点、按发现顺序去重，带 `max_depth`
  上限 —— 数据损坏造出的长链不会打穿递归栈（BFS 的已访问集合天然做了环保护）；
- **拓扑序**：Kahn 算法。出队一个资产时所有「依赖它」的资产各少一个待满足项 ——
  反向表的用途正在这里：正向表只能回答「我依赖谁」，而推进拓扑序需要
  「谁在等我」；
- **环检测**：`TopologicalOrder` 排不完即存在环，返回 `false` 并给出能排出来的
  那部分，而不是静默给出一份错误顺序。

### 依赖从哪里来

`AssetObject` 的反射元数据已经能描述「哪些字段是资产引用」：

- `PropertyMetaInfo::flags` 含 `PF_HasAssetRef`；
- `PropertyMetaInfo::asset_ref` 给出该字段指向的资产类别（`geo` / `tex` …）。

因此**不需要**每个资产派生类手写一遍依赖解析：遍历 `ClassMetaInfo::properties`，
挑出带 `PF_HasAssetRef` 的字段，按 `offset` 取出值（`AssetID` 或 URI 字符串），
在资产装配到 `AssetState::Ready` 时一次性 `RegisterDependencies()` 即可。

---

## 三、相关文件

- `inc/hgl/asset/AssetID.h` / `src/AssetID.cpp` —— 身份类型、UUIDv7 生成、ID 寻址拼写
- `inc/hgl/asset/AssetIndex.h` / `src/AssetIndex.cpp` —— 索引项、双主键表、逻辑路径推导
- `inc/hgl/asset/AssetManifest.h` / `src/AssetManifest.cpp` —— 清单格式与读写
- `inc/hgl/asset/AssetDependencyGraph.h` / `src/AssetDependencyGraph.cpp` —— 依赖图
- `src/Tools/AssetManifestTool/` —— 离线校验与内容哈希
- `test/TestAssetID.cpp` / `TestAssetIndex.cpp` / `TestAssetManifest.cpp` /
  `TestAssetDependencyGraph.cpp` —— 契约回归基线
