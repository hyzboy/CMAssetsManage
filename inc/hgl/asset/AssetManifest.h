#pragma once

/**
 * `.asset_manifest.toml` —— 目录包裹的持久化清单元数据。
 *
 * ── 为什么目录包裹必须有清单文件 ──────────────────────────────────────────
 *
 * 目录包裹的物理载体是「一堆散落的文件」，它天然缺少打包文件那种自带索引头。
 * 但有两件事**不能**每次挂载都重新发明：
 *
 *   1. **身份**。UUID 一旦每次扫描重新生成，所有跨资产引用（场景 → 材质 → 贴图）
 *      在每次启动后都会断链。所以第一次扫描分配的身份必须落到磁盘上；
 *   2. **元信息**。文件长度、最后修改时间、内容哈希 —— 前两者让「文件有没有变」
 *      可以在不读内容的前提下判定，后者让「内容是不是同一份」可以在不读内容的
 *      前提下判定。没有它们，增量导入与热重载都只能退化成「全量重读」。
 *
 * 打包文件后端不需要这个文件：打包时清单被编译进包头，语义完全相同。
 * 也就是说本文件是「目录包裹版的包头」，**不是**某个后端的私有格式。
 *
 * ── 格式 ──────────────────────────────────────────────────────────────────
 *
 * ```toml
 * manifest_version = 1
 * pack_name        = "res"
 * hash_algorithm   = "BLAKE3"     # 包级统一的内容哈希算法，作用于**所有**条目
 * updated_at       = 1730000000  # Unix 秒
 *
 * [[assets]]
 * id    = "0190f2c1..."          # 32 位无连字符十六进制
 * type  = "geo"                  # 与 AssetTypeTable 的唯一拼写一致
 * path  = "models/hero"          # 逻辑路径，无扩展名
 * file  = "models/hero.geo.toml"  # 包内物理相对路径，含扩展名
 * size  = 40960
 * mtime = 1729900000
 * hash  = "5d41..."              # 可为缺省（尚未计算）
 * ```
 *
 * ── 三条必须守住的规则 ────────────────────────────────────────────────────
 *
 * · **`hash_algorithm` 是包级而不是条目级**。一个包里混用两种算法，等于
 *   「同一列数字有两套量纲」，比对时不单要知道值还要知道各自算法，
 *   任何一次排序/去重/比对都多一个维度。真实需求里也没有「同一个包里
 *   这十张贴图用 BLAKE3、那十张用 MD5」的场景；
 * · **`hash` 缺省 = 尚未计算**，不是「算出来是空」。空串与缺省在语义上等价，
 *   但写出时缺省不落键，让清单在「只做挂载扫描、还没跑离线工具」的中间状态下
 *   依然干净；
 * · **`id` 只在新文件出现时生成**。已存在的文件必须复用清单里的 ID ——
 *   这是引用不断链的唯一保证。复用判据是 `file` 字段（物理相对路径），
 *   不是 `path`：改名意味着换逻辑路径，但物理文件没变时是否换身份，
 *   由调用方决定（本层只提供「按物理路径查旧条目」的能力）。
 *
 * ── 读取失败不等于空清单 ──────────────────────────────────────────────────
 *
 * `LoadAssetManifest` 在**文件不存在**时返回 false 并给出明确的
 * 「not found」，与「文件存在但解析失败」区分开：前者是正常的首次挂载，
 * 后者是必须报给用户的数据损坏。把两者混为一谈会在解析失败时静默地把
 * 整包资产重新分配一遍身份 —— 那是最难恢复的一类事故。
 */

#include <hgl/asset/AssetIndex.h>

#include <hgl/type/String.h>

namespace hgl::asset
{
    /// 目录包裹根下的清单文件名。
    inline constexpr const char *kAssetManifestFileName = ".asset_manifest.toml";

    /// 当前格式版本。字段语义变更时递增，读取端按版本决定如何解释。
    inline constexpr int kAssetManifestVersion = 1;

    /// 包级内容哈希算法的缺省值。
    inline constexpr const char *kDefaultContentHashAlgorithm = "BLAKE3";

    /**
     * 清单头：与「有哪些资产」无关的那部分。
     *
     * 单独成结构而不是塞进 `AssetIndexTable`：头信息描述的是**这个包**，
     * 索引表描述的是**包里的资产**，两者的生命周期与消费方都不同
     * （离线工具改头，运行时读头）。
     */
    struct AssetManifestInfo
    {
        int         version        = kAssetManifestVersion;
        AnsiString  pack_name;                                  ///<挂载名（可空）
        AnsiString  hash_algorithm = kDefaultContentHashAlgorithm;///<包级内容哈希算法
        uint64      updated_at     = 0;                          ///<写出的 Unix 秒
    };//struct AssetManifestInfo

    /// `<root>/<.asset_manifest.toml>`。
    OSString GetAssetManifestPath(const OSString &root_path);

    /**
     * 读清单并填充索引表。
     *
     * 已存在的条目**合并**进 `out_table`（不清空调用方已有内容），这样
     * 「先读清单、再叠加扫描结果」的对拍流程不需要中间容器。
     *
     * @param filename  清单文件的完整路径
     * @param out_table 目标索引表
     * @param out_info  清单头；文件不存在时保持缺省值
     * @param out_error 失败原因（可为 nullptr）
     * @return 成功解析返回 true；文件不存在、解析失败、版本过新都返回 false
     */
    bool LoadAssetManifest(const OSString &filename,
                           AssetIndexTable &out_table,
                           AssetManifestInfo &out_info,
                           AnsiString *out_error = nullptr);

    /**
     * 把索引表写成 TOML 文本。
     *
     * 纯函数、不碰磁盘 —— 便于单元测试与离线工具复用。条目按
     * `(类型段, 逻辑路径)` 排序输出：清单进版本库，稳定的字节序才能让
     * 「加了一个资产」在 diff 里只多一行，而不是整段重排。
     */
    AnsiString WriteAssetManifestText(const AssetIndexTable &table, const AssetManifestInfo &info);

    /**
     * 写清单（先写 `<file>.tmp` 再改名，避免中途失败留下半截文件）。
     *
     * @param filename  清单文件的完整路径
     * @param table     索引表（内容哈希随条目一起写出）
     * @param info      清单头；`updated_at` 为 0 时自动填当前时间
     * @param out_error 失败原因（可为 nullptr）
     */
    bool SaveAssetManifest(const OSString &filename,
                           const AssetIndexTable &table,
                           AssetManifestInfo &info,
                           AnsiString *out_error = nullptr);
}//namespace hgl::asset
