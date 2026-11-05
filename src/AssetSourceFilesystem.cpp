#include<hgl/asset/AssetSource.h>
#include<hgl/filesystem/EnumFile.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/io/FileInputStream.h>
#include<hgl/platform/Platform.h>
#include<hgl/Charset.h>

namespace hgl::asset
{
    namespace
    {
        /**
         * 目录枚举器。
         *
         * 不使用 `filesystem::GetFileInfoList`：它的实现只转发了 `ProcFile`，
         * 目录项会被静默丢掉，而包裹的 `ListDirectory` 必须同时给出子目录。
         *
         * 平台差异：Windows 的枚举器对每个目录项都会调用 `ProcFolderEnd`
         * （即使不递归），UNIX 版本走的是 `ProcFolder` 回调（当前 `EnumFile`
         * 并未声明该虚函数，属于 CMCore 侧待修的问题）。因此这里按平台选择
         * 可用的那个回调。
         */
        class SourceEnumerator:public filesystem::EnumFile
        {
            ManagedArray<SourceEntry> *out_list;
            const U8String            &base_path;

        private:

            void Append(const filesystem::FileInfo &fi,bool is_directory)
            {
                SourceEntry entry;

                entry.name        =ToU8String(OSString(fi.name));
                entry.path        =base_path.IsEmpty()?entry.name:(base_path+U8String(u8"/")+entry.name);
                entry.size        =is_directory?0:fi.size;
                entry.mtime       =fi.mtime;
                entry.is_directory=is_directory;

                out_list->Create(entry);
            }

        protected:

#if HGL_OS == HGL_OS_Windows
            void ProcFolderEnd(struct filesystem::EnumFileConfig *,
                               struct filesystem::EnumFileConfig *,
                               filesystem::FileInfo &fi) override
            {
                Append(fi,true);
            }
#else
            void ProcFolderBegin(struct filesystem::EnumFileConfig *,
                                 struct filesystem::EnumFileConfig *,
                                 filesystem::FileInfo &fi) override
            {
                Append(fi,true);
            }
#endif//HGL_OS == HGL_OS_Windows

            void ProcFile(struct filesystem::EnumFileConfig *,filesystem::FileInfo &fi) override
            {
                Append(fi,false);
            }

        public:

            SourceEnumerator(ManagedArray<SourceEntry> *list,const U8String &base)
                :out_list(list),base_path(base) {}
        };//class SourceEnumerator
    }//namespace

    /**
     * 目录包裹：把挂载名映射到一个真实目录。
     *
     * 这是「位置透明」的当前实现；将来换成真实包裹文件（zip / pak）时，
     * 只需另写一个实现同一组虚函数的类，`pack(<name>)` 之下的资产路径不用改。
     * 因此这里的 Exists/Stat/ListDirectory 不是可选装饰 —— 它们是后端契约。
     */
    class AssetsSourceFilesytem:public AssetSource
    {
        OSString root_path;
        bool     only_read;

    private:

        /// 规范化后的包裹内路径 → 真实路径。`norm` 为空表示挂载根本身。
        OSString MakeFullPath(const U8String &norm)const
        {
            if(norm.IsEmpty())
                return root_path;

            return filesystem::JoinPathWithFilename(root_path,ToOSString(norm));
        }

    public:

        AssetsSourceFilesytem(const AnsiString &sn,const OSString &path,const bool _or):AssetSource(sn)
        {
            root_path=path;
            only_read=_or;
        }

        bool IsReadOnly()const{return only_read;}

        bool hasNameAccess()const override{return true;}

        bool Exists(const U8String &path)const override
        {
            SourceEntry entry;

            return Stat(path,entry);
        }

        bool Stat(const U8String &path,SourceEntry &out)const override
        {
            U8String norm;

            // 空串表示挂载根；其余必须能通过规范化（拒绝越界）。
            if(!path.IsEmpty()&&!NormalizePackPath(path,norm))
                return false;

            const OSString fullname=MakeFullPath(norm);

            filesystem::FileInfo fi;

            if(!filesystem::GetFileInfo(fullname.c_str(),fi))
                return false;

            out.name        =ToU8String(OSString(fi.name));
            out.path        =norm;
            out.size        =fi.is_directory?0:fi.size;
            out.mtime       =fi.mtime;
            out.is_directory=fi.is_directory;

            return true;
        }

        io::InputStream *Open(const U8String &path) override
        {
            U8String norm;

            if(!NormalizePackPath(path,norm))
                return nullptr;

            const OSString fullname=MakeFullPath(norm);

            if(!filesystem::FileCanRead(fullname))
                return nullptr;

            io::FileInputStream *fis=new io::FileInputStream;

            if(fis->Open(fullname))
                return fis;

            delete fis;
            return nullptr;
        }

        int ListDirectory(const U8String &path,ManagedArray<SourceEntry> &out)const override
        {
            U8String norm;

            // 空串表示挂载根；其余必须能通过规范化（拒绝越界）。
            if(!path.IsEmpty()&&!NormalizePackPath(path,norm))
                return -1;

            const OSString folder=MakeFullPath(norm);

            if(!filesystem::IsDirectory(folder.c_str()))
                return -1;

            const int before=out.GetCount();

            filesystem::EnumFileConfig config(folder);

            config.proc_folder=true;
            config.proc_file  =true;
            config.sub_folder =false;

            SourceEnumerator enumerator(&out,norm);

            enumerator.Enum(&config);

            return out.GetCount()-before;
        }
    };//class AssetsSourceFilesytem:public AssetSource

    AssetSource *CreateSourceByFilesystem(const AnsiString &uri,const OSString &path,const bool only_read)
    {
        if(uri.IsEmpty())
            return nullptr;

        if(path.IsEmpty())
            return nullptr;

        if(!filesystem::IsDirectory(path.c_str()))
            return nullptr;

        if(GetSource(uri))
            return nullptr;

        AssetSource *src=new AssetsSourceFilesytem(uri,path,only_read);

        if(!RegisterAssetsSource(uri,src))
        {
            delete src;
            return nullptr;
        }

        return src;
    }
}//namespace hgl::asset
