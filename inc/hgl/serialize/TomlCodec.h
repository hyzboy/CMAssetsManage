#pragma once

/**
 * TOML 后端 —— `hgl::serialize::Value` 树 ↔ TOML 文本的机械搬运。
 *
 * 这一层不含任何资产语义：不做字段名校验、不做范围校验、不做缺省值填充。
 * 那些全部发生在 `Serialize()`（唯一一份生成代码）里。
 *
 * ── 类型映射 ──────────────────────────────────────────────────────────────
 * | Value      | TOML |
 * |---|---|
 * | Bool       | boolean |
 * | Int        | integer |
 * | UInt       | integer（超出 int64 时退化为浮点，附注释） |
 * | Float/Double | float |
 * | String     | string（UTF-8） |
 * | Array      | array |
 * | Table      | table（有序键，与声明顺序一致） |
 *
 * ── 分层 ──────────────────────────────────────────────────────────────────
 * 本头是**纯 HGL 接口**：只有「文本 ⇄ 值树」的入口、诊断结构与类型化模板，
 * 不出现任何 toml11 符号。toml11 的 DOM 转换与格式化都在 `TomlCodec.cpp` 里，
 * 因此它只是 `CMAssetsManage` 的实现细节，不会外溢到使用方。
 */

#include <hgl/serialize/Archive.h>
#include <hgl/serialize/Value.h>

#include <cstdint>
#include <cstring>
namespace hgl::serialize
{
    /**
     * @brief TOML 解析失败详情。
     *
     * 后端只负责「文本 → 值树」，但解析失败时**必须**把行号带出来 —— 资产文件动辄
     * 数百行，只报「解析失败」等于没报。行号约定 **1 起**，0 表示无法定位。
     */
    struct TomlParseError
    {
        int line = 0;         ///< 出错行（1 起，0 = 未知）
        AnsiString title;     ///< 错误类别（toml11 的 title，如 `syntax error`）
        AnsiString detail;    ///< 该行的具体说明

        /// 拼成一句可直接展示的诊断文本。
        AnsiString Format() const
        {
            AnsiString result;

            if (line > 0)
            {
                result += AnsiString("第 ");
                result += AnsiString::numberOf(line);
                result += AnsiString(" 行：");
            }

            if (detail.Length() == 0)
            {
                result += (title.Length() > 0) ? title : AnsiString("TOML 语法错误");
                return result;
            }

            result += detail;

            // 具体说明与类别不同时补上类别，否则「第 3 行：bad = = 2」看不出为什么错。
            if (title.Length() > 0 && hgl::stricmp(title.c_str(), detail.c_str()) != 0)
            {
                result += AnsiString("（");
                result += title;
                result += AnsiString("）");
            }

            return result;
        }
    };

    // ── TOML 文本 ⇄ Value 树（实现在 TomlCodec.cpp）─────────────────────────

    /// 便捷入口：解析 NUL 结尾文本 → 值树（不取诊断详情）。
    bool FromTomlText(const char *text, Value &out);

    /**
     * @brief 解析文本 → Value 树，失败时给出行号。
     *
     * @param text      文本起点
     * @param size      字节数（允许文本里出现 `\0`）
     * @param out       目标值树；失败时内容未定义
     * @param out_error 可选输出：解析失败详情
     */
    bool FromTomlText(const char *text, size_t size, Value &out,
                      TomlParseError *out_error = nullptr);

    /// 便捷入口：Value 树 → TOML 文本。
    AnsiString ToTomlText(const Value &src);
    /**
     * @brief 文本 → `T`：解析成值树后立刻整体读成结构。
     *
     * 这是资产文件读取的**唯一**入口。文本层只产出值树；字段名、类型、缺键语义、
     * 枚举拼写表全部来自生成的 `StructSerializer<T>`，本函数不认任何具体资产类型。
     *
     * @param out_archive 可选输出：序列化阶段（而非解析阶段）的失败详情
     * @param out_error   可选输出：TOML 解析阶段的失败详情
     */
    template <typename T>
    inline bool LoadFromTomlText(const char *text, const size_t size, T &out,
                                 Archive *out_archive = nullptr,
                                 TomlParseError *out_error = nullptr)
    {
        Value root;
        if (!FromTomlText(text, size, root, out_error))
            return false;

        return ReadFromValue<T>(root, out, out_archive);
    }

    /// 便捷重载：以 NUL 结尾的文本。
    template <typename T>
    inline bool LoadFromTomlText(const char *text, T &out,
                                 Archive *out_archive = nullptr,
                                 TomlParseError *out_error = nullptr)
    {
        return LoadFromTomlText<T>(text, text ? std::strlen(text) : 0u, out,
                                   out_archive, out_error);
    }
    /**
     * @brief `T` → TOML 文本：整体写成值树后一次格式化。
     *
     * @param out_archive 可选输出：序列化阶段的失败详情
     * @return 成功时返回文本；失败返回空串（详情见 `out_archive`）
     */
    template <typename T>
    inline AnsiString SaveToTomlText(const T &src, Archive *out_archive = nullptr)
    {
        Value root;

        Archive archive = Archive::ForWrite(root);
        archive.WriteTo(root, src);

        const bool ok = archive.Ok();

        if (out_archive)
            *out_archive = std::move(archive);

        if (!ok)
            return AnsiString();

        return ToTomlText(root);
    }
}//namespace hgl::serialize
