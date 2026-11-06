#pragma once

/**
 * 资产索引表 —— 逻辑身份 / 逻辑路径 → 物理载体的唯一路由器。
 *
 * ── 它解决什么问题 ────────────────────────────────────────────────────────
 *
 * 在引入「无扩展名逻辑寻址」之前，上层拿到的是一串物理路径（`models/hero.geo.toml`），
 * 于是每一层都在猜：先试 `.geo.toml`，不行再试 `.geo.fb`，再不行试 `.bin`……
 * 这套「试探」在开发期也许能跑，但它有三条无法回避的代价：
 *
 *   1. **每一个未命中的试探都是一次真实 IO**。十万级资产上冷启动直接变成秒级；
 *   2. **开发期与发行期行为分叉**。真实包裹（pak）里没有目录树、也不允许逐个试探，
 *      于是「开发期能跑、发行期炸」的缺陷只在出包后暴露；
 *   3. **物理格式泄漏进上层契约**。URI 里写死 `.geo.toml`，发行改成 FlatBuffers
 *      就要重写所有引用它的场景与材质文件。
 *
 * 索引表把这些一次性解决：挂载时**一次**扫完（目录后端）或**一次**读完包头
 * （打包后端），此后所有定位都是内存查表。上层只写 `geo:pack(res):models/hero`，
 * 与「盘上到底是 TOML 还是 FlatBuffers」再无关系。
 *
 * ── 三条正交的索引 ────────────────────────────────────────────────────────
 *
 * ```
 *   AssetID            ──┐
 *                        ├──> AssetIndexEntry ──> physical_path  ──> 字节流
 *   (type, logical)    ──┘
 * ```
 *
 * · **按 ID**：引用关系的锚点。场景文件引用材质、材质引用贴图，全部记 ID。
 *   资产被改名、挪目录、换格式，引用方一行都不用改；
 * · **按 (类型, 逻辑路径)**：给人用的入口（URI、编辑器、日志）。
 *   类型必须进键 —— `models/hero` 同时可能是几何与材质，只有路径不足以定位。
 *
 * 两个键指向同一个 `AssetIndexEntry`，因此**任何增删都必须同时维护两侧**，
 * 这正是本类把写入收拢成唯一一个 `AddOrUpdate()` 的原因：调用方拿不到裸容器，
 * 也就没有机会只写一半。
 *
 * ── 与内容哈希的分工 ──────────────────────────────────────────────────────
 *
 * `content_hash` 是本表**唯一**一个「不由挂载期产生」的字段：它由离线工具
 * `AssetManifestTool` 按包级统一算法（默认 BLAKE3）算出后回写清单。
 * 运行时只读它、从不计算 —— 挂载期扫一遍目录就要等 BLAKE3 把所有资产读一遍，
 * 那是不可接受的启动开销。
 */

#include <hgl/asset/AssetID.h>
#include <hgl/asset/AssetURI.h>

#include <hgl/sync/RWLock.h>
#include <hgl/type/ManagedArray.h>
#include <hgl/type/String.h>
#include <hgl/type/UnorderedMap.h>

namespace hgl::asset
{
    /**
     * 索引里的一项：一个资产的「身份 + 逻辑位置 + 物理载体 + 元信息」。
     *
     * 值语义。物理载体字段刻意保留**相对路径**而非绝对路径：打包文件后端根本没有
     * 绝对路径，把所有后端拉平成同一种表示，上层才可能对它们一视同仁。
     */
    struct AssetIndexEntry
    {
        AssetID     id;                             ///<唯一身份（UUIDv7）
        AssetType   type = AssetType::Unknown;      ///<资产类型（与 URI 第一段一致）

        U8String    logical_path;                   ///<逻辑路径，无扩展名，'/' 分隔
        U8String    physical_path;                  ///<后端内的相对路径，含扩展名

        uint64      size  = 0;                      ///<字节数；后端不知情时为 0
        uint64      mtime = 0;                      ///<后端自己的修改时间刻度；不知情时为 0

        /**
         * 内容哈希的十六进制文本（小写，无分隔符）。
         *
         * **空串 = 尚未计算**，与「算出来是空」不可能混淆。长度由包级
         * `hash_algorithm` 决定（BLAKE3 ⇒ 64 字符，MD5 ⇒ 32），因此这里不存长度。
         */
        AnsiString  content_hash;

        bool HasContentHash() const { return !content_hash.IsEmpty(); }
    };//struct AssetIndexEntry

    // ── 逻辑路径推导 ────────────────────────────────────────────────────────

    /**
     * 后缀是不是「只表示物理编码格式、与资产类型无关」的容器后缀。
     *
     * 目前认：`toml`（作者侧文本）、`fb` / `flat`（FlatBuffers）、`bin`（裸二进制）。
     * 判据必须小而稳：加一个不认识的后缀进这张表，等于把「名叫 `x.data` 的资产」
     * 悄悄改名成 `x`。
     */
    bool IsAssetFormatSuffix(const U8String &suffix);

