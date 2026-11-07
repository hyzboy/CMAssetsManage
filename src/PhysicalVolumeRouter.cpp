#include<hgl/asset/PhysicalVolumeRouter.h>

#if HGL_OS == HGL_OS_Windows
#include<hgl/filesystem/EnumVolume.h>
#endif

#include<hgl/filesystem/Filename.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/log/Log.h>

DEFINE_LOGGER_MODULE(AssetIO)
USE_MODULE_LOGGER(AssetIO)

namespace hgl::asset
{
    namespace
    {
#if HGL_OS == HGL_OS_Windows
        AssetDeviceType MapDriverType(const filesystem::VolumeInfo::DriverType driver_type)
        {
            switch(driver_type)
            {
                case filesystem::VolumeInfo::DriverType::Removable: return AssetDeviceType::Removable;
                case filesystem::VolumeInfo::DriverType::Fixed:     return AssetDeviceType::Fixed;
                case filesystem::VolumeInfo::DriverType::Remote:    return AssetDeviceType::Remote;
                case filesystem::VolumeInfo::DriverType::CDROM:     return AssetDeviceType::CDROM;
                case filesystem::VolumeInfo::DriverType::RamDisk:   return AssetDeviceType::RamDisk;
                default:                                            return AssetDeviceType::Unknown;
            }
        }
#endif
    }//namespace

    OSString PhysicalVolumeRouter::NormalizeRoot(const OSString &path)
    {
        OSString result=filesystem::NormalizeFilename(path);

        const int len=result.Length();

        if(len<=0)
            return result;

        if(result.c_str()[len-1]!=HGL_DIRECTORY_SEPARATOR)
        {
            const os_char tail[2]={HGL_DIRECTORY_SEPARATOR,0};

            result+=OSString(tail);
        }

        return result;
    }

    OSString PhysicalVolumeRouter::NormalizeMatchKey(const OSString &path)
    {
        OSString result=filesystem::NormalizeFilename(path);

#if HGL_OS == HGL_OS_Windows
        // Windows 文件系统不区分大小写，匹配键必须折叠大小写，
        // 与 LoadedFileManager::MakeLoadedFileKey 的处理保持一致。
        result.LowerCase();
#endif

        return result;
    }

    PhysicalVolumeRouter::VolumeRecord *PhysicalVolumeRouter::FindRecord(const AssetDeviceID id)const
    {
        const int count=volumes.GetCount();

        for(int i=0;i<count;i++)
        {
            VolumeRecord *record=volumes.At(i);

            if(record&&record->device_id==id)
                return record;
        }

        return nullptr;
    }

    void PhysicalVolumeRouter::RebuildOrder()const
    {
        order.Clear();

        const int count=volumes.GetCount();

        for(int i=0;i<count;i++)
        {
            VolumeRecord *record=volumes.At(i);

            if(!record)
                continue;

            const int len=record->match_root.Length();

            // 插入排序：卷数量是个位数，比任何通用排序都简单可靠。
            // 先在尾部占位再向前冒泡，全程只用索引，不保留元素引用(Add 可能触发重分配)。
            int pos=order.GetCount();

            order.Add(record);

            while(pos>0&&order[pos-1]->match_root.Length()<len)
            {
                order[pos]=order[pos-1];
                --pos;
            }

            order[pos]=record;
        }

        order_dirty=false;
    }

    const PhysicalVolumeRouter::VolumeRecord *PhysicalVolumeRouter::GetMatchOrderAt(const int index)const
    {
        if(order_dirty)
            RebuildOrder();

        if(index<0||index>=order.GetCount())
            return nullptr;

        return order[index];
    }

    PhysicalVolumeRouter::VolumeRecord *PhysicalVolumeRouter::AddOrUpdate(const OSString &root,
                                                                         const AssetDeviceType type,
                                                                         const OSString &name,
                                                                         const OSString &label,
                                                                         const OSString &filesystem_name,
                                                                         const uint32 serial,
                                                                         const uint64 total_space,
                                                                         const uint64 free_space)
    {
        const OSString new_root=NormalizeRoot(root);

        if(new_root.IsEmpty())
            return nullptr;

        const OSString new_match_root=NormalizeMatchKey(new_root);

        const int count=volumes.GetCount();

        for(int i=0;i<count;i++)
        {
            VolumeRecord *record=volumes.At(i);

            if(record&&record->match_root==new_match_root)
            {
                // 同一挂载根：保留已分配的编号，只刷新易变字段。
                record->root        =new_root;
                record->type        =type;
                record->name        =name;
                record->label       =label;
                record->filesystem  =filesystem_name;
                record->serial      =serial;
                record->total_space =total_space;
                record->free_space  =free_space;

                return record;
            }
        }

        VolumeRecord *record=volumes.Create();

        if(!record)
            return nullptr;

        record->device_id   =next_device_id++;
        record->type        =type;
        record->root        =new_root;
        record->match_root  =new_match_root;
        record->name        =name;
        record->label       =label;
        record->filesystem  =filesystem_name;
        record->serial      =serial;
        record->total_space =total_space;
        record->free_space  =free_space;

        order_dirty=true;

        return record;
    }

