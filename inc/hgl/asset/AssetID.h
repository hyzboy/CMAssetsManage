#pragma once

/**
 * 资产唯一身份标识（128 位）。
 *
 * ── 为什么「身份」与「内容哈希」必须是两个东西 ──────────────────────────────
 *
 * 一个资产在管线里要回答两个完全不同的问题：
 *
 * | 问题 | 答案载体 | 什么时候变 |
 * |---|---|---|
 * | 「**这一个**资产是谁？」 | `AssetID`（本文件） | 只有删除/重建资产时才变 |
 * | 「它的内容还是上次那份吗？」 | 内容哈希（`.asset_manifest.toml` 的 `hash`） | 内容一改就变 |
 *
 * 把两者合成一个值看着省事，实际上会同时废掉两边：
 *
 *   · 用内容哈希当身份 ⇒ 作者改一个数值，哈希就变，所有引用该资产的场景/材质
 *     全部断链。编辑器里改一行数据能炸掉半个工程，这是不可接受的；
 *   · 用身份当内容指纹 ⇒ 内容改了身份不变，增量编译无法判断「要不要重导」。
 *
 * 因此本文件**只**定义身份。内容哈希是按包统一算法（默认 BLAKE3）算出来的
 * 十六进制串，随索引项一起存在清单里，语义上完全是另一列。二者永不互相替代。
 *
 * ── 为什么是 128 位、为什么用 UUIDv7 ────────────────────────────────────────
 *
 * · 128 位是「碰撞概率可以当不存在」与「本地存储开销」的唯一平衡点：64 位在
 *   十万级资产量下生日碰撞已不可忽略，256 位则让索引表膨胀一倍而没有收益。
 * · 选 **v7 而非 v4**：v7 = 48 位毫秒时间戳 + 版本位 + 62 位随机数。它同时是
 *   **可排序**的 —— 同一毫秒内靠随机位散开，跨毫秒则天然按创建顺序单调递增。
 *   这让「按 UUID 排序 = 按创建时间排序」，索引表与打包文件的目录天然有序，
 *   二分查找与磁盘局部性都直接受益；v4 则完全是随机序，只能整表哈希。
 *   `CMCore` 的 `GetUUIDv7Timestamp()` 已经提供了 48 位时间戳的换算，这里直接复用。
 *
 * ── 文本形态 ──────────────────────────────────────────────────────────────
 *
 * 内部存储是两个 `uint64`（`high` 在前，即网络字节序的前 8 字节），刻意**不**用
 * `uint8[16]`：128 位整数比较是一次两条 `cmp`，比 16 字节 `memcmp` 快得多，
 * 而索引表的主要操作正是比较与排序。
 *
 * 对外给出两种文本：
 *   · `ToString()`     → 32 个小写十六进制字符，**无连字符**。清单、URI、
 *     日志一律用它：定长、无分隔符，切分与解析都最省事；
 *   · `ToUUIDString()` → 标准 RFC 9562 形态 `8-4-4-4-12`。只给「人要看」的场合
 *     （编辑器、外部工具互操作）。两种形态都由 `Parse` 接受。
 *
 * ── 与 `hgl::util::hash::Digest` 的关系 ───────────────────────────────────
 *
 * 不复用 `Digest<16>`：它是「某个哈希算法的输出」，带 `FromString/ToLowerString`
 * 这类面向摘要的语义，且位于 `CMUtil`。`AssetID` 是**身份**，不是摘要 —— 它没有
 * 「用哪个算法算出来」这个概念，加进来只会诱导误用（例如拿 ID 当内容比对）。
 * 两者保持独立类型，编译器才能替我们拦住「把 ID 当哈希塞进哈希字段」。
 */

#include <hgl/type/DataType.h>
#include <hgl/type/String.h>
#include <hgl/util/hash/FNV1a.h>

#include <compare>
#include <cstddef>
#include <functional>

namespace hgl::asset
{
    /**
     * 资产唯一身份标识。
     *
     * 值语义、可平凡拷贝、可比较、可作 `hgl::UnorderedMap` 的键。
     */
    struct AssetID
    {
        uint64 high = 0;    ///<UUID 文本的前 16 个十六进制字符（时间戳 + 版本 + rand_a）
        uint64 low  = 0;    ///<UUID 文本的后 16 个十六进制字符（变体位 + rand_b）

