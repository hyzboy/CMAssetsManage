#include<hgl/asset/AssetID.h>
#include<hgl/sync/Mutex.h>
#include<hgl/time/Time.h>
#include<hgl/util/hash/wyhash.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/io/FileInputStream.h>

namespace hgl::asset
{
    namespace
    {
        // ── 十六进制 ────────────────────────────────────────────────────────
        constexpr char kHexLower[] = "0123456789abcdef";

        inline int HexValue(const char ch)
        {
            if(ch>='0'&&ch<='9')return ch-'0';
            if(ch>='a'&&ch<='f')return ch-'a'+10;
            if(ch>='A'&&ch<='F')return ch-'A'+10;

            return -1;
        }

        inline bool IsUUIDSeparator(const char ch)
        {
            // 只把 RFC 形态的连字符视为可跳过；其它杂字符一律视为非法，
            // 否则严格解析就退化成了宽松解析。
            return ch=='-';
        }

        inline uint64 ReadBE64(const uint8 *p)
        {
            uint64 v = 0;

            for(int i=0;i<8;i++)
                v = (v<<8) | p[i];

            return v;
        }

        inline void WriteBE64(uint8 *p,const uint64 v)
        {
            for(int i=0;i<8;i++)
                p[i] = uint8((v >> ((7-i)*8)) & 0xFF);
        }

        /// rand_b 的有效位数：62（最高两位被变体位占据）。
        constexpr uint64 kRandBMask = 0x3FFFFFFFFFFFFFFFull;

        /// rand_a 的有效位数：12。
        constexpr uint64 kRandAMask = 0x0FFFull;
    }//namespace

    // ── 生成 ────────────────────────────────────────────────────────────────

    AssetID AssetID::Create()
    {
        // RFC 9562 §5.7：
        //   unix_ts_ms | ver(4)=7 | rand_a(12) | var(2)=0b10 | rand_b(62)
        //
        // ── 为什么 rand_b 是「每毫秒取一次随机起点、之后逐次 +1」 ──────────────
        // 纯粹的逐次随机只能保证「不重复」，保证不了「可排序」—— 同一毫秒内
        // 一批资产的身份排序会完全随机。而身份序 == 创建序是索引表与打包文件
        // 布局的基础（有序 ⇒ 二分查找、磁盘局部性、清单 diff 稳定）。
        //
        // 因此：
        //   · 新的一毫秒：rand_a 与 rand_b 各取一次随机起点（保证跨进程/跨机器
        //     的离散度），rand_b 起点落在 62 位可用空间内；
        //   · 同一毫秒内：rand_a 不变，rand_b 严格 +1。
        // 比较序 = (ts, rand_a, rand_b)，于是「后创建的一定更大」，且必然唯一。
        // rand_a 相同是刻意的：它在同毫秒内一旦变化，rand_b 就不再是唯一决定序。
        //
        // 3. 一把互斥锁 —— 多线程同时导入资产时不产生撕裂（计数器的 +1 必须
        //    与 rand_b 的读取原子地成对发生）。

        static hgl::Mutex lock;
        static uint64     rand_seed = 0;
        static uint64     last_ts   = 0;
        static uint64     rand_a    = 0;
        static uint64     rand_b    = 0;

        hgl::MutexLock guard(&lock);

        if(rand_seed==0)
            rand_seed = hgl::GetUUIDv7Timestamp() ^ 0x9E3779B97F4A7C15ull;

        uint64 ts = hgl::GetUUIDv7Timestamp();

        if(ts > last_ts)
        {
            // 新的一毫秒：重置随机起点。
            last_ts = ts;

            rand_a = wyrand(&rand_seed) & kRandAMask;
            rand_b = wyrand(&rand_seed) & kRandBMask;
        }
        else
        {
            // 同一毫秒，或系统时钟回拨。回拨时冻结在最后见过的时间戳上，
            // 宁可让若干毫秒「挤在一起」，也不让身份序倒退 —— 倒退会让
            // 有序索引表与追加写的打包文件同时失效。
            ts = last_ts;

            ++rand_b;

            if(rand_b > kRandBMask)
            {
                // 62 位溢出：单毫秒内生成 2^62 个资产才会走到这里。换一组
                // 随机起点兜底，代价是打破一次单调性，但绝不重复。
                rand_a = wyrand(&rand_seed) & kRandAMask;
                rand_b = wyrand(&rand_seed) & kRandBMask;
            }
        }

        AssetID id;

        id.high = ((ts & 0x0000FFFFFFFFFFFFull) << 16)
                | 0x7000ull
                | rand_a;

        id.low  = 0x8000000000000000ull
                | rand_b;

        return id;
    }

