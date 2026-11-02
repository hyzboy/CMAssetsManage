/**
 * FlatCodec 实现 —— 依赖 flatbuffers，故不进公共头。
 *
 * `hgl/serialize/FlatCodec.h` 只保留描述表、值树与类型化门面；本文件承载
 * `flatbuffers::` 的全部布局推导与字节搬运，使 `CMAssetsManage` 对
 * flatbuffers 的依赖可以降为 PRIVATE。
 */

#include <hgl/serialize/FlatCodec.h>
#include <hgl/serialize/Value.h>
#include <hgl/type/ValueArray.h>

#include <flatbuffers/flatbuffers.h>

#include <cstring>
#include <type_traits>

namespace
{
    /// 把 FlatBufferBuilder 里已完成的缓冲整块搬进值树侧字节数组。
    bool CopyBuilderTo(flatbuffers::FlatBufferBuilder &fbb,
                       hgl::ValueArray<uint8_t> &out_bytes)
    {
        const size_t size = fbb.GetSize();
        if (size == 0)
            return false;

        if (!out_bytes.Resize(static_cast<int>(size)))
            return false;

        std::memcpy(out_bytes.GetData(), fbb.GetBufferPointer(), size);
        return true;
    }
}//namespace

namespace hgl::serialize::fb
{
namespace detail
{
        template <typename T>
        inline void DecodeScalarVector(const flatbuffers::Table *t,
                                       const flatbuffers::voffset_t vo,
                                       Value &out)
        {
            const auto *vec = t->GetPointer<const flatbuffers::Vector<T> *>(vo);
            out.SetArray();
            if (!vec)
                return;
            for (uint32_t i = 0; i < vec->size(); ++i)
            {
                Value *slot = out.AddElement();
                if (!slot)
                    continue;
                const T v = vec->Get(i);
                if constexpr (std::is_floating_point_v<T>)
                    slot->SetDouble(static_cast<double>(v));
                else if constexpr (std::is_signed_v<T>)
                    slot->SetInt(static_cast<int64_t>(v));
                else
                    slot->SetUInt(static_cast<uint64_t>(v));
            }
        }

