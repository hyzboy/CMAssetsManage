#pragma once

/**
 * FlatBuffers 后端 —— `hgl::serialize::Value` 树 ↔ FlatBuffers 缓冲。
 *
 * ── 布局从哪来 ────────────────────────────────────────────────────────────
 * FlatBuffers 的物理布局（voffset、内联 struct、向量元素类型）无法从值树本身
 * 推断，必须由结构布局决定。本文件提供**两条入口，共用同一份字节搬运实现**：
 *
 *   · `ClassMetaInfo` 入口 —— 唯一真源。布局直接来自 `HGL_ASSET_STRUCT` /
 *     `HGL_PROPERTY` 生成的反射表（名字 / 类别 / 标志位 / 嵌套元数据），
 *     因此 AssetGen **不再**需要为每个资产吐一份 `.fbs` 描述表。
 *   · `TableDesc` 入口 —— 旧路径：布局来自生成的手写描述表。现存生成物仍在用，
 *     待生成器收敛后整体删除。
 *
 * 关键差别：语义（缺省、校验、枚举名、容器降级）**只存在于** `StructSerializer<T>::Do()`
 * 里一次；这里只有「按布局搬运字节」的机械逻辑。新增 JSON / Protobuf 后端时，
 * 同样只需一份自己的布局解释器，不需要重新生成任何资产代码。
 *
 * ── 布局推导规则（`ClassMetaInfo` 入口）────────────────────────────────────
 *   · 值树字段名   = `TomlKeyOf(prop)`（`Key=` 覆盖后的文本键名）
 *   · 值树字段 id  = 「`PF_Serialized` 属性的声明序号」，从 1 起
 *                    —— 与 `Serialize.gen.h` 里 `ar.Field(id, name, v)` 完全一致
 *   · FlatBuffers voffset = `4 + 2 * (`PF_SerializedFlatBuffers` 属性的声明序号 - 1)`
 *
 * 两个计数器**刻意分离**：允许「只进 TOML 不进 FlatBuffers」的属性（`PF_EditOnly`
 * 之类）交错出现而不破坏任何一边的序号。
 *
 * ── 写入顺序约束 ──────────────────────────────────────────────────────────
 * FlatBuffers 缓冲区向后增长，父表的子串 / 子表 / 向量必须在 `StartTable()` 之前
 * 建好。所以编码分两趟：
 *   1. 逐字段预建子对象，记下 offset 或内联 struct 字节；
 *   2. `StartTable()` 后逐个 `AddElement` / `AddOffset` / `AddStruct`，再 `EndTable()`。
 * 这正是值树这层中间表示存在的意义 —— 遍历顺序与缓冲布局顺序彻底解耦。
 *
 * ── 校验 ──────────────────────────────────────────────────────────────────
 * `ClassMetaInfo` 入口自带递归 `VerifyTable()` / `VerifyBuffer()`：它照着同一份
 * 反射表逐字段核对 vtable 项、标量宽度、偏移合法性、子表与向量的元素类型，
 * 语义与 flatc 生成的 `Verify()` 等价，且**不需要**任何 schema 级生成头。
 * 解码前务必先过一遍 —— 坏缓冲在 `GetField` 里越界是未定义行为。
 *
 * ── 分层 ──────────────────────────────────────────────────────────────────
 * 本头是**纯描述层**：只有描述表、值树与类型化门面，不引用任何 flatbuffers
 * 符号。全部布局推导与字节搬运都在 `FlatCodec.cpp` 里，因此 flatbuffers 只是
 * `CMAssetsManage` 的实现细节，不会外溢到使用方。
 */

#include <hgl/asset/AssetObject.h>
#include <hgl/serialize/Archive.h>
#include <hgl/serialize/Value.h>
#include <hgl/type/ValueArray.h>

namespace hgl::serialize
{
    /// 一个合法 FlatBuffers 缓冲的最小长度：根偏移 + 根表起始处的 vtable 偏移。
    /// 取代 flatbuffers 的 `FLATBUFFERS_MIN_BUFFER_SIZE`，让调用方不必包含该头。
    constexpr size_t kFlatBufferMinSize = sizeof(uint32_t) * 2;
}//namespace hgl::serialize

namespace hgl::serialize::fb
{
    /// 字段在 FlatBuffers 里的类型。
    enum class FbType : uint8_t
    {
        None = 0,
        Bool,
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float, Double,
        Enum,          ///< 有底层 32 位整数的 enum class；树上以名字字符串保存
        String,
        Struct,        ///< 内联定长数值结构（Float2 / Float3 / Float4）
        Table,         ///< 子表
        VectorScalar,  ///< vector<标量>
        VectorString,  ///< vector<string>
        VectorTable,   ///< vector<表>
    };

    struct TableDesc;

    /// 枚举名 ↔ 数值；由 AssetGen 生成，直接转发到生成的 AssetEnum* 函数。
    using EnumFromNameFn = bool (*)(const char *name, int64_t &out);
    using EnumToNameFn = const char *(*)(int64_t value);