        static constexpr int kByteCount     = 16;   ///<字节数
        static constexpr int kTextLength    = 32;   ///<`ToString()` 的字符数
        static constexpr int kUUIDTextLength= 36;   ///<`ToUUIDString()` 的字符数

        /// 全零 = 尚未分配身份。缺省构造出的就是这个状态。
        bool IsZero() const { return high == 0 && low == 0; }

        void Clear() { high = 0; low = 0; }

        // ── 生成 ───────────────────────────────────────────────────────────

        /**
         * 创建一个全新的 UUIDv7 身份。
         *
         * 保证进程内唯一（时间戳 + 逐次递增的计数器 + wyrand 状态推进），
         * 并且在任意两次调用之间单调可比 —— 后者是「按 ID 排序即按创建排序」的依据。
         *
         * 线程安全：内部用一把互斥锁保护计数器与随机状态。生成只在导入/扫描
         * 新资产时发生（不是热路径），因此不为此引入无锁结构。
         */
        static AssetID Create();

        // ── 字节 / 文本互转 ────────────────────────────────────────────────

        /// 从 16 字节大端（网络字节序）读入。
        static AssetID FromBytes(const uint8 *bytes);

        /// 宽松解析：非十六进制字符（含 `-`）一律跳过；长度不足或超长则得到全零。
        static AssetID FromString(const char *text);
        static AssetID FromString(const AnsiString &text);

        /**
         * 严格解析：只接受 32 个十六进制字符，或标准 `8-4-4-4-12` 形态。
         *
         * 清单与 URI 必须用这个 —— 宽松版会把 `id:` 这类前缀也吃进去。
         *
         * @param text NUL 结尾文本
         * @param out  成功时写入结果；失败时置零
         * @return 解析成功且非全零
         */
        static bool Parse(const char *text, AssetID &out);
        static bool Parse(const AnsiString &text, AssetID &out);

        /// 写出 16 字节大端。
        void ToBytes(uint8 *out) const;

        /// 写出 32 个小写十六进制字符（无连字符）。清单/URI/日志的唯一拼写。
        void ToString(AnsiString &out) const;
        AnsiString ToString() const;

        /// 写出标准 UUID 文本 `8-4-4-4-12`（仅给人看）。
        void ToUUIDString(AnsiString &out) const;
        AnsiString ToUUIDString() const;

        auto operator<=>(const AssetID &) const = default;
    };//struct AssetID

    /// URI 定位段里表示「按 ID 寻址」的前缀：`id:<32 hex>`。
    inline constexpr const char *kAssetIDTargetPrefix = "id:";

    /**
     * 识别并解析「按 ID 寻址」的定位段。
     *
     * 只接受 `id:` 后紧跟严格 32 位十六进制（大小写不敏感）。其它形态一律 false，
     * 让调用方按普通逻辑路径处理 —— 这个判据必须窄，否则一个叫 `id:xxx` 的目录
     * 就没法用路径访问了。
     */
    bool ParseAssetIDTarget(const U8String &target, AssetID &out);

    /// 拼出 ID 寻址的定位段：`id:<32 hex>`。
    U8String MakeAssetIDTarget(const AssetID &id);
}//namespace hgl::asset

namespace std
{
    /// 让 `hgl::UnorderedMap<AssetID, T>` 可以直接用默认模板参数。
    /// 混合交给 `hgl::hash::FNV1aAppend`（与 `hgl::String` 的特化同一口径），
    /// 不在本文件手写散列。
    template<> struct hash<hgl::asset::AssetID>
    {
        size_t operator()(const hgl::asset::AssetID &id) const noexcept
        {
            hgl::uint64 result = hgl::hash::FNV1aInit<hgl::uint64>();

            result = hgl::hash::FNV1aAppend(result, id.high);
            result = hgl::hash::FNV1aAppend(result, id.low);

            return static_cast<size_t>(result);
        }
    };
}//namespace std