        /// 标量向量编码：把树上的数组逐项转成目标标量宽度后一次性建向量。
        inline flatbuffers::Offset<void> EncodeScalarVector(const FbType elem,
                                                            const Value &node,
                                                            flatbuffers::FlatBufferBuilder &fbb)
        {
            const int count = node.GetElementCount();

            switch (elem)
            {
            case FbType::Bool:
            case FbType::UInt8:
            {
                ValueArray<uint8_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    uint64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetUInt(raw);
                    tmp.Add(static_cast<uint8_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<uint8_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::Int8:
            {
                ValueArray<int8_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    int64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetInt(raw);
                    tmp.Add(static_cast<int8_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<int8_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::Int16:
            {
                ValueArray<int16_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    int64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetInt(raw);
                    tmp.Add(static_cast<int16_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<int16_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::UInt16:
            {
                ValueArray<uint16_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    uint64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetUInt(raw);
                    tmp.Add(static_cast<uint16_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<uint16_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::Int32:
            case FbType::Enum:
            {
                ValueArray<int32_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    int64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetInt(raw);
                    tmp.Add(static_cast<int32_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<int32_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::UInt32:
            {
                ValueArray<uint32_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    uint64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetUInt(raw);
                    tmp.Add(static_cast<uint32_t>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<uint32_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::Int64:
            {
                ValueArray<int64_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    int64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetInt(raw);
                    tmp.Add(raw);
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<int64_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::UInt64:
            {
                ValueArray<uint64_t> tmp;
                for (int i = 0; i < count; ++i)
                {
                    uint64_t raw = 0;
                    if (const Value *v = node.GetElement(i))
                        v->GetUInt(raw);
                    tmp.Add(raw);
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<uint64_t>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            case FbType::Float:
            {
                ValueArray<float> tmp;
                for (int i = 0; i < count; ++i)
                {
                    double raw = 0.0;
                    if (const Value *v = node.GetElement(i))
                        v->GetDouble(raw);
                    tmp.Add(static_cast<float>(raw));
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<float>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            default:
            {
                ValueArray<double> tmp;
                for (int i = 0; i < count; ++i)
                {
                    double raw = 0.0;
                    if (const Value *v = node.GetElement(i))
                        v->GetDouble(raw);
                    tmp.Add(raw);
                }
                return flatbuffers::Offset<void>(
                    fbb.CreateVector<double>(tmp.GetData(), static_cast<size_t>(count)).o);
            }
            }
        }

        /// 枚举字段的数值化：树上可能是名字字符串，也可能是整数。
        inline int64_t ResolveEnumValue(const Value &node, const FieldDesc &field)
        {
            int64_t raw = 0;
            if (node.IsString())
            {
                const U8String &utf8 = node.GetString();
                const char *name = reinterpret_cast<const char *>(utf8.c_str());
                if (field.enum_from_name && name)
                    field.enum_from_name(name, raw);
                return raw;
            }
            node.GetInt(raw);
            return raw;
        }

        /// 标量写入：普通字段按 schema 缺省值省略（FlatBuffers 惯例）；
        /// 可选标量（Presence<> 字段）必须无条件写入，否则「显式设为缺省值」
        /// 与「从未设置」在缓冲里无法区分。
        template <typename T>
        inline void AddScalar(flatbuffers::FlatBufferBuilder &fbb, const FieldDesc &field,
                              const T value, const T def)
        {
            if (field.optional_scalar)
                fbb.template AddElement<T>(field.voffset, value);
            else
                fbb.template AddElement<T>(field.voffset, value, def);
        }

        // ── `ClassMetaInfo` → 字段布局 ──────────────────────────────────────

        /// `PropertyKind` → FlatBuffers 标量类型（非标量返回 `None`）。
        inline FbType FbTypeOfKind(const hgl::asset::PropertyKind kind)
        {
            using hgl::asset::PropertyKind;

            switch (kind)
            {
            case PropertyKind::Bool:   return FbType::Bool;
            case PropertyKind::Int8:   return FbType::Int8;
            case PropertyKind::Int16:  return FbType::Int16;
            case PropertyKind::Int32:  return FbType::Int32;
            case PropertyKind::Int64:  return FbType::Int64;
            case PropertyKind::UInt8:  return FbType::UInt8;
            case PropertyKind::UInt16: return FbType::UInt16;
            case PropertyKind::UInt32: return FbType::UInt32;
            case PropertyKind::UInt64: return FbType::UInt64;
            case PropertyKind::Float:  return FbType::Float;
            case PropertyKind::Double: return FbType::Double;
            case PropertyKind::Enum:   return FbType::Enum;  ///< FlatBuffers 侧统一 int32
            default:                   return FbType::None;
            }
        }

        /// 是否「按 voffset 直读」的标量字段（表 / 串 / 结构体 / 向量都靠 offset）。
        inline bool IsScalarFbType(const FbType type)
        {
            switch (type)
            {
            case FbType::None:
            case FbType::String:
            case FbType::Struct:
            case FbType::Table:
            case FbType::VectorScalar:
            case FbType::VectorString:
            case FbType::VectorTable:
                return false;
            default:
                return true;
            }
        }

        /// 容器属性的元素类别 → 向量字段类型。无法识别的元素类别返回 `None`，
        /// 让整条字段被跳过 —— 宁可不写，也不要写错字节。
        inline FbType VectorFbType(const hgl::asset::PropertyKind elem)
        {
            using hgl::asset::PropertyKind;

            if (elem == PropertyKind::String)
                return FbType::VectorString;
            if (elem == PropertyKind::Struct)
                return FbType::VectorTable;
            if (IsScalarFbType(FbTypeOfKind(elem)))
                return FbType::VectorScalar;
            return FbType::None;
        }

        /**
         * @brief 单个属性 → 单个字段布局（FlatBuffers 视角）。
         *
         * @param prop       反射表里的一项
         * @param tree_id    值树字段 id（`PF_Serialized` 声明序号，1 起）
         * @param fb_ordinal FlatBuffers 字段序号（0 起，`PF_SerializedFlatBuffers` 声明序号）
         */
        inline FieldDesc MakeFieldDesc(const hgl::asset::PropertyMetaInfo &prop,
                                       const uint32_t tree_id,
                                       const uint32_t fb_ordinal)
        {
            using hgl::asset::PropertyKind;

            FieldDesc field;
            field.id = tree_id;
            field.name = hgl::asset::TomlKeyOf(prop);
            field.voffset = static_cast<flatbuffers::voffset_t>(4u + 2u * fb_ordinal);

            switch (prop.kind)
            {
            case PropertyKind::Vector2f:
                field.type = FbType::Struct;
                field.components = 2;
                break;
            case PropertyKind::Vector3f:
                field.type = FbType::Struct;
                field.components = 3;
                break;
            case PropertyKind::Vector4f:
                field.type = FbType::Struct;
                field.components = 4;
                break;
            case PropertyKind::Struct:
                field.type = FbType::Table;
                field.meta = prop.struct_meta;
                break;
            case PropertyKind::String:
                field.type = FbType::String;
                break;
            case PropertyKind::ValueArray:
            case PropertyKind::PtrArray:
                field.type = VectorFbType(prop.elem_kind);
                field.elem_type = FbTypeOfKind(prop.elem_kind);
                field.meta = prop.struct_meta;
                break;
            default:
                field.type = FbTypeOfKind(prop.kind);
                break;
            }

            if (field.type == FbType::Enum || field.elem_type == FbType::Enum)
            {
                field.enum_from_name = prop.enum_parse;
                field.enum_to_name = prop.enum_to_name;
            }

            // 只有标量需要这个位：表 / 串 / 结构体 / 向量缺席时值树里干脆没有该键，
            // 三态天然成立；而标量的「不写」与「写 0」在缓冲里字节相同。
            field.optional_scalar = hgl::asset::IsPresenceProperty(prop)
                                 && IsScalarFbType(field.type);

            return field;
        }

        /**
         * @brief 把 `ClassMetaInfo` 展开成 `FieldDesc` 数组（FlatBuffers 视角的布局）。
         *
         * 无法识别的字段类型（`FbType::None`，例如生成器没填对 `elem_kind`）会被
         * **跳过**，但序号照常推进 —— 否则它后面的字段 voffset 会整体前移，把缓冲
         * 写坏。跳过的字段既不解码也不编码，值树里保持缺席。
         */
        inline void BuildFields(const hgl::asset::ClassMetaInfo &meta,
                                ValueArray<FieldDesc> &out)
        {
            out.Clear();

            if (!meta.properties)
                return;

            out.Reserve(static_cast<int>(meta.property_count));

            uint32_t tree_id = 0;
            uint32_t fb_ordinal = 0;

            for (uint32_t i = 0; i < meta.property_count; ++i)
            {
                const hgl::asset::PropertyMetaInfo &prop = meta.properties[i];

                if (!hgl::asset::IsSerializedToml(prop))
                    continue;   // 值树里根本不会有这个键（`ar.Field()` 也只发这一批）

                ++tree_id;

                if (!hgl::asset::IsSerializedFlatBuffers(prop))
                    continue;   // 只进 TOML 的字段：占 id，不占 voffset

                const FieldDesc field = MakeFieldDesc(prop, tree_id, fb_ordinal);
                ++fb_ordinal;

                if (field.type == FbType::None)
                    continue;

                out.Add(field);
            }
        }
    // ── 编码 ────────────────────────────────────────────────────────────────

    // 四个入口两两成对：`TableDesc` 是旧描述表路径，`ClassMetaInfo` 是唯一真源路径。
    // 两者都先规范化成 `FieldDesc[]` 再交给同一份 `EncodeTableFields` / `DecodeTableFields`，
    // 于是字节搬运逻辑只有一份。

    inline flatbuffers::Offset<void> EncodeTable(flatbuffers::FlatBufferBuilder &fbb,
                                                 const Value &node,
                                                 const TableDesc &desc);

    inline flatbuffers::Offset<void> EncodeTable(flatbuffers::FlatBufferBuilder &fbb,
                                                 const Value &node,
                                                 const hgl::asset::ClassMetaInfo &meta);

    inline void DecodeTable(const flatbuffers::Table *t, const TableDesc &desc,
                            Value &out);

    inline void DecodeTable(const flatbuffers::Table *t,
                            const hgl::asset::ClassMetaInfo &meta, Value &out);

        /// 子表编码：按字段里带的是哪种嵌套描述分派。
        inline flatbuffers::Offset<void> EncodeSubTable(flatbuffers::FlatBufferBuilder &fbb,
                                                        const Value &node,
                                                        const FieldDesc &field)
        {
            if (field.meta)
                return EncodeTable(fbb, node, *field.meta);
            if (field.table)
                return EncodeTable(fbb, node, *field.table);
            return flatbuffers::Offset<void>();
        }

        /// 子表解码：同上。
        inline void DecodeSubTable(const flatbuffers::Table *t, const FieldDesc &field,
                                   Value &out)
        {
            if (field.meta)
                DecodeTable(t, *field.meta, out);
            else if (field.table)
                DecodeTable(t, *field.table, out);
        }

    /**
     * 把值树按布局写成 FlatBuffers 表。
     * 值树里存在但布局里没有的字段会被忽略（例如仅编辑器使用的 EditOnly 字段）。
     */
    inline flatbuffers::Offset<void> EncodeTableFields(flatbuffers::FlatBufferBuilder &fbb,
                                                       const Value &node,
                                                       const FieldDesc *fields,
                                                       const int field_count)
    {
        ValueArray<flatbuffers::Offset<void>> offsets;
        ValueArray<uint8_t> has_offset;
        ValueArray<StructBuffer> structs;
        ValueArray<uint8_t> has_struct;

        for (int i = 0; i < field_count; ++i)
        {
            offsets.Add(flatbuffers::Offset<void>());
            has_offset.Add(0);
            StructBuffer empty{};
            structs.Add(empty);
            has_struct.Add(0);
        }

        // ── 第一趟：预建所有子对象 ─────────────────────────────────────────
        for (int i = 0; i < field_count; ++i)
        {
            const FieldDesc &field = fields[i];
            const Value *value = node.FindField(field.name, field.id);
            if (!value)
                continue;

            switch (field.type)
            {
            case FbType::String:
            {
                const U8String &utf8 = value->GetString();
                const char *text = reinterpret_cast<const char *>(utf8.c_str());
                offsets[i] = flatbuffers::Offset<void>(
                    fbb.CreateString(text ? text : "").o);
                has_offset[i] = 1;
                break;
            }
            case FbType::Table:
            {
                if (!value->IsTable())
                    break;
                if (!field.meta && !field.table)
                    break;
                offsets[i] = detail::EncodeSubTable(fbb, *value, field);
                has_offset[i] = 1;
                break;
            }
            case FbType::Struct:
            {
                StructBuffer buffer{};
                const int components = (field.components >= 2 && field.components <= 4)
                                           ? field.components
                                           : 3;
                for (int c = 0; c < components; ++c)
                {
                    double raw = 0.0;
                    if (const Value *item = value->GetElement(c))
                        item->GetDouble(raw);
                    buffer.f[c] = static_cast<float>(raw);
                }
                structs[i] = buffer;
                has_struct[i] = 1;
                break;
            }
            case FbType::VectorScalar:
            {
                offsets[i] = detail::EncodeScalarVector(field.elem_type, *value, fbb);
                has_offset[i] = 1;
                break;
            }
            case FbType::VectorString:
            {
                const int count = value->GetElementCount();
                ValueArray<flatbuffers::Offset<flatbuffers::String>> items;
                for (int k = 0; k < count; ++k)
                {
                    const char *text = "";
                    if (const Value *item = value->GetElement(k))
                        text = reinterpret_cast<const char *>(item->GetString().c_str());
                    items.Add(fbb.CreateString(text ? text : ""));
                }
                offsets[i] = flatbuffers::Offset<void>(
                    fbb.CreateVector(items.GetData(), static_cast<size_t>(count)).o);
                has_offset[i] = 1;
                break;
            }
            case FbType::VectorTable:
            {
                if (!value->IsArray())
                    break;
                if (!field.meta && !field.table)
                    break;

                const int count = value->GetElementCount();
                ValueArray<flatbuffers::Offset<void>> items;
                for (int k = 0; k < count; ++k)
                {
                    const Value *item = value->GetElement(k);
                    items.Add(item ? detail::EncodeSubTable(fbb, *item, field)
                                   : flatbuffers::Offset<void>());
                }
                offsets[i] = flatbuffers::Offset<void>(
                    fbb.CreateVector(items.GetData(),
                                     static_cast<size_t>(count)).o);
                has_offset[i] = 1;
                break;
            }
            default:
                break;
            }
        }

        // ── 第二趟：建表本体 ───────────────────────────────────────────────
        const flatbuffers::uoffset_t start = fbb.StartTable();

        for (int i = 0; i < field_count; ++i)
        {
            const FieldDesc &field = fields[i];
            const Value *value = node.FindField(field.name, field.id);
            if (!value)
                continue;

            switch (field.type)
            {
            case FbType::Bool:
            {
                bool b = false;
                value->GetBool(b);
                detail::AddScalar<uint8_t>(fbb, field, static_cast<uint8_t>(b ? 1u : 0u), 0);
                break;
            }
            case FbType::Int8:
            {
                int64_t raw = 0;
                value->GetInt(raw);
                detail::AddScalar<int8_t>(fbb, field, static_cast<int8_t>(raw), 0);
                break;
            }
            case FbType::Int16:
            {
                int64_t raw = 0;
                value->GetInt(raw);
                detail::AddScalar<int16_t>(fbb, field, static_cast<int16_t>(raw), 0);
                break;
            }
            case FbType::Int32:
            case FbType::Enum:
            {
                const int64_t raw = field.type == FbType::Enum
                                        ? detail::ResolveEnumValue(*value, field)
                                        : [&] { int64_t v = 0; value->GetInt(v); return v; }();
                detail::AddScalar<int32_t>(fbb, field, static_cast<int32_t>(raw), 0);
                break;
            }
            case FbType::Int64:
            {
                int64_t raw = 0;
                value->GetInt(raw);
                detail::AddScalar<int64_t>(fbb, field, raw, 0);
                break;
            }
            case FbType::UInt8:
            {
                uint64_t raw = 0;
                value->GetUInt(raw);
                detail::AddScalar<uint8_t>(fbb, field, static_cast<uint8_t>(raw), 0);
                break;
            }
            case FbType::UInt16:
            {
                uint64_t raw = 0;
                value->GetUInt(raw);
                detail::AddScalar<uint16_t>(fbb, field, static_cast<uint16_t>(raw), 0);
                break;
            }
            case FbType::UInt32:
            {
                uint64_t raw = 0;
                value->GetUInt(raw);
                detail::AddScalar<uint32_t>(fbb, field, static_cast<uint32_t>(raw), 0);
                break;
            }
            case FbType::UInt64:
            {
                uint64_t raw = 0;
                value->GetUInt(raw);
                detail::AddScalar<uint64_t>(fbb, field, raw, 0);
                break;
            }
            case FbType::Float:
            {
                double raw = 0.0;
                value->GetDouble(raw);
                detail::AddScalar<float>(fbb, field, static_cast<float>(raw), 0.0f);
                break;
            }
            case FbType::Double:
            {
                double raw = 0.0;
                value->GetDouble(raw);
                detail::AddScalar<double>(fbb, field, raw, 0.0);
                break;
            }
            case FbType::Struct:
                if (has_struct[i])
                {
                    const int components = (field.components >= 2 && field.components <= 4)
                                               ? field.components
                                               : 3;
                    if (components == 2)
                        fbb.AddStruct(field.voffset, &structs[i].v2);
                    else if (components == 3)
                        fbb.AddStruct(field.voffset, &structs[i].v3);
                    else
                        fbb.AddStruct(field.voffset, &structs[i].v4);
                }
                break;
            default:
                if (has_offset[i])
                    fbb.AddOffset(field.voffset, offsets[i]);
                break;
            }
        }

        const flatbuffers::uoffset_t end = fbb.EndTable(start);
        return flatbuffers::Offset<void>(end);
    }

    /// 旧路径：手写描述表 → 表。
    inline flatbuffers::Offset<void> EncodeTable(flatbuffers::FlatBufferBuilder &fbb,
                                                 const Value &node,
                                                 const TableDesc &desc)
    {
        return EncodeTableFields(fbb, node, desc.fields, desc.field_count);
    }

    /// 唯一真源路径：反射元数据 → 表。
    inline flatbuffers::Offset<void> EncodeTable(flatbuffers::FlatBufferBuilder &fbb,
                                                 const Value &node,
                                                 const hgl::asset::ClassMetaInfo &meta)
    {
        ValueArray<FieldDesc> fields;
        detail::BuildFields(meta, fields);
        return EncodeTableFields(fbb, node, fields.GetData(), fields.GetCount());
    }

    // ── 解码 ────────────────────────────────────────────────────────────────

    inline void DecodeTableFields(const flatbuffers::Table *t, const FieldDesc *fields,
                                  const int field_count, Value &out)
    {
        out.SetTable();
        if (!t)
            return;

        for (int i = 0; i < field_count; ++i)
        {
            const FieldDesc &field = fields[i];
            const bool present = t->GetOptionalFieldOffset(field.voffset) != 0;

            // 与 TOML 的「缺键保留类内初始值」不同：FlatBuffers 是**完整快照**。
            // 标量字段即使等于 schema 缺省值（因此没写进缓冲）也必须覆盖目标，
            // 否则 C++ 类内初始值会盖过 schema 缺省值。
            // 例外是 Presence<> 字段（optional_scalar）：它本身表达「用户是否显式设置」，
            // 没写进缓冲就是缺席，必须如实反映到值树里（不给值树建键）。
            const bool is_scalar_field =
                field.type != FbType::String && field.type != FbType::Table
                && field.type != FbType::Struct && field.type != FbType::VectorScalar
                && field.type != FbType::VectorString && field.type != FbType::VectorTable
                && field.type != FbType::None;

            if (!present && (!is_scalar_field || field.optional_scalar))
                continue;

            Value *slot = out.AddField(field.name, field.id);
            if (!slot)
                continue;

            switch (field.type)
            {
            case FbType::Bool:
                slot->SetBool(t->GetField<uint8_t>(field.voffset, 0) != 0);
                break;
            case FbType::Int8:
                slot->SetInt(t->GetField<int8_t>(field.voffset, 0));
                break;
            case FbType::Int16:
                slot->SetInt(t->GetField<int16_t>(field.voffset, 0));
                break;
            case FbType::Int32:
                slot->SetInt(t->GetField<int32_t>(field.voffset, 0));
                break;
            case FbType::Int64:
                slot->SetInt(t->GetField<int64_t>(field.voffset, 0));
                break;
            case FbType::UInt8:
                slot->SetUInt(t->GetField<uint8_t>(field.voffset, 0));
                break;
            case FbType::UInt16:
                slot->SetUInt(t->GetField<uint16_t>(field.voffset, 0));
                break;
            case FbType::UInt32:
                slot->SetUInt(t->GetField<uint32_t>(field.voffset, 0));
                break;
            case FbType::UInt64:
                slot->SetUInt(t->GetField<uint64_t>(field.voffset, 0));
                break;
            case FbType::Float:
                slot->SetFloat(t->GetField<float>(field.voffset, 0.0f));
                break;
            case FbType::Double:
                slot->SetDouble(t->GetField<double>(field.voffset, 0.0));
                break;
            case FbType::Enum:
            {
                const int64_t raw = t->GetField<int32_t>(field.voffset, 0);
                const char *name = field.enum_to_name ? field.enum_to_name(raw) : nullptr;
                if (name)
                    slot->SetString(name);
                else
                    slot->SetInt(raw);
                break;
            }
            case FbType::String:
            {
                const auto *str = t->GetPointer<const flatbuffers::String *>(field.voffset);
                slot->SetString(str ? str->c_str() : "");
                break;
            }
            case FbType::Struct:
            {
                const int components = (field.components >= 2 && field.components <= 4)
                                           ? field.components
                                           : 3;
                slot->SetArray();
                const float *p = nullptr;
                if (components == 2)
                {
                    const auto *s = t->GetStruct<const Vec2f *>(field.voffset);
                    if (s)
                        p = reinterpret_cast<const float *>(s);
                }
                else if (components == 3)
                {
                    const auto *s = t->GetStruct<const Vec3f *>(field.voffset);
                    if (s)
                        p = reinterpret_cast<const float *>(s);
                }
                else
                {
                    const auto *s = t->GetStruct<const Vec4f *>(field.voffset);
                    if (s)
                        p = reinterpret_cast<const float *>(s);
                }
                if (p)
                {
                    for (int c = 0; c < components; ++c)
                    {
                        Value *item = slot->AddElement();
                        if (item)
                            item->SetFloat(p[c]);
                    }
                }
                break;
            }
            case FbType::Table:
            {
                const auto *sub =
                    t->GetPointer<const flatbuffers::Table *>(field.voffset);
                if (sub)
                    detail::DecodeSubTable(sub, field, *slot);
                break;
            }
            case FbType::VectorScalar:
                switch (field.elem_type)
                {
                case FbType::Bool:
                case FbType::UInt8:
                    detail::DecodeScalarVector<uint8_t>(t, field.voffset, *slot);
                    break;
                case FbType::Int8:
                    detail::DecodeScalarVector<int8_t>(t, field.voffset, *slot);
                    break;
                case FbType::Int16:
                    detail::DecodeScalarVector<int16_t>(t, field.voffset, *slot);
                    break;
                case FbType::UInt16:
                    detail::DecodeScalarVector<uint16_t>(t, field.voffset, *slot);
                    break;
                case FbType::Int32:
                case FbType::Enum:
                    detail::DecodeScalarVector<int32_t>(t, field.voffset, *slot);
                    break;
                case FbType::UInt32:
                    detail::DecodeScalarVector<uint32_t>(t, field.voffset, *slot);
                    break;
                case FbType::Int64:
                    detail::DecodeScalarVector<int64_t>(t, field.voffset, *slot);
                    break;
                case FbType::UInt64:
                    detail::DecodeScalarVector<uint64_t>(t, field.voffset, *slot);
                    break;
                case FbType::Float:
                    detail::DecodeScalarVector<float>(t, field.voffset, *slot);
                    break;
                default:
                    detail::DecodeScalarVector<double>(t, field.voffset, *slot);
                    break;
                }
                break;
            case FbType::VectorString:
            {
                const auto *vec = t->GetPointer<const flatbuffers::Vector<
                    flatbuffers::Offset<flatbuffers::String>> *>(field.voffset);
                slot->SetArray();
                if (vec)
                {
                    for (uint32_t k = 0; k < vec->size(); ++k)
                    {
                        const flatbuffers::String *text = vec->Get(k);
                        Value *item = slot->AddElement();
                        if (item)
                            item->SetString(text ? text->c_str() : "");
                    }
                }
                break;
            }
            case FbType::VectorTable:
            {
                const auto *vec = t->GetPointer<const flatbuffers::Vector<
                    flatbuffers::Offset<flatbuffers::Table>> *>(field.voffset);
                slot->SetArray();
                if (vec && (field.meta || field.table))
                {
                    for (uint32_t k = 0; k < vec->size(); ++k)
                    {
                        Value *item = slot->AddElement();
                        if (!item)
                            continue;
                        detail::DecodeSubTable(vec->Get(k), field, *item);
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

    /// 旧路径：手写描述表 ← 表。
    inline void DecodeTable(const flatbuffers::Table *t, const TableDesc &desc,
                            Value &out)
    {
        DecodeTableFields(t, desc.fields, desc.field_count, out);
    }

    /// 唯一真源路径：反射元数据 ← 表。
    inline void DecodeTable(const flatbuffers::Table *t,
                            const hgl::asset::ClassMetaInfo &meta, Value &out)
    {
        ValueArray<FieldDesc> fields;
        detail::BuildFields(meta, fields);
        DecodeTableFields(t, fields.GetData(), fields.GetCount(), out);
    }

    // ── 校验 ────────────────────────────────────────────────────────────────
    //
    // 照着同一份反射表递归核对缓冲：vtable 项是否指向合法区间、标量宽度是否够、
    // 字符串 / 向量 / 子表偏移是否落在缓冲内。与 flatc 生成的 `Verify()` 等价，
    // 但不需要任何 schema 级生成头。解码前必须先过这一关 —— 坏缓冲在 `GetField()`
    // 里越界是未定义行为。

        /// 字段存在时按 `T` 的宽度校验其内联字节；缺席直接通过。
        /// 直接转调 `Table::VerifyField()` —— 它自己解析 vtable 偏移并处理缺席。
        template <typename T>
        inline bool VerifyScalarField(flatbuffers::Verifier &verifier,
                                      const flatbuffers::Table *t,
                                      const flatbuffers::voffset_t vo,
                                      const size_t align)
        {
            return t->template VerifyField<T>(verifier, vo, align);
        }

        /// 偏移类字段（串 / 表 / 向量）先校验「偏移本身指向缓冲内」，再取值。
        inline bool VerifyFieldOffset(flatbuffers::Verifier &verifier,
                                      const flatbuffers::Table *t,
                                      const flatbuffers::voffset_t vo)
        {
            return t->template VerifyOffset<>(verifier, vo);
        }

    inline bool VerifyTable(flatbuffers::Verifier &verifier, const flatbuffers::Table *t,
                            const hgl::asset::ClassMetaInfo &meta);

    inline bool VerifyTable(flatbuffers::Verifier &verifier, const flatbuffers::Table *t,
                            const hgl::asset::ClassMetaInfo &meta)
    {
        if (!t)
            return true;

        if (!verifier.VerifyTableStart(reinterpret_cast<const uint8_t *>(t)))
            return false;

        ValueArray<FieldDesc> fields;
        detail::BuildFields(meta, fields);

        bool ok = true;
        for (int i = 0; i < fields.GetCount() && ok; ++i)
        {
            const FieldDesc &field = fields[i];
            const flatbuffers::voffset_t vo = field.voffset;

            switch (field.type)
            {
            case FbType::Bool:
            case FbType::UInt8:
                ok = detail::VerifyScalarField<uint8_t>(verifier, t, vo, 1);
                break;
            case FbType::Int8:
                ok = detail::VerifyScalarField<int8_t>(verifier, t, vo, 1);
                break;
            case FbType::Int16:
            case FbType::UInt16:
                ok = detail::VerifyScalarField<uint16_t>(verifier, t, vo, 2);
                break;
            case FbType::Int32:
            case FbType::UInt32:
            case FbType::Float:
            case FbType::Enum:
                ok = detail::VerifyScalarField<uint32_t>(verifier, t, vo, 4);
                break;
            case FbType::Int64:
            case FbType::UInt64:
            case FbType::Double:
                ok = detail::VerifyScalarField<uint64_t>(verifier, t, vo, 8);
                break;
            case FbType::Struct:
            {
                const int components = (field.components >= 2 && field.components <= 4)
                                           ? field.components
                                           : 3;
                if (components == 2)
                    ok = detail::VerifyScalarField<Vec2f>(verifier, t, vo, 4);
                else if (components == 3)
                    ok = detail::VerifyScalarField<Vec3f>(verifier, t, vo, 4);
                else
                    ok = detail::VerifyScalarField<Vec4f>(verifier, t, vo, 4);
                break;
            }
            case FbType::String:
            {
                ok = detail::VerifyFieldOffset(verifier, t, vo);
                if (!ok || t->GetOptionalFieldOffset(vo) == 0)
                    break;
                const auto *str = t->GetPointer<const flatbuffers::String *>(vo);
                ok = verifier.VerifyString(str);
                break;
            }
            case FbType::Table:
            {
                ok = detail::VerifyFieldOffset(verifier, t, vo);
                if (!ok || t->GetOptionalFieldOffset(vo) == 0 || !field.meta)
                    break;
                const auto *sub = t->GetPointer<const flatbuffers::Table *>(vo);
                ok = VerifyTable(verifier, sub, *field.meta);
                break;
            }
            case FbType::VectorScalar:
            {
                ok = detail::VerifyFieldOffset(verifier, t, vo);
                if (!ok || t->GetOptionalFieldOffset(vo) == 0)
                    break;

                switch (field.elem_type)
                {
                case FbType::Bool:
                case FbType::UInt8:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<uint8_t> *>(vo));
                    break;
                case FbType::Int8:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<int8_t> *>(vo));
                    break;
                case FbType::Int16:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<int16_t> *>(vo));
                    break;
                case FbType::UInt16:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<uint16_t> *>(vo));
                    break;
                case FbType::Int32:
                case FbType::Enum:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<int32_t> *>(vo));
                    break;
                case FbType::UInt32:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<uint32_t> *>(vo));
                    break;
                case FbType::Int64:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<int64_t> *>(vo));
                    break;
                case FbType::UInt64:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<uint64_t> *>(vo));
                    break;
                case FbType::Float:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<float> *>(vo));
                    break;
                case FbType::Double:
                    ok = verifier.VerifyVector(
                        t->GetPointer<const flatbuffers::Vector<double> *>(vo));
                    break;
                default:
                    break;
                }
                break;
            }
            case FbType::VectorString:
            {
                ok = detail::VerifyFieldOffset(verifier, t, vo);
                if (!ok || t->GetOptionalFieldOffset(vo) == 0)
                    break;
                const auto *vec = t->GetPointer<const flatbuffers::Vector<
                    flatbuffers::Offset<flatbuffers::String>> *>(vo);
                ok = verifier.VerifyVector(vec) && verifier.VerifyVectorOfStrings(vec);
                break;
            }
            case FbType::VectorTable:
            {
                ok = detail::VerifyFieldOffset(verifier, t, vo);
                if (!ok || t->GetOptionalFieldOffset(vo) == 0 || !field.meta)
                    break;
                const auto *vec = t->GetPointer<const flatbuffers::Vector<
                    flatbuffers::Offset<flatbuffers::Table>> *>(vo);
                if (!verifier.VerifyVector(vec))
                {
                    ok = false;
                    break;
                }
                if (!vec)
                    break;
                for (uint32_t k = 0; k < vec->size() && ok; ++k)
                    ok = VerifyTable(verifier, vec->Get(k), *field.meta);
                break;
            }
            default:
                break;
            }
        }

        verifier.EndTable();
        return ok;
    }
}//namespace detail

    // ── 缓冲级入口 ──────────────────────────────────────────────────────────

    /// 完整缓冲的根级校验（含可选的 file identifier 与根表递归校验）。
    bool VerifyBuffer(const void *data, const size_t size,
                      const hgl::asset::ClassMetaInfo &meta,
                      const char *file_identifier)
    {
        if (!data || size < FLATBUFFERS_MIN_BUFFER_SIZE)
            return false;

        const auto *bytes = static_cast<const uint8_t *>(data);

        if (file_identifier && file_identifier[0]
            && !flatbuffers::BufferHasIdentifier(bytes, file_identifier))
            return false;

        /// 根偏移自身必须先落在缓冲内，之后 `VerifyTableStart()` 才能安全读 vtable。
        const auto root_off = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(bytes);
        if (root_off < sizeof(flatbuffers::uoffset_t) || root_off + sizeof(int32_t) > size)
            return false;

        flatbuffers::Verifier verifier(bytes, size);
        const auto *root =
            reinterpret_cast<const flatbuffers::Table *>(bytes + root_off);

        return detail::VerifyTable(verifier, root, meta);
    }

    /// 值树 → 完整 FlatBuffers 缓冲（旧路径：手写描述表）。
    bool EncodeToBuffer(const Value &root, const TableDesc &desc,
                        const char *file_identifier, ValueArray<uint8_t> &out_bytes)
    {
        out_bytes.Clear();

        flatbuffers::FlatBufferBuilder fbb;
        const flatbuffers::Offset<void> root_offset = detail::EncodeTable(fbb, root, desc);
        if (file_identifier && file_identifier[0])
            fbb.Finish(root_offset, file_identifier);
        else
            fbb.Finish(root_offset);

        return CopyBuilderTo(fbb, out_bytes);
    }

    /// 值树 → 完整 FlatBuffers 缓冲（唯一真源路径：反射元数据）。
    bool EncodeToBuffer(const Value &root, const hgl::asset::ClassMetaInfo &meta,
                        const char *file_identifier, ValueArray<uint8_t> &out_bytes)
    {
        out_bytes.Clear();

        flatbuffers::FlatBufferBuilder fbb;
        const flatbuffers::Offset<void> root_offset = detail::EncodeTable(fbb, root, meta);
        if (file_identifier && file_identifier[0])
            fbb.Finish(root_offset, file_identifier);
        else
            fbb.Finish(root_offset);

        return CopyBuilderTo(fbb, out_bytes);
    }

    /// 完整 FlatBuffers 缓冲 → 值树（旧路径）。强校验请先跑 `VerifyBuffer`。
    bool DecodeFromBuffer(const uint8_t *data, const size_t size,
                          const TableDesc &desc, Value &out)
    {
        if (!data || size < FLATBUFFERS_MIN_BUFFER_SIZE)
            return false;

        const flatbuffers::Table *root =
            flatbuffers::GetRoot<flatbuffers::Table>(data);
        if (!root)
            return false;

        detail::DecodeTable(root, desc, out);
        return true;
    }

    /**
     * @brief 完整 FlatBuffers 缓冲 → 值树（唯一真源路径）。
     *
     * **不做边界校验** —— 调用方要么先跑 `VerifyBuffer()`（推荐，或直接用
     * `VerifyAndDecodeFlatBuffers<T>()`），要么自己保证缓冲可信。
     */
    bool DecodeFromBuffer(const uint8_t *data, const size_t size,
                          const hgl::asset::ClassMetaInfo &meta, Value &out)
    {
        if (!data || size < FLATBUFFERS_MIN_BUFFER_SIZE)
            return false;

        const flatbuffers::Table *root =
            flatbuffers::GetRoot<flatbuffers::Table>(data);
        if (!root)
            return false;

        detail::DecodeTable(root, meta, out);
        return true;
    }
}//namespace hgl::serialize::fb
