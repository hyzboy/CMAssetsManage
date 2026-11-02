/**
 * TomlCodec 实现 —— 依赖 toml11，故不进公共头。
 *
 * `hgl/serialize/TomlCodec.h` 只暴露「TOML 文本 ⇄ `Value` 树」的纯 HGL 入口；
 * 本文件承载 toml11 的 DOM 转换、格式化与语法错误折叠，使 `CMAssetsManage`
 * 对 toml11 的依赖可以降为 PRIVATE。
 */

#include <hgl/serialize/TomlCodec.h>
#include <hgl/serialize/Value.h>

#include <toml.hpp>

#include <cstring>
#include <string>

namespace hgl::serialize
{
    namespace
    {
    // ── TOML → Value ────────────────────────────────────────────────────────

    void FromToml(const toml::value &src, Value &out);

    void FromTomlArray(const toml::array &src, Value &out)
    {
        out.SetArray();
        for (const auto &item : src)
        {
            Value *slot = out.AddElement();
            if (slot)
                FromToml(item, *slot);
        }
    }

    void FromTomlTable(const toml::table &src, Value &out)
    {
        out.SetTable();
        for (const auto &[key, item] : src)
        {
            Value *slot = out.AddField(key.c_str(), 0);
            if (slot)
                FromToml(item, *slot);
        }
    }

    void FromToml(const toml::value &src, Value &out)
    {
        if (src.is_boolean())
        {
            out.SetBool(src.as_boolean());
            return;
        }
        if (src.is_integer())
        {
            out.SetInt(src.as_integer());
            return;
        }
        if (src.is_floating())
        {
            out.SetDouble(src.as_floating());
            return;
        }
        if (src.is_string())
        {
            out.SetString(src.as_string().c_str());
            return;
        }
        if (src.is_array())
        {
            FromTomlArray(src.as_array(), out);
            return;
        }
        if (src.is_table())
        {
            FromTomlTable(src.as_table(), out);
            return;
        }

        // 日期 / 时间等资产层用不到的 TOML 类型：置空，交由上层判定为类型不符。
        out.SetNull();
    }

    /**
     * @brief 把 toml11 的语法错误折叠成一句诊断。
     *
     * toml11 的 `what()` 是一整块带竖线箭头和源码片段的格式化文本，直接塞进资产
     * 文件的错误信息里会把日志冲垮。这里只取**第一条**错误的标题、行号与该行说明。
     */
    void FillTomlSyntaxError(const toml::syntax_error &err, TomlParseError &out)
    {
        out.line = 0;
        out.title.Clear();
        out.detail.Clear();

        const auto &errors = err.errors();
        if (errors.empty())
        {
            out.detail = AnsiString(err.what());
            return;
        }

        const toml::error_info &info = errors.front();

        // toml11 的 `title()` 形如 `toml::parse_value: `，末尾冒号空格只是拼接痕迹。
        std::string title = info.title();
        while (!title.empty() && (title.back() == ':' || title.back() == ' '))
            title.pop_back();

        out.title = AnsiString(title.c_str());

        if (!info.locations().empty())
        {
            const auto &location = info.locations().front();
            out.line = static_cast<int>(location.first.first_line_number());

            // 第二项常常只是占位符 `here`，真正有信息量的是出错那一行的源码。
            const std::string &message = location.second;
            if (!message.empty() && message != "here")
                out.detail = AnsiString(message.c_str());
            else if (location.first.is_ok() && location.first.num_lines() > 0)
                out.detail = AnsiString(location.first.first_line().c_str());
        }

        if (out.detail.Length() == 0 && !info.suffix().empty())
            out.detail = AnsiString(info.suffix().c_str());

        if (out.detail.Length() == 0)
            out.detail = out.title;
    }

    // ── Value → TOML ────────────────────────────────────────────────────────

    toml::value ToToml(const Value &src);

