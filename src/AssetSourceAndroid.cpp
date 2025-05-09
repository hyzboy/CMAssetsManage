#include<hgl/assets/AssetsSource.h>

#include<sys/stat.h>
#include<sys/types.h>
#include<unistd.h>

namespace hgl
{
    namespace assets
    {
        class AndroidFileInputStream:public InputStream
        {
            AAsset *asset;

        public:

            AndroidFileInputStream(AAsset *aa)
            {
                asset=aa;
            }

            ~AndroidFileInputStream()
            {
                Close();
            }

            void    Close()
            {
                if(!asset)return;

                AAsset_close(asset);
                asset=nullptr;
            }

            int64   Read(void *ptr,int64 size)
            {
                if(!asset)return(-1);

                return AAsset_read(asset,ptr,size);
            }

            int64   Peek(void *ptr,int64 size)
            {
                int result=Read(ptr,size);

                if(result>0)
                    AAsset_seek(asset,-result,0);

                return(result);
            }

            int64   ReadFully(void *buf,int64 buf_size){return Read(buf,buf_size);}         ///<充分读取,保证读取到指定长度的数据(不计算超时)

            bool    CanRestart()const{return true;}
            bool    CanSeek()const{return true;}
            bool    CanSize()const{return true;}
            bool    CanPeek()const{return true;}

            bool    Restart()
            {
                if(!asset)return(false);

                return AAsset_seek(asset,0,SEEK_SET)
            }

            int64   Skip(int64 bytes)
            {
                return Seek(bytes,soCurrent);
            }

            int64   Seek(int64 offset,SeekOrigin origin)
            {
                if(!asset)return(-1);

                return AAsset_seek64(asset,offset,origin)
            }

            int64   Tell()const
            {
                if(!asset)return(-1);

                return(AAsset_getLength(asset)-AAsset_getRemainingLength(asset));
            }

            int64   GetSize()const
            {
                if(!asset)return(-1);

                return AAsset_getLength(asset);
            }

            int64   Available()const
            {
                if(!asset)return(-1);

                return AAsset_getRemainingLength(asset);
            }

        };//class AndroidFileInputStream:public InputStream

        class AssetsSourceFilesytem:public AssetsSource
        {
            OSString root_path;
            bool only_read;

        public:

            AssetsSourceFilesytem(const UTF8String &sn,const OSString &path,const bool or):AssetsSource(sn)
            {
                root_path=path;
                only_read=or;
            }

            bool hasNameAccess()const override{return true;}

            io::InputStream *OpenByName(const UTF8String &filename)
            {
                const OSString &fullname=filesystem::MergeFilename(root_path,ToOSString(filename));

                AAsset *aa=AAssetManager_open(__assetManager,path.c_str(),AASSET_MODE_RANDOM);

                if(!aa)
                    return(nullptr);

                return(new AndroidFileInputStream(aa));
            }
        };//class AssetsSourceFilesytem:public AssetsSource

        AssetsSource *CreateByFilesystem(const OSString &path,const bool only_read)
        {
            if(path.IsEmpty())
                return(nullptr);

            return(new AssetsSourceFilesytem(path,only_read));
        }
    }//namespace assets
}//namespace hgl
