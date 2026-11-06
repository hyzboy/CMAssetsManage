#pragma once

/**
 * 资产双向依赖图 —— 正向「引用了谁」与反向「被谁引用」。
 *
 * ── 为什么必须是双向，而且必须集中管理 ────────────────────────────────────
 *
 * 资产之间构成有向图：场景引用材质，材质引用贴图。两个方向回答的问题**互不重叠**：
 *
 * | | 正向（Dependencies） | 反向（Dependents） |
 * |---|---|---|
 * | 装载 | 级联装载与拓扑排序：装 A 之前必须先把 B 准备好 | 引用计数卸载：没有任何反向引用才可从内存释放 |
 * | 热更 | 自身依赖是否已就绪 | **波及追踪**：贴图改了，顺着反向边找到所有材质并重建 |
 * | 打包 | 从关卡出发求可达闭包，决定哪些资产进同一个包 | 剔除没有任何反向引用的孤儿资产 |
 * | 编辑器 | 列出「本资产引用了谁」 | **安全删除**：删/挪一个资产前警告「被 N 个资产引用」 |
 *
 * 反向边**不**能靠「扫描全表算出来」临时拼：那需要遍历所有资产的依赖列表，
 * 在热更这种毫秒级路径上完全不可接受。因此必须显式维护，而且必须在**同一处**
 * 维护 —— 一旦允许各资产自己记反向引用，「A 卸载了但 B 的反向表里还留着 A」
 * 这类不一致就会以最难复现的方式出现（悬空引用、内存泄漏、删不掉的资产）。
 *
 * ── 为什么不放进 `AssetObject` ────────────────────────────────────────────
 *
 * 把两个数组塞进 `AssetObject` 看着更「面向对象」，实际会带来三个问题：
 *
 *   1. 反向边需要跨对象同步修改，多线程下必然出现两把锁的获取顺序问题；
 *   2. 反向引用是**弱**语义（B 不拥有 A），但放进 A 的成员就变成了强持有，
 *      卸载一个资产要先遍历所有引用它的对象去改字段 —— 那正是崩溃的温床；
 *   3. `AssetObject` 是**可选**的多态外壳（见 `AssetObject.h`），绝大多数
 *      纯数据规约不继承它。依赖图若挂在基类上，这些规约就永远进不了图。
 *
 * 因此：`AssetObject` 只管自己的身份，图的维护全部集中在这里，读者与写者
 * 各持一把读写锁。
 *
 * ── 三条不变量 ────────────────────────────────────────────────────────────
 *
 * · 每条 `forward[A]` 项都必然在 `reverse[b]` 里有一条对应的 `A`，反之亦然；
 * · 依赖列表**去重**且**不含自身**：自依赖既无意义，也会让拓扑排序必然失败；
 * · `RegisterDependencies` 是**整体替换**而不是追加：资产的依赖集合由它自己
 *   的内容决定，重载一次就该整体刷新。局部追加会让「删掉了一行引用」永远
 *   从图里消失不掉。
 *
 * ── 线程约定 ──────────────────────────────────────────────────────────────
 *
 * 所有公开方法内部加锁，可并发调用。返回 `ValueArray<AssetID>` 的都是**副本**，
 * 调用方拿到后无需持锁。`AssetID` 是 16 字节平凡类型，`ValueArray` 的拷贝
 * 就是一次块拷贝，因此这里的「返回副本」不构成负担。
 */

#include <hgl/asset/AssetID.h>

#include <hgl/sync/RWLock.h>
#include <hgl/type/UnorderedMap.h>
#include <hgl/type/ValueArray.h>

namespace hgl::asset
{
    /**
     * 资产依赖图。
     *
     * 不可拷贝 —— 两张图之间的正反向一致性无法通过逐字段复制保持。
     */
    class AssetDependencyGraph
    {
        mutable RWLock lock;