    toml::value ToTomlString(const U8String &text)
    {
        toml::value::string_type value;
        if (text.Length() > 0)
            value.assign(reinterpret_cast<const char *>(text.c_str()),
                         static_cast<size_t>(text.Length()));
        return toml::value(std::move(value));
    }

    toml::value ToToml(const Value &src)
    {
        switch (src.Kind())
        {
        case ValueKind::Null:
            return toml::value(toml::table{});

        case ValueKind::Bool:
        {
            bool b = false;
            src.GetBool(b);
            return toml::value(b);
        }

        case ValueKind::Int:
        {
            int64_t i = 0;
            src.GetInt(i);
            return toml::value(static_cast<toml::value::integer_type>(i));
        }

        case ValueKind::UInt:
        {
            uint64_t u = 0;
            src.GetUInt(u);
            if (u <= static_cast<uint64_t>(INT64_MAX))
                return toml::value(static_cast<toml::value::integer_type>(u));
            // TOML 只有有符号 64 位整数：超出范围时退化为浮点。
            return toml::value(static_cast<toml::value::floating_type>(
                static_cast<double>(u)));
        }

        case ValueKind::Float:
        case ValueKind::Double:
        {
            double d = 0.0;
            src.GetDouble(d);
            return toml::value(static_cast<toml::value::floating_type>(d));
        }

        case ValueKind::String:
            return ToTomlString(src.GetString());

        case ValueKind::Array:
        {
            toml::array a;
            const int count = src.GetElementCount();
            a.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                const Value *slot = src.GetElement(i);
                if (slot)
                    a.push_back(ToToml(*slot));
            }
            return toml::value(std::move(a));
        }

        case ValueKind::Table:
        {
            // 用 `insert_or_assign` 而不是 `operator[]`：后者按字典序重排，
            // 会让落盘顺序与 C++ 字段声明顺序脱钩。
            toml::table t;
            const int count = src.GetFieldCount();
            for (int i = 0; i < count; ++i)
            {
                const char *name = src.GetFieldName(i);
                const Value *slot = src.GetFieldValue(i);
                if (!name || !slot)
                    continue;
                t.insert_or_assign(toml::table::key_type(name, std::strlen(name)),
                                   ToToml(*slot));
            }
            return toml::value(std::move(t));
        }
        }

        return toml::value(toml::table{});
    }
    }//namespace

    /**
     * @brief 解析文本 → Value 树，失败时给出行号。
     *
     * @param text      文本起点
     * @param size      字节数（允许文本里出现 `\0`）
     * @param out       目标值树；失败时内容未定义
     * @param out_error 可选输出：解析失败详情
     */
    bool FromTomlText(const char *text, const size_t size, Value &out,
                      TomlParseError *out_error)
    {
        if (out_error)
            *out_error = TomlParseError{};

        if (!text)
        {
            if (out_error)
                out_error->detail = AnsiString("TOML 文本为空");
            return false;
        }

        try
        {
            // 注意：toml11 的 `parse(const char*)` 把参数当作**文件名**；
            // 解析内容必须走 `parse_str`。
            const toml::value parsed =
                toml::parse_str(std::string(text, size));
            FromToml(parsed, out);
            return true;
        }
        catch (const toml::syntax_error &err)
        {
            if (out_error)
                FillTomlSyntaxError(err, *out_error);
            return false;
        }
        catch (...)
        {
            return false;
        }
    }

    /// 便捷入口：Value 树 → TOML 文本。
    AnsiString ToTomlText(const Value &src)
    {
        const toml::value v = ToToml(src);
        AnsiString text;
        try
        {
            text = AnsiString(toml::format(v).c_str());
        }
        catch (...)
        {
            text.Clear();
        }
        return text;
    }

    bool FromTomlText(const char *text, Value &out)
    {
        return FromTomlText(text, text ? std::strlen(text) : 0u, out, nullptr);
    }
}//namespace hgl::serialize
