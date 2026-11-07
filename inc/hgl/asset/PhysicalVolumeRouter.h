#pragma once

#include<hgl/platform/Platform.h>
#include<hgl/type/ManagedArray.h>
#include<hgl/type/String.h>
#include<hgl/type/ValueArray.h>

namespace hgl::asset
{
    using AssetDeviceID = uint32;

    /** 默认设备编号：未匹配到任何挂载点的路径都走这条通道。 */
    inline constexpr AssetDeviceID AssetDeviceDefault = 0;

    /**
     * 挂载点的设备类型，与平台无关。
     *
     * 为什么要自建这个枚举：CMCore 的 filesystem::VolumeInfo::DriverType 只存在于
     * Windows 实现(EnumVolume.h 整个被 #if HGL_OS==HGL_OS_Windows 包住)，
     * 直接把它写进本头会让整个资产库在非 Windows 上编不过。
     */
    enum class AssetDeviceType : uint8
    {
        Unknown=0,      ///<未知类型
        Removable,      ///<可移动设备(U盘/移动硬盘)
        Fixed,          ///<固定磁盘
        Remote,         ///<网络/远程设备
        CDROM,          ///<光盘驱动器
        RamDisk,        ///<内存虚拟磁盘
        Mounted,        ///<手工挂载点(包、虚拟目录)
    };

    /**
     * 物理卷路由器：把文件路径映射到唯一物理设备编号。
     *
     * 设计目标只有一个 —— 让上层能按"物理设备"而不是"文件"来做 IO 并发控制。
     * 同一块磁盘上的并发读只会互相抢磁头/通道，串行反而更快；不同设备之间
     * 却应该并行。因此这里只回答一个问题：这个路径落在哪台设备上。
     *
     * 匹配规则：最长前缀匹配。挂载根在登记时就被归一化并补齐尾部分隔符，
     * 所以 "D:\Games" 不会误匹配 "D:\Games2\x"（尾部隔离符强制了路径分量边界）。
     * Windows 下比较前会折叠大小写，与文件系统的行为一致。
     *
     * 编号稳定性：同一挂载根重复 Enumerate() 时沿用原编号，只刷新容量等易变字段。
     * 热插拔后调用 Enumerate() 即可，在途请求的路由结果不受影响。
     *
     * 本类不做任何 IO，也不持有文件句柄；纯数据结构。
     */
    class PhysicalVolumeRouter
    {
    public:

        /** 一个已登记的挂载点。 */
        struct VolumeRecord
        {
            AssetDeviceID       device_id   =AssetDeviceDefault;            ///<设备编号
            AssetDeviceType     type        =AssetDeviceType::Unknown;      ///<设备类型
            OSString            root;                                       ///<归一化挂载根，必以分隔符结尾
            OSString            match_root;                                 ///<用于前缀比较的键(Windows 下折叠大小写)
            OSString            name;                                       ///<平台卷名(如 \\?\Volume{...}\)
            OSString            label;                                      ///<卷标
            OSString            filesystem;                                 ///<文件系统名称
            uint32              serial      =0;                             ///<卷序列号
            uint64              total_space =0;                             ///<总容量(字节)
            uint64              free_space  =0;                             ///<自由容量(字节)
        };

    private:

        ManagedArray<VolumeRecord>  volumes;            ///<拥有全部登记记录
        mutable ValueArray<VolumeRecord *> order;       ///<按 match_root 长度降序的匹配顺序(延迟构建)
        mutable bool                order_dirty=true;   ///<order 需要重建
        AssetDeviceID               next_device_id=1;   ///<下一个可用编号(0 保留给默认通道)

    private:

        void RebuildOrder()const;
        VolumeRecord *FindRecord(AssetDeviceID id)const;
        VolumeRecord *AddOrUpdate(const OSString &root,
                                  AssetDeviceType type,
                                  const OSString &name,
                                  const OSString &label,
                                  const OSString &filesystem_name,
                                  uint32 serial,
                                  uint64 total_space,
                                  uint64 free_space);

    public:

        PhysicalVolumeRouter()=default;
        ~PhysicalVolumeRouter()=default;

        NO_COPY_NO_MOVE(PhysicalVolumeRouter)

        /**
         * 枚举当前机器上的全部挂载点并登记，热插拔后再次调用即可。
         *
         * @return 登记后的卷总数；负数表示枚举失败(已有登记不受影响)。
         * @note 非 Windows 平台没有系统级卷枚举，等价于把根目录 "/" 登记为唯一设备。
         */
        int Enumerate();

        /**
         * 手工登记一个挂载点(包挂载、虚拟目录或测试用)。
         *
         * @param root 挂载根路径，非空；内部会归一化并补齐尾部隔离符。
         * @param type 设备类型，手工挂载点默认 Mounted。
         * @param id   指定的设备编号；0 表示自动分配，非 0 且已被占用则失败。
         * @return 成功返回 true；同一 root 重复登记会原地更新并返回 true。
         */
        bool AddVolume(const OSString &root,
                       AssetDeviceType type=AssetDeviceType::Mounted,
                       AssetDeviceID id=0);

        /** 注销一个挂载点；编号不存在返回 false。 */
        bool RemoveVolume(AssetDeviceID id);

        /** 清空全部登记并重置编号分配。 */
        void Clear();

        /** 已登记的挂载点数量。 */
        int GetVolumeCount()const{return volumes.GetCount();}

        /** 按登记索引取记录，越界返回空指针。 */
        const VolumeRecord *GetVolumeAt(int index)const{return volumes.At(index);}

        /** 按匹配优先级(最长前缀优先)取记录，越界返回空指针。 */
        const VolumeRecord *GetMatchOrderAt(int index)const;

        /** 按编号取记录，未登记返回空指针。 */
        const VolumeRecord *GetVolume(AssetDeviceID id)const{return FindRecord(id);}

        /**
         * 把路径映射到物理设备编号。
         *
         * @return 命中的设备编号；没有任何挂载点匹配时返回 AssetDeviceDefault。
         */
        AssetDeviceID Resolve(const OSString &path)const;

        /** 归一化挂载根：分隔符规范化 + 补齐尾部隔离符。 */
        static OSString NormalizeRoot(const OSString &path);

        /** 归一化路径并生成匹配键(Windows 下折叠大小写)。 */
        static OSString NormalizeMatchKey(const OSString &path);
    };
}//namespace hgl::asset