    bool PhysicalVolumeRouter::AddVolume(const OSString &root, const AssetDeviceType type, const AssetDeviceID id)
    {
        const OSString new_match_root=NormalizeMatchKey(NormalizeRoot(root));

        if(new_match_root.IsEmpty())
            return false;

        if(id!=AssetDeviceDefault)
        {
            const VolumeRecord *exist=FindRecord(id);

            // 指定编号时必须确认该编号没有被别的挂载根占用；同一根原地更新是允许的。
            if(exist&&!(exist->match_root==new_match_root))
                return false;
        }

        VolumeRecord *record=AddOrUpdate(root,type,OSString(),OSString(),OSString(),0,0,0);

        if(!record)
            return false;

        if(id!=AssetDeviceDefault)
        {
            record->device_id=id;

            if(next_device_id<=id)
                next_device_id=id+1;
        }

        return true;
    }

    bool PhysicalVolumeRouter::RemoveVolume(const AssetDeviceID id)
    {
        const int count=volumes.GetCount();

        for(int i=0;i<count;i++)
        {
            const VolumeRecord *record=volumes.At(i);

            if(record&&record->device_id==id)
            {
                volumes.DeleteAt(i);
                order_dirty=true;

                return true;
            }
        }

        return false;
    }

    void PhysicalVolumeRouter::Clear()
    {
        volumes.Clear();
        order.Clear();

        order_dirty=true;
        next_device_id=1;
    }

    int PhysicalVolumeRouter::Enumerate()
    {
#if HGL_OS == HGL_OS_Windows
        filesystem::VolumeCheckConfig config;
        filesystem::VolumeInfoList list;

        if(filesystem::EnumVolume(list,config)<0)
        {
            FLogWarning(u8"EnumVolume failed, keeping the existing volume records.");
            return -1;
        }

        const int count=list.GetCount();

        for(int i=0;i<count;i++)
        {
            const filesystem::VolumeInfo &info=list[i];

            // 未分配盘符的卷(只挂在目录上或完全裸卷)没有可用的路径前缀，无法参与匹配。
            if(info.path[0]==0)
                continue;

            AddOrUpdate(OSString(info.path),
                        MapDriverType(info.driver_type),
                        OSString(info.name),
                        OSString(info.volume_label),
                        OSString(info.file_system),
                        info.serial,
                        info.total_space,
                        info.free_space);
        }

        return volumes.GetCount();
#else
        // 非 Windows 平台没有系统级卷枚举：把根目录当作唯一设备。
        // 之后再按需用 AddVolume 手工挂载真正的独立设备(例如 /mnt/xxx)。
        if(!AddVolume(OS_TEXT("/"),AssetDeviceType::Fixed))
            return -1;

        return volumes.GetCount();
#endif
    }

    AssetDeviceID PhysicalVolumeRouter::Resolve(const OSString &path)const
    {
        if(volumes.GetCount()<=0)
            return AssetDeviceDefault;

        const OSString key=NormalizeMatchKey(path);

        if(key.IsEmpty())
            return AssetDeviceDefault;

        if(order_dirty)
            RebuildOrder();

        const int count=order.GetCount();

        for(int i=0;i<count;i++)
        {
            const VolumeRecord *record=order[i];

            if(!record)
                continue;

            const int root_len=record->match_root.Length();

            // 路径正好等于挂载根本身(去掉尾部隔离符)也算命中；
            // 否则只登记了 D:\Games 时 Resolve("D:\Games") 会漏到默认通道。
            if(key.Length()==root_len-1&&key.Comp(record->match_root.c_str(),root_len-1)==0)
                return record->device_id;

            if(key.StartsWith(record->match_root))
                return record->device_id;
        }

        return AssetDeviceDefault;
    }
}//namespace hgl::asset