    struct FieldDesc
    {
        uint32_t id = 0;                  ///< 值树里的字段 id（与 Serialize 的 id 一致）
        const char *name = nullptr;       ///< 值树里的字段名
        FbType type = FbType::None;
        uint16_t voffset = 0;             ///< FlatBuffers 字段偏移：4 + 2*声明序号
        FbType elem_type = FbType::None;  ///< 向量元素类型
        uint8_t components = 0;           ///< Struct 的分量数（2/3/4）
        const TableDesc *table = nullptr; ///< Table / VectorTable 的子表描述
        EnumFromNameFn enum_from_name = nullptr; ///< Enum 专用
        EnumToNameFn enum_to_name = nullptr;     ///< Enum 专用
        /// 标量字段是否为「可选标量」（对应 Spec 里的 Presence<T>）。
        /// 为 true 时：编码即使是 0 也必须真写进缓冲，解码则“没写即缺席”，
        /// 从而保留 Presence 三态语义；普通标量仍走「完整快照」覆盖。
        bool optional_scalar = false;

        /// 子表 / 表向量的嵌套元数据（`ClassMetaInfo` 入口用）。
        /// 与 `table` 二者只会有其一：`meta` 是唯一真源路径，`table` 是旧描述表路径。
        const hgl::asset::ClassMetaInfo *meta = nullptr;
    };

    /// 把即 Present<> 对应的标量字段标记为「可选标量」（生成代码用）。
    constexpr FieldDesc OptScalar(FieldDesc field)
    {
        field.optional_scalar = true;
        return field;
    }

    struct TableDesc
    {
        const char *name = nullptr;
        const FieldDesc *fields = nullptr;
        int field_count = 0;

        const FieldDesc *Find(const uint32_t id) const
        {
            for (int i = 0; i < field_count; ++i)
            {
                if (fields[i].id == id)
                    return &fields[i];
            }
            return nullptr;
        }

        const FieldDesc *FindByName(const char *name) const
        {
            if (!name)
                return nullptr;
            for (int i = 0; i < field_count; ++i)
            {
                if (fields[i].name && hgl::stricmp(fields[i].name, name) == 0)
                    return &fields[i];
            }
            return nullptr;
        }
    };

    // ── 内联 struct 载体 ────────────────────────────────────────────────────
    //
    // FlatBuffers 的 struct 是「按最大成员对齐的裸字节块」。本工程用到的定长
    // 向量结构只有 2/3/4 个 32 位分量（Float2/3/4），用三个定宽 POD 即可完全
    // 覆盖，且对齐与 flatc 生成的结构一致（对齐 4）。

    struct Vec2f { float x, y; };
    struct Vec3f { float x, y, z; };
    struct Vec4f { float x, y, z, w; };

    union StructBuffer
    {
        float f[4];
        Vec2f v2;
        Vec3f v3;
        Vec4f v4;
        uint8_t raw[16];
    };

    // ── 缓冲级入口（实现在 FlatCodec.cpp，本头不出现 flatbuffers 类型）──────
    //
    // 四个入口两两成对：`TableDesc` 是手写描述表路径，`ClassMetaInfo` 是唯一真源
    // 路径。两者都先规范化成 `FieldDesc[]` 再交给同一份字节搬运实现。

    /// 完整缓冲的根级校验（含可选的 file identifier 与根表递归校验）。
    bool VerifyBuffer(const void *data, size_t size,
                      const hgl::asset::ClassMetaInfo &meta,
                      const char *file_identifier = nullptr);

    /// 值树 → 完整 FlatBuffers 缓冲（旧路径：手写描述表）。
    bool EncodeToBuffer(const Value &root, const TableDesc &desc,
                        const char *file_identifier, ValueArray<uint8_t> &out_bytes);

    /// 值树 → 完整 FlatBuffers 缓冲（唯一真源路径：反射元数据）。
    bool EncodeToBuffer(const Value &root, const hgl::asset::ClassMetaInfo &meta,
                        const char *file_identifier, ValueArray<uint8_t> &out_bytes);

    /// 完整缓冲 → 值树（旧路径）。强校验请先跑 `VerifyBuffer`。
    bool DecodeFromBuffer(const uint8_t *data, size_t size,
                          const TableDesc &desc, Value &out);

    /// 完整缓冲 → 值树（唯一真源路径）。强校验请先跑 `VerifyBuffer`。
    bool DecodeFromBuffer(const uint8_t *data, size_t size,
                          const hgl::asset::ClassMetaInfo &meta, Value &out);

    /// 便捷重载：元数据取 `StructSerializer<T>::Meta()`。
    template <typename T>
    inline bool VerifyBuffer(const void *data, const size_t size,
                             const char *file_identifier = nullptr)
    {
        return VerifyBuffer(data, size, *StructSerializer<T>::Meta(), file_identifier);
    }
    // ── 类型化门面 ──────────────────────────────────────────────────────────
    //
    // `StructSerializer<T>::Do()` 把 C++ 结构 ⇄ 值树，本模块把值树 ⇄ 缓冲字节。
    // 于是**任何**加了 `HGL_ASSET_STRUCT` 的结构都自动获得无损二进制往返，
    // 不需要 `.fbs`、不需要 flatc、不需要每个结构一份生成代码。
    //
    // `StructSerializer<T>::Meta()`（由生成器产出）让便捷重载能自己找到反射表。

