#pragma once

#include<hgl/type/DataType.h>

#include<source_location>

namespace Asset
{
    using namespace hgl;

#pragma pack(push,1)
    union TimeID
    {
        uint64 raw_data;

        struct 
        {        
            uint year:12;               //4096
            uint month:4;
            uint day:5;
            uint hour:5;
            uint minute:6;
            uint seconds:6;
            uint micro_second:20;       //
        };
    };

    constexpr const size_t TimeIDBytes=sizeof(TimeID);

    TimeID GetCurrentTimeID();

    using AssetTypeID=uint32;
    
    enum AssetProvid:uint8
    {
        Unknow=0,

        EngineCore,         ///<引擎核心资产(一般为内置硬编码，只有修改源代码才可能变动)
        EngineTemp,         ///<引擎临时资产(一般为内置硬编码产生的可变临时资产)

        EngineOpen,         ///<引擎开放资产(如开放的Shader，图标等)
    };

    struct AssetTypeRegistryInfo
    {
        AssetTypeID             type_id;
        char                    name[128];
        std::source_location    source_location;
        TimeID                  registry_time;
    };

    uint32 RegistryTypeID(const std::source_location &,const char *);
    const char *GetTypeName(const AssetTypeID &);

    union AssetID
    {
        struct RawData
        {
            uint64 raw_data[4];
        };

        struct IDInfo
        {
            AssetTypeID type_id;        ///<32bits 资产类型
            uint32      creator;        ///<32bits 创建者ID，项目整体会记录当前开发者的 OS和OS USER。然后用这个做个列表，这里的ID就指这个列表中的序号。
                                        ///<需要注意的是这个创建者和真实创建者无关，所以即便是同一个人，只要是换了台电脑，或是更新了一下系统，这个ID都有可能会产生变化。
                                        ///<它只是用于增加AssetID多样性的一个手段。
            TimeID      time_id;        ///<64bits 时间ID
            uint32      serial;         ///<32bits 顺序序列号(注：所有资产都从1开始编号，不存在0号资产，如果出现0号表示出问题或空)
        };
    };

    constexpr const size_t AssetIDBytes=sizeof(AssetID);
#pragma pack(pop)

    uint32 GetLocalDeveloperID();       ///<取得本机开发者(资产ID中的Creator)

    class AssetIDGenerater
    {
        AssetTypeID type_id;
        uint32 developer_id;
        uint32 serial;

    public:

        AssetIDGenerater(const AssetTypeID &);
    };
}//namespace Asset
