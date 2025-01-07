﻿#include<hgl/assets/AssetsSource.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/io/FileInputStream.h>
#include<hgl/CodePage.h>

namespace hgl
{
    namespace assets
    {
        class AssetsSourceFilesytem:public AssetsSource
        {
            OSString root_path;
            bool only_read;

        public:

            AssetsSourceFilesytem(const U8String &sn,const OSString &path,const bool _or):AssetsSource(sn)
            {
                root_path=path;
                only_read=_or;
            }

            bool hasNameAccess()const override{return true;}

            io::InputStream *Open(const U8String &filename)
            {
                const OSString &fullname=filesystem::MergeFilename(root_path,ToOSString(filename));

                if(!filesystem::FileCanRead(fullname))
                    return(nullptr);

                io::FileInputStream *fis=new io::FileInputStream;

                if(fis->Open(fullname))
                    return fis;

                delete fis;
                return(nullptr);
            }
        };//class AssetsSourceFilesytem:public AssetsSource

        AssetsSource *CreateSourceByFilesystem(const U8String &uri,const OSString &path,const bool only_read)
        {
            if(!uri.IsEmpty())
            {
                if(GetSource(uri))
                    return(nullptr);
            }

            if(path.IsEmpty())
                return(nullptr);

            if(!filesystem::IsDirectory(path))
                return(nullptr);

            return(new AssetsSourceFilesytem(uri,path,only_read));
        }
    }//namespace assets
}//namespace hgl