    /**
     * @brief `T` → FlatBuffers 缓冲字节（`StructSerializer<T>::Do()` + 反射布局）。
     *
     * @param out_bytes       输出缓冲；失败时被清空
     * @param meta            结构反射元数据
     * @param file_identifier 可选的四字节文件标识
     * @param out_error       失败时写入序列化失败原因（Archive 报的第一个错）
     * @return 序列化与编码都成功时返回 true
     */
    template <typename T>
    inline bool EncodeToFlatBuffers(ValueArray<uint8_t> &out_bytes, const T &src,
                                    const hgl::asset::ClassMetaInfo &meta,
                                    const char *file_identifier = nullptr,
                                    AnsiString *out_error = nullptr)
    {
        Value node;
        Archive ar = Archive::ForWrite(node);
        StructSerializer<T>::Do(ar, src);

        if (out_error)
            *out_error = AnsiString(ar.ErrorMessage());

        if (!ar.Ok())
        {
            out_bytes.Clear();
            return false;
        }

        return EncodeToBuffer(node, meta, file_identifier, out_bytes);
    }

    /// 便捷重载：元数据取 `StructSerializer<T>::Meta()`。
    template <typename T>
    inline bool EncodeToFlatBuffers(ValueArray<uint8_t> &out_bytes, const T &src,
                                    const char *file_identifier = nullptr,
                                    AnsiString *out_error = nullptr)
    {
        return EncodeToFlatBuffers<T>(out_bytes, src, *StructSerializer<T>::Meta(),
                                      file_identifier, out_error);
    }

    /**
     * @brief FlatBuffers 缓冲 → `T`（**不校验**边界）。
     *
     * @param out_error     失败时写入原因
     * @param out_fail_path 失败时写入失败字段的完整表路径
     */
    template <typename T>
    inline bool DecodeFromFlatBuffers(const uint8_t *data, const size_t size, T &out,
                                      const hgl::asset::ClassMetaInfo &meta,
                                      const char *file_identifier = nullptr,
                                      AnsiString *out_error = nullptr,
                                      AnsiString *out_fail_path = nullptr)
    {
        (void)file_identifier;

        Value node;
        if (!DecodeFromBuffer(data, size, meta, node))
        {
            if (out_error)
                *out_error = AnsiString("FlatBuffers 缓冲长度不足或根表非法");
            return false;
        }

        Archive ar = Archive::ForRead(node);
        ar.ReadFrom<T>(node, out);

        if (out_error)
            *out_error = AnsiString(ar.ErrorMessage());
        if (out_fail_path)
            *out_fail_path = AnsiString(ar.FailedPath());

        return ar.Ok();
    }

    /// 便捷重载：元数据取 `StructSerializer<T>::Meta()`。
    template <typename T>
    inline bool DecodeFromFlatBuffers(const uint8_t *data, const size_t size, T &out,
                                      const char *file_identifier = nullptr,
                                      AnsiString *out_error = nullptr,
                                      AnsiString *out_fail_path = nullptr)
    {
        return DecodeFromFlatBuffers<T>(data, size, out, *StructSerializer<T>::Meta(),
                                        file_identifier, out_error, out_fail_path);
    }

    /**
     * @brief 先校验、再解码 —— 读**外部来源**缓冲一律走这条。
     *
     * 校验失败时不碰 `out`，只写诊断信息。
     */
    template <typename T>
    inline bool VerifyAndDecodeFlatBuffers(const void *data, const size_t size, T &out,
                                           const hgl::asset::ClassMetaInfo &meta,
                                           const char *file_identifier = nullptr,
                                           AnsiString *out_error = nullptr,
                                           AnsiString *out_fail_path = nullptr)
    {
        if (!VerifyBuffer(data, size, meta, file_identifier))
        {
            if (out_error)
                *out_error = AnsiString("FlatBuffers 校验失败：缓冲结构不合法");
            return false;
        }

        return DecodeFromFlatBuffers<T>(static_cast<const uint8_t *>(data), size, out,
                                        meta, file_identifier, out_error, out_fail_path);
    }

    /// 便捷重载：元数据取 `StructSerializer<T>::Meta()`。
    template <typename T>
    inline bool VerifyAndDecodeFlatBuffers(const void *data, const size_t size, T &out,
                                           const char *file_identifier = nullptr,
                                           AnsiString *out_error = nullptr,
                                           AnsiString *out_fail_path = nullptr)
    {
        return VerifyAndDecodeFlatBuffers<T>(data, size, out,
                                             *StructSerializer<T>::Meta(),
                                             file_identifier, out_error, out_fail_path);
    }
}//namespace hgl::serialize::fb
