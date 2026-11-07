#include<hgl/asset/LoadedFileManager.h>
#include<hgl/filesystem/Filename.h>
#include<hgl/filesystem/FileSystem.h>

namespace hgl
{
    OSString MakeLoadedFileKey(const OSString &filename)
    {
        OSString key=filesystem::NormalizeFilename(filename);

#if HGL_OS == HGL_OS_Windows
        key.LowerCase();                            // Windows 文件系统大小写无关，查表键也折叠大小写
#endif

        return key;
    }

    OSString MakeLoadedFileAbsKey(const OSString &path)
    {
        if(path.IsEmpty())
            return path;

        OSString norm=filesystem::NormalizeFilename(path);

        // 盘符（"x:"）或前导分隔符视为绝对路径；不做磁盘访问
        const bool is_abs = (norm.Length()>=2 && norm[1]==OS_TEXT(':'))
                         || norm[0]==OS_TEXT('/')
                         || norm[0]==OS_TEXT('\\');

        if(!is_abs)
        {
            OSString cwd;

            if(filesystem::GetCurrentPath(cwd) && !cwd.IsEmpty())
                norm=filesystem::JoinPathWithFilename(cwd,norm);
            // 取不到工作目录就退回规范化后的原样（仍然比原串稳定）
        }

        return MakeLoadedFileKey(norm);
    }

    LoadedFile::LoadedFile(const OSString &filename)
        :request(filename)
    {
        meta.source_path=filename;
        path_key=MakeLoadedFileKey(filename);
    }

    LoadedFile::LoadedFile(const OSString &filename,void *dest_buffer,int64 dest_capacity)
        :request(filename)
    {
        meta.source_path=filename;
        path_key=MakeLoadedFileKey(filename);
        request.SetDestinationBuffer(dest_buffer,dest_capacity);
    }

    bool LoadedFile::OnLoadPayload()
    {
        // 字节已由读盘线程写入 request；空文件(0 字节)同样视为成功
        return(meta.source_size==0||request.GetData()!=nullptr);
    }

    void LoadedFile::OnUnloadPayload()
    {
        request.ReleaseData();
    }

    void LoadedFile::OnRefZero()
    {
        if(owner)
            owner->OnFileRefZero(this);         // 只入队，真正的 Release/delete 由 Collect() 完成
        else
        {
            Release();                          // 管理器已不存在：按 AutoDestroy 的老规矩自行了断
            delete this;
        }
    }
}//namespace hgl