    // ── 字节互转 ────────────────────────────────────────────────────────────

    AssetID AssetID::FromBytes(const uint8 *bytes)
    {
        AssetID id;

        if(!bytes)
            return id;

        id.high = ReadBE64(bytes);
        id.low  = ReadBE64(bytes+8);

        return id;
    }

    void AssetID::ToBytes(uint8 *out) const
    {
        if(!out)
            return;

        WriteBE64(out,  high);
        WriteBE64(out+8,low);
    }

    // ── 文本互转 ────────────────────────────────────────────────────────────

    AssetID AssetID::FromString(const char *text)
    {
        AssetID id;

        if(!text)
            return id;

        uint8 bytes[kByteCount];
        int   byte_index = 0;
        int   half       = 0;      // 0 = 高半字节，1 = 低半字节

        for(const char *p=text;*p;p++)
        {
            if(IsUUIDSeparator(*p))
                continue;

            const int v = HexValue(*p);

            if(v<0)
            {
                // 宽松解析：非法字符直接结束，已解析到的部分保留。
                break;
            }

            if(byte_index>=kByteCount)
            {
                // 超出 32 个有效字符：视为输入非法，整体作废。
                return AssetID();
            }

            if(half==0)
            {
                bytes[byte_index] = uint8(v<<4);
                half              = 1;
            }
            else
            {
                bytes[byte_index] |= uint8(v);
                ++byte_index;
                half = 0;
            }
        }

        if(byte_index!=kByteCount || half!=0)
            return AssetID();

        return FromBytes(bytes);
    }

    AssetID AssetID::FromString(const AnsiString &text)
    {
        return FromString(text.c_str());
    }

    bool AssetID::Parse(const char *text, AssetID &out)
    {
        out.Clear();

        if(!text)
            return false;

        uint8 bytes[kByteCount];
        int   byte_index = 0;
        int   half       = 0;

        for(const char *p=text;*p;p++)
        {
            if(IsUUIDSeparator(*p))
                continue;

            const int v = HexValue(*p);

            if(v<0)
                return false;

            if(byte_index>=kByteCount)
                return false;

            if(half==0)
            {
                bytes[byte_index] = uint8(v<<4);
                half              = 1;
            }
            else
            {
                bytes[byte_index] |= uint8(v);
                ++byte_index;
                half = 0;
            }
        }

        if(byte_index!=kByteCount || half!=0)
            return false;

        out = FromBytes(bytes);

        // 全零在身份语义里是「未分配」，不允许从外部文本注入。
        return !out.IsZero();
    }

    bool AssetID::Parse(const AnsiString &text, AssetID &out)
    {
        return Parse(text.c_str(), out);
    }

    void AssetID::ToString(AnsiString &out) const
    {
        const uint8 bytes[kByteCount] =
        {
            uint8((high>>56)&0xFF), uint8((high>>48)&0xFF), uint8((high>>40)&0xFF), uint8((high>>32)&0xFF),
            uint8((high>>24)&0xFF), uint8((high>>16)&0xFF), uint8((high>> 8)&0xFF), uint8( high     &0xFF),
            uint8((low >>56)&0xFF), uint8((low >>48)&0xFF), uint8((low >>40)&0xFF), uint8((low >>32)&0xFF),
            uint8((low >>24)&0xFF), uint8((low >>16)&0xFF), uint8((low >> 8)&0xFF), uint8( low     &0xFF),
        };

        char text[kTextLength];

        for(int i=0;i<kByteCount;i++)
        {
            text[i*2  ] = kHexLower[(bytes[i]>>4)&0x0F];
            text[i*2+1] = kHexLower[ bytes[i]    &0x0F];
        }

        out = AnsiString(text,kTextLength);
    }

    AnsiString AssetID::ToString() const
    {
        AnsiString result;

        ToString(result);

        return result;
    }

    void AssetID::ToUUIDString(AnsiString &out) const
    {
        AnsiString hex;

        ToString(hex);

        const char *src = hex.c_str();

        char text[kUUIDTextLength];

        int dst = 0;

        for(int i=0;i<kTextLength;i++)
        {
            if(i==8||i==12||i==16||i==20)
                text[dst++]='-';

            text[dst++]=src[i];
        }

        out = AnsiString(text,kUUIDTextLength);
    }

    AnsiString AssetID::ToUUIDString() const
    {
        AnsiString result;

        ToUUIDString(result);

        return result;
    }

    // ── ID 寻址定位段 ───────────────────────────────────────────────────────