    /**
     * 从物理相对路径推导出「资产类型 + 逻辑路径」。
     *
     * ```
     *   models/hero.geo.toml  --declared=Geometry-->  (Geometry, models/hero)
     *   models/hero.geo.fb    --declared=Unknown -->  (Geometry, models/hero)
     *   tex/brick.tex         --declared=Unknown -->  (Texture , tex/brick )
     *   models/hero           --declared=Geometry-->  (Geometry, models/hero)
     *   data/blob.bin         --declared=Unknown -->  失败（不是资产）
     * ```
     *
     * 剥离顺序是**先格式、后类型**：`hero.geo.toml` 先掉 `.toml` 得到
     * `hero.geo`，此时末尾 `geo` 才被认成类型后缀。若是「名叫 `geo` 的目录」，
     * 那么 `models/geo` 会被误剥成 `models` —— 这是唯一无法在纯文本层面消除的
     * 歧义。之所以可以接受：查表未命中时上层会回落「按原始路径直通」，
     * 于是 `models/geo` 仍然打得开；反过来（不剥）则会让所有常规资产全部失配。
     *
     * @param physical_path 后端内的相对路径（'/' 或 '\\' 分隔均可）
     * @param declared_type URI 已声明的类型；`Unknown` 表示交给后缀判定
     * @param out_type      推导出的类型；失败时为 `Unknown`
     * @param out_logical   推导出的逻辑路径；失败时被清空
     * @return 推导出确定类型且逻辑路径非空时返回 true
     */
    bool DeriveLogical(const U8String &physical_path, AssetType declared_type,
                       AssetType &out_type, U8String &out_logical);

    /**
     * 拼出索引主键：`<类型段>:<逻辑路径>`（如 `geo:models/hero`）。
     *
     * 类型段用 `AssetURI` 的**唯一拼写表**（`GetAssetTypeName`），不另起一份。
     */
    U8String MakeIndexPathKey(AssetType type, const U8String &logical_path);

    /**
     * 资产索引表：线程安全的双主键索引。
     *
     * 线程约定：所有公开方法内部加锁，可并发调用；`GetAllEntries()` 返回的不是
     * 内部容器而是**快照副本**，因此调用方拿到结果后无需继续持锁。
     * 本类不可拷贝 —— 双主键之间的对应关系不能靠逐字段复制保持。
     */
    class AssetIndexTable
    {
        mutable RWLock lock;

        UnorderedMap<AssetID, AssetIndexEntry> by_id;
        UnorderedMap<U8String, AssetID>        by_path;

        /// 包级统一的内容哈希算法名（`BLAKE3` / `MD5` / ...）。空 = 尚未声明。
        AnsiString hash_algorithm;

    public:

        AssetIndexTable()=default;
        ~AssetIndexTable()=default;

        AssetIndexTable(const AssetIndexTable &)=delete;
        AssetIndexTable &operator=(const AssetIndexTable &)=delete;

    public:

        int  GetCount() const;
        bool IsEmpty() const;
        void Clear();

        /// 包级内容哈希算法名。返回副本 —— 引用会在解锁后失效。
        AnsiString GetHashAlgorithm() const;
        void       SetHashAlgorithm(const AnsiString &algo);

    public:     // 写入

        /**
         * 插入或覆盖一项，**同时**维护 ID 与路径两侧。
         *
         * 三种需要清理的陈旧映射都在这里一并处理：
         *   · 同一 ID 换了路径（资产被改名/挪目录）；
         *   · 同一路径换了 ID（资产被删除后重建）；
         *   · 同一路径换了类型（`.geo.toml` 改成了 `.tex`）。
         *
         * @return 逻辑路径或物理路径为空时返回 false（不写入任何一侧）
         */
        bool AddOrUpdate(const AssetIndexEntry &entry);

        bool RemoveByID(const AssetID &id);
        bool RemoveByPath(AssetType type, const U8String &logical_path);

        /**
         * 用另一张表的内容**整体替换**本表。
         *
         * 扫描完成后的换入必须是一次性的：逐条 Clear/AddOrUpdate 会让并发的
         * 读者看到「半张索引」—— 那比看到旧索引糟得多（旧索引至少自洽）。
         * 因此整个替换在**一次写锁**内完成。
         *
         * `hash_algorithm` 一并复制：它描述的就是这张表里的 hash 列。
         */
        bool CopyFrom(const AssetIndexTable &other);

    public:     // 查询（全部只读、可并发）

        bool FindByID(const AssetID &id, AssetIndexEntry &out) const;
        bool FindByPath(AssetType type, const U8String &logical_path, AssetIndexEntry &out) const;
        bool FindByKey(const U8String &path_key, AssetIndexEntry &out) const;

        bool ContainsID(const AssetID &id) const;
        bool ContainsPath(AssetType type, const U8String &logical_path) const;

        /**
         * 只按逻辑路径查，忽略类型。
         *
         * 给诊断与编辑器用（「这条路径上有哪些类型」）。因为主键含类型，这里
         * 只能线性扫描 —— 刻意不为它建第三个索引：一旦真需要高频调用，
         * 说明上层把类型弄丢了，那是调用方的契约错误，不该由本类买单。
         */
        int FindByLogicalPath(const U8String &logical_path, ManagedArray<AssetIndexEntry> &out) const;

        /// 全表快照（每项都是深拷贝）。
        int GetAllEntries(ManagedArray<AssetIndexEntry> &out) const;
    };//class AssetIndexTable
}//namespace hgl::asset
