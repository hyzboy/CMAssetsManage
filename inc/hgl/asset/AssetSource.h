#pragma once

#include<hgl/type/String.h>
#include<hgl/type/ManagedArray.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace asset
    {
        /**
         * 资产索引方法
         */
        enum class AssetIndexType
        {
            Unknow=0,                   ///<未知类型
            ID,                         ///<ID
            Name,                       ///<字符串名称

            ENUM_CLASS_RANGE(ID,Name)
        };//enum class AssetIndexType

        /**
         * 包裹内一项的元信息。
         *
         * 目录后端与打包文件后端都必须能填出这个结构 —— 这是「把目录包裹换成真实
         * 包裹文件时上层零改动」的前提。缺失任何一项（尤其是 `mtime` 与 `is_directory`）
         * 都会让调用方重新退回去直接摸 filesystem。
         */
        struct SourceEntry
        {
            U8String    name;                       ///<单段名称（不含路径）
            U8String    path;                       ///<包裹内相对路径（'/' 分隔）
            uint64      size        = 0;            ///<字节数；目录为 0
            uint64      mtime       = 0;            ///<最后修改时间；后端不支持时为 0
            bool        is_directory = false;       ///<是否目录
        };//struct SourceEntry

        /**
         * 规范化包裹内路径。
         *
         * 规则：
         *   · `\` 与 `/` 一律视为分隔符，输出统一用 `/`；
         *   · 合并连续分隔符，忽略 `.` 段；
         *   · **拒绝** `..` 段（不允许越出挂载根）；
         *   · **拒绝**以 `/` / `\` 开头的绝对路径与 `C:` 形态的盘符路径；
         *   · 拒绝空路径与只由分隔符组成的路径。
         *
         * 刻意**不**做大小写折叠：目录后端在 Windows 上需要折叠，打包文件后端则
         * 取决于包内索引的构建方式，这属于后端自己的查表键策略（目录后端实现里
         * 复用 `MakeLoadedFileKey` 的口径）。
         *
         * @param in  原始包裹内路径（UTF-8）
         * @param out 规范化结果；失败时被清空
         * @return 规范化成功返回 true
         */
        bool NormalizePackPath(const U8String &in,U8String &out);

        /**
         * 资产来源
         *
         * ## 与 `AssetURI` 的分工
         *
         * `AssetSource` 是**可插拔的 IO 后端**：给定短名 `sn`（如 `u8"res"`）与一个根，
         * 回答「字节流从哪来、能不能匿名/按名/按 ID/按位置取」。
         * `AssetURI`（`<hgl/asset/AssetURI.h>`）是**定位语法**：把
         * `<type>:pack(<name>):<target>` 拆成 `type` / `pack_name` / `target` 三段并归类。
         *
         * 二者是对偶而非重叠：`AssetURI::pack_name` 只是一段短名字符串，`AssetSource`
         * 是这个短名背后的实现。解析 URI 一律走 `AssetURI::Parse`，**不要在这里重复解析**。
         *
         * ## 位置透明：目录 / 打包文件只换实现类
         *
         * `CreateSourceByFilesystem` 产出的是「目录包裹」；将来产出「真实包裹文件」
         * 的后端只需实现同一组虚函数。因此 **loader 不得直接调用
         * `filesystem::FileExist` / `FileCanRead`**，一律走
         * `<hgl/asset/AssetStream.h>` 的 `OpenAssetStream()`，否则切换后端时会漏掉
         * 一批绕过包裹的读盘路径。
         *
         * ## 线程约定
         *
         * 挂载表（`RegisterAssetsSource` / `GetSource`）受读写锁保护，可在任意线程
         * 查找；但 `AssetSource` 实例本身的方法是否线程安全由各后端自行声明。
         */
        class AssetSource
        {
            AnsiString uri_short_name;

        public:

            /// 挂载名（`pack(<name>)` 括号里的那段）。限定 ASCII 标识符，
            /// 因此用 `AnsiString` 而非 `U8String` —— 与 `AssetURI::pack_name` 同型。
            const AnsiString &GetName()const{return uri_short_name;}

        public:

            explicit AssetSource(const AnsiString &);
            virtual ~AssetSource();

            virtual bool hasAnonymousAccess ()const{return false;}
            virtual bool hasNameAccess      ()const{return false;}
            virtual bool hasIDAccess        ()const{return false;}
            virtual bool hasPositionAccess  ()const{return false;}

        public:     // 能力契约：目录后端与打包文件后端必须给出同样的语义

            /// 包裹内是否存在该路径。默认返回 false（后端未实现该能力）。
            virtual bool Exists(const U8String &path)const;

            /// 取一项的元信息。默认返回 false。
            virtual bool Stat(const U8String &path,SourceEntry &out)const;

            /**
             * 打开包裹内文件的只读字节流。
             *
             * @return 调用方负责 delete；失败返回 nullptr
             */
            virtual io::InputStream *Open(const U8String &path);

            /**
             * 列出目录下的直接子项（不递归）。
             *
             * @param path 包裹内目录路径；空串表示挂载根
             * @param out  追加写入（不清空调用方已有内容）
             * @return 新增的项数；后端不支持列举时返回 -1
             *
             * **打包文件后端必须实现**：包内索引表要能回答「这个目录下有什么」，
             * 否则将来从目录切到包文件时，任何依赖枚举的调用点都会失效。
             */
            virtual int ListDirectory(const U8String &path,ManagedArray<SourceEntry> &out)const;

        public:

            /// 由当前来源派生子来源（例如把包内的某个子目录单独挂载）。默认不支持。
            virtual AssetSource *       CreateSubSource (const U8String &){return nullptr;}
        };//class AssetSource

        /**
         * 登记来源。`src` 的 `GetName()` 必须等于 `name`，且同名不能重复登记。
         * 登记表接管 `src` 的生命周期（注销时 delete）。
         */
        bool            RegisterAssetsSource   (const AnsiString &name,AssetSource *src);

        /// 注销并 delete 该来源。找不到返回 false。
        bool            UnregisterAssetsSource (const AnsiString &name);

        /// 按挂载名查找（线程安全）。未挂载返回 nullptr。
        AssetSource *   GetSource              (const AnsiString &uri_short_name);

        /// 当前已挂载的来源数（线程安全）。
        int             GetSourceCount         ();

        /// 清空登记表并 delete 全部来源（主要给测试用）。
        void            ClearAssetsSource      ();

        /**
         * 创建一个「目录包裹」并登记为 `sn`。
         *
         * @param sn         挂载名（对应 `pack(<name>)`）
         * @param pathname   真实目录；必须是已存在的目录
         * @param only_read  是否只读（当前仅作为元信息保留，写入能力尚未开放）
         * @return 成功返回已登记的来源；`sn` 已存在或路径无效时返回 nullptr
         */
        AssetSource *CreateSourceByFilesystem(const AnsiString &sn,const OSString &pathname,const bool only_read);
    }//namespace asset
}//namespace hgl