    bool ParseAssetIDTarget(const U8String &target, AssetID &out)
    {
        out.Clear();

        const int len = target.Length();

        // "id:" + 32 字符 = 35 是唯一合法长度。定长判据把「叫 id: 开头」的
        // 普通目录彻底排除在外，无需再做什么启发式。
        if(len != 3 + AssetID::kTextLength)
            return false;

        const u8char *str = target.c_str();

        if(!str)
            return false;

        if(str[0]!=u8char('i')||str[1]!=u8char('d')||str[2]!=u8char(':'))
            return false;

        // 定位段用 UTF-8 存的，但 ID 的拼写是纯 ASCII，逐字符窄化即可。
        char text[AssetID::kTextLength+1];

        for(int i=0;i<AssetID::kTextLength;i++)
        {
            const u8char ch = str[3+i];

            if(ch>0x7F)
                return false;

            text[i]=char(ch);
        }

        text[AssetID::kTextLength]=0;

        return AssetID::Parse(text,out);
    }

    U8String MakeAssetIDTarget(const AssetID &id)
    {
        const AnsiString hex = id.ToString();

        U8String result(u8"id:");

        result += U8String(reinterpret_cast<const u8char *>(hex.c_str()),hex.Length());

        return result;
    }

    bool ExtractAssetIDFromHeader(const void *buffer, size_t size, AssetID &out)
    {
        out.Clear();
        if(!buffer || size < 36)
            return false;

        const char *p = static_cast<const char *>(buffer);
        const char *end = p + size;

        while(p < end)
        {
            while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                ++p;

            if(p >= end)
                break;

            if(*p == '#')
            {
                while(p < end && *p != '\n')
                    ++p;
                continue;
            }

            if(*p == '[')
                break;

            if((p + 2 < end) && p[0] == 'i' && p[1] == 'd' &&
               (p[2] == ' ' || p[2] == '\t' || p[2] == '='))
            {
                p += 2;
                while(p < end && (*p == ' ' || *p == '\t'))
                    ++p;

                if(p < end && *p == '=')
                {
                    ++p;
                    while(p < end && (*p == ' ' || *p == '\t'))
                        ++p;

                    if(p < end && (*p == '"' || *p == '\''))
                    {
                        const char quote = *p++;
                        const char *val_start = p;
                        while(p < end && *p != quote && *p != '\r' && *p != '\n')
                            ++p;

                        if(p < end && *p == quote)
                        {
                            const size_t val_len = p - val_start;
                            if(val_len == AssetID::kTextLength || val_len == AssetID::kUUIDTextLength)
                            {
                                char id_buf[AssetID::kUUIDTextLength + 1];
                                for(size_t i = 0; i < val_len; ++i)
                                    id_buf[i] = val_start[i];
                                id_buf[val_len] = '\0';

                                if(AssetID::Parse(id_buf, out))
                                    return true;
                            }
                        }
                    }
                }
            }

            while(p < end && *p != '\n')
                ++p;
        }

        return false;
    }

    bool ExtractAssetIDFromFile(const OSString &filename, AssetID &out)
    {
        out.Clear();
        if(filename.IsEmpty() || !filesystem::FileExist(filename))
            return false;

        io::OpenFileInputStream opener(filename);
        if(!opener)
            return false;

        char buf[512];
        const int64 read_bytes = opener->Read(buf, sizeof(buf));
        if(read_bytes <= 0)
            return false;

        return ExtractAssetIDFromHeader(buf, static_cast<size_t>(read_bytes), out);
    }

    bool InjectAssetIDToSourceFile(const OSString &filename, const AssetID &id)
    {
        if(filename.IsEmpty() || id.IsZero())
            return false;

        AssetID existing_id;
        if(ExtractAssetIDFromFile(filename, existing_id))
            return true;

        int64 size = 0;
        void *data = filesystem::LoadFileToMemory(filename, size, true);
        if(!data && size < 0)
            return false;

        AnsiString id_line = "id = \"";
        id_line += id.ToString();
        id_line += "\"\n";

        AnsiString new_content = id_line;
        if(data && size > 0)
        {
            new_content += AnsiString(static_cast<const char *>(data), static_cast<int>(size));
            delete[] static_cast<char *>(data);
        }

        const OSString tmp_name = filename + OS_TEXT(".tmp");
        if(filesystem::SaveMemoryToFile(tmp_name, new_content.c_str(), new_content.Length()) <= 0)
            return false;

        filesystem::FileDelete(filename);
        return filesystem::FileRename(filename, tmp_name);
    }
}//namespace hgl::asset