        /// 正向：`A -> [A 直接引用的资产]`
        UnorderedMap<AssetID, ValueArray<AssetID>> forward;

        /// 反向：`B -> [直接引用 B 的资产]`
        UnorderedMap<AssetID, ValueArray<AssetID>> reverse;

    public:

        AssetDependencyGraph()=default;
        ~AssetDependencyGraph()=default;

        AssetDependencyGraph(const AssetDependencyGraph &)=delete;
        AssetDependencyGraph &operator=(const AssetDependencyGraph &)=delete;

    public:     // 写入

        /**
         * 注册（**整体替换**）一个资产的直接依赖。
         *
         * 列表内的重复项会被去掉，自身项会被忽略（自依赖既无意义又会让拓扑
         * 排序必然失败）。被引用的资产即使从未注册过依赖，也会出现在反向图里
         * —— 「有东西引用它」本身就是需要记录的事实。
         *
         * @param asset        资产身份；全零身份被拒绝（未分配身份的资产不该入图）
         * @param dependencies 它直接引用的资产
         * @return `asset` 为全零时返回 false
         */
        bool RegisterDependencies(const AssetID &asset, const ValueArray<AssetID> &dependencies);

        /**
         * 移除一个资产并**同时**拆掉所有相关边。
         *
         * 三件事一起做，缺一就会留下悬空：
         *   · 拆掉它的正向边，并从它每个依赖的反向表里删掉它；
         *   · 拆掉它的反向边，并从它每个引用方的正向表里删掉它；
         *   · 移除它自身的两条记录。
         *
         * @return 图中原本存在该资产时返回 true
         */
        bool UnregisterAsset(const AssetID &asset);

        void Clear();

    public:     // 查询

        /// 已注册依赖的资产数（有正向边的节点）。
        int GetRegisteredCount() const;

        /// 图中出现过的节点总数（正向 ∪ 反向）。
        int GetNodeCount() const;

        /// 取直接依赖。没有记录时返回 false 并清空 `out`。
        bool GetDependencies(const AssetID &asset, ValueArray<AssetID> &out) const;

        /// 取直接引用方。没有记录时返回 false 并清空 `out`。
        bool GetDependents(const AssetID &asset, ValueArray<AssetID> &out) const;

        int  GetDependentCount(const AssetID &asset) const;
        int  GetDependencyCount(const AssetID &asset) const;

        /// 有任何资产直接引用它 —— 这是「能否安全删除/卸载」的判据。
        bool HasDependents(const AssetID &asset) const;

    public:     // 图算法

        /**
         * 求传递闭包（广度优先，结果不含起点，按发现顺序去重）。
         *
         * @param asset     起点
         * @param out       追加写入（不清空调用方已有内容）
         * @param recursive false 时只取直接边
         * @param max_depth 深度上限；`0` 表示不限。设上限是为了在数据损坏
         *                  （意外成环）时仍能返回一份可用的部分结果
         * @return 本次新增的项数
         */
        int CollectDependencies(const AssetID &asset, ValueArray<AssetID> &out,
                                bool recursive = true, int max_depth = 0) const;

        /// 反向传递闭包：谁（间接）引用了它。热更波及与安全删除都走这里。
        int CollectDependents(const AssetID &asset, ValueArray<AssetID> &out,
                              bool recursive = true, int max_depth = 0) const;

        /**
         * 拓扑序：**被依赖者排在依赖者之前**（Kahn 算法）。
         *
         * 顺序可直接用于装载：顺着这个序列走，装载任何一项时它的依赖都已就位。
         *
         * @param out 成功时写入完整序列；检测到环时写入「能排出来的那部分」并返回 false
         * @return 无环返回 true
         */
        bool TopologicalOrder(ValueArray<AssetID> &out) const;

        /// 图中是否存在环（`TopologicalOrder` 的成功性判据）。
        bool IsAcyclic() const;
    };//class AssetDependencyGraph
}//namespace hgl::asset
