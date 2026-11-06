// TestAssetID.cpp —— 资产身份标识的契约判据。
//
// 锁定四件事：
//   1. 生成：非全零、版本/变体位正确、跨调用全局唯一且单调递增（v7 的意义）；
//   2. 文本往返：32 位无连字符拼写与 8-4-4-4-12 标准形态互相可解析；
//   3. 严格解析：必须拒绝非法字符、长度不足、全零（身份未分配）；
//   4. ID 寻址定位段：`id:<32 hex>` 是窄判据，其它任何形态都不得命中。
#include <hgl/asset/AssetID.h>

#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/type/UnorderedMap.h>

using namespace hgl;
using namespace hgl::asset;

namespace
{
    int g_fail = 0;
    int g_pass = 0;

    AnsiString Narrow(const AnsiString &str)
    {
        return str;
    }

    void Check(bool cond, const char *what, const char *detail = "")
    {
        if (cond)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s %s", what, detail);
    }

    void CheckText(const AnsiString &actual, const AnsiString &expect, const char *what)
    {
        if (actual == expect)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s 实际=[%s] 期望=[%s]",
                  what, Narrow(actual).c_str(), Narrow(expect).c_str());
    }

    // ── 生成 ───────────────────────────────────────────────────────────
    void TestCreate()
    {
        const AssetID id = AssetID::Create();

        Check(!id.IsZero(), "Create 不得返回全零");

        // 版本位：UUID 文本第 13 个字符，即 high 的 bit 12..15
        Check(((id.high >> 12) & 0x0F) == 7, "版本位必须为 7");

        // 变体位：low 的 bit 62..63 必须是 0b10
        Check(((id.low >> 62) & 0x03) == 0x02, "变体位必须为 0b10");

        // 唯一性 + 单调性：v7 的核心承诺是按创建顺序可排序
        constexpr int kCount = 512;

        AssetID ids[kCount];

        int duplicate = 0;
        int disorder = 0;

        for (int i = 0; i < kCount; ++i)
        {
            ids[i] = AssetID::Create();

            if (i == 0)
                continue;

            if (!(ids[i - 1] < ids[i]))
                ++disorder;

            for (int j = 0; j < i; ++j)
                if (ids[j] == ids[i])
                    ++duplicate;
        }

        Check(duplicate == 0, "连续生成的 512 个 ID 不得重复");
        Check(disorder == 0, "连续生成的 ID 必须严格单调递增");

        AssetID cleared = id;

        cleared.Clear();
        Check(cleared.IsZero(), "Clear 后应为全零");
    }

    // ── 文本往返 ───────────────────────────────────────────────────────
    void TestText()
    {
        const AssetID a = AssetID::Create();

        const AnsiString text = a.ToString();

        Check(text.Length() == AssetID::kTextLength, "ToString 长度必须是 32");

        bool all_hex = true;

        for (int i = 0; i < text.Length(); ++i)
        {
            const char ch = text.c_str()[i];

            const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');

            if (!ok)
                all_hex = false;
        }

        Check(all_hex, "ToString 必须是 32 个小写十六进制字符");

        AssetID back;

        Check(AssetID::Parse(text, back), "Parse(ToString(x)) 必须成功");
        Check(back == a, "Parse(ToString(x)) == x");

        // 标准 UUID 形态
        const AnsiString uuid = a.ToUUIDString();

        Check(uuid.Length() == AssetID::kUUIDTextLength, "ToUUIDString 长度必须是 36");
        Check(uuid.c_str()[8] == '-' && uuid.c_str()[13] == '-'
           && uuid.c_str()[18] == '-' && uuid.c_str()[23] == '-',
              "ToUUIDString 连字符位置必须是 8/13/18/23");

        AssetID uuid_back;

        Check(AssetID::Parse(uuid, uuid_back), "Parse(UUID 形态) 必须成功");
        Check(uuid_back == a, "UUID 形态往返一致");

        // 大写同样可解析
        AnsiString upper = text;

        for (int i = 0; i < upper.Length(); ++i)
        {
            char ch = upper.c_str()[i];

            if (ch >= 'a' && ch <= 'f')
                ch = char(ch - 'a' + 'A');

            upper[i] = ch;
        }

        AssetID upper_back;

        Check(AssetID::Parse(upper, upper_back), "大写形态必须可解析");
        Check(upper_back == a, "大写往返一致");
    }

    // ── 字节往返 ───────────────────────────────────────────────────────
    void TestBytes()
    {
        const AssetID a = AssetID::Create();

        uint8 bytes[AssetID::kByteCount];

        a.ToBytes(bytes);

        const AssetID b = AssetID::FromBytes(bytes);

        Check(a == b, "字节往返一致");

        // 大端：high 的最高字节在最前
        Check(bytes[0] == uint8((a.high >> 56) & 0xFF), "字节序为大端（high 高字节在前）");
        Check(bytes[15] == uint8(a.low & 0xFF), "字节序为大端（low 低字节在后）");

        Check(AssetID::FromBytes(nullptr).IsZero(), "FromBytes(nullptr) 返回全零");

        const AssetID zero = AssetID::FromBytes(nullptr);
        uint8 zero_bytes[AssetID::kByteCount];

        zero.ToBytes(zero_bytes);
        Check(zero_bytes[0] == 0 && zero_bytes[15] == 0, "全零写出全零");
    }

    // ── 严格解析的拒绝清单 ─────────────────────────────────────────────
    void TestParseReject()
    {
        AssetID out;

        out = AssetID::Create();

        Check(!AssetID::Parse("", out), "空串必须拒绝");
        Check(out.IsZero(), "拒绝时必须置零");

        Check(!AssetID::Parse("0123456789abcdef", out), "长度不足必须拒绝");
        Check(!AssetID::Parse("0123456789abcdef0123456789abcdef0", out), "长度超出必须拒绝");
        Check(!AssetID::Parse("0123456789abcdef0123456789abcdeg", out), "非十六进制字符必须拒绝");
        Check(!AssetID::Parse("0123456789abcdef0123456789abcde", out), "少一位必须拒绝");
        Check(!AssetID::Parse("00000000000000000000000000000000", out), "全零必须拒绝");
        Check(!AssetID::Parse(nullptr, out), "nullptr 必须拒绝");

        // 宽松解析：跳过连字符，但不能把非法前缀一起吃进去
        const AssetID id = AssetID::FromString("0123456789abcdef0123456789ABCDEF");

        Check(!id.IsZero(), "FromString 大小写混合可解析");

        const AssetID dashed = AssetID::FromString("01234567-89ab-cdef-0123-456789abcdef");

        Check(dashed == id, "FromString 跳过连字符");

        Check(AssetID::FromString("").IsZero(), "FromString 空串为全零");
        Check(AssetID::FromString(nullptr).IsZero(), "FromString nullptr 为全零");
    }

    // ── AsseADID 作为 UnorderedMap 键 ──────────────────────────────────
    void TestAsMapKey()
    {
        UnorderedMap<AssetID, int> map;

        AssetID ids[8];

        for (int i = 0; i < 8; ++i)
        {
            ids[i] = AssetID::Create();

            Check(map.Add(ids[i], i), "Add 新键必须成功");
        }

        Check(map.GetCount() == 8, "map 计数为 8");

        bool all_hit = true;

        for (int i = 0; i < 8; ++i)
        {
            int value = -1;

            if (!map.Get(ids[i], value) || value != i)
                all_hit = false;
        }

        Check(all_hit, "按 ID 取回原值");

        Check(!map.Add(ids[0], 99), "重复键 Add 必须失败");

        Check(map.DeleteByKey(ids[3]), "DeleteByKey 成功");
        Check(map.GetCount() == 7, "删除后计数为 7");
    }

    // ── ID 寻址定位段 ──────────────────────────────────────────────────
    void TestIDTarget()
    {
        const AssetID id = AssetID::Create();

        const U8String target = MakeAssetIDTarget(id);

        Check(target.Length() == 35, "id: 定位段长度必须是 35");

        AssetID parsed;

        Check(ParseAssetIDTarget(target, parsed), "ParseAssetIDTarget 命中");
        Check(parsed == id, "ParseAssetIDTarget 值与原文一致");

        // 大写的 ID 定位段也要能读
        const AnsiString hex = id.ToString();
        U8String upper_target(u8"id:");

        for (int i = 0; i < hex.Length(); ++i)
        {
            char ch = hex.c_str()[i];

            if (ch >= 'a' && ch <= 'f')
                ch = char(ch - 'a' + 'A');

            upper_target += U8String(reinterpret_cast<const u8char *>(&ch), 1);
        }

        AssetID upper_parsed;

        Check(ParseAssetIDTarget(upper_target, upper_parsed), "大写 ID 定位段可解析");
        Check(upper_parsed == id, "大写 ID 定位段值一致");

        // 窄判据：任何不像 `id:<32hex>` 的串都必须落回普通路径
        const U8String reject[] =
        {
            U8String(u8"id:"),
            U8String(u8"id"),
            U8String(u8"models/hero"),
            U8String(u8"id:1234"),
            U8String(u8"id:0123456789abcdef0123456789abcde"),     // 少一位
            U8String(u8"id:0123456789abcdef0123456789abcdef0"),   // 多一位
            U8String(u8"id:0123456789abcdef0123456789abcdeg"),    // 非十六进制
            U8String(u8"ID:0123456789abcdef0123456789abcdef"),    // 前缀大小写不匹配
            U8String(u8"xx:0123456789abcdef0123456789abcdef"),
            U8String(u8"id:00000000000000000000000000000000"),    // 全零
        };

        bool all_reject = true;

        for (const U8String &bad : reject)
        {
            AssetID tmp;

            if (ParseAssetIDTarget(bad, tmp))
                all_reject = false;
        }

        Check(all_reject, "非法定位段必须全部拒绝");
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetID"));

    TestCreate();
    TestText();
    TestBytes();
    TestParseReject();
    TestAsMapKey();
    TestIDTarget();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetID: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetID: %d 项全部通过 ===", g_pass);
    return 0;
}
