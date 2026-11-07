#pragma once

/**
 * 资产 URI 字节源：把 AssetURI（含 pack:// 包内资产）适配成 IO worker
 * 可直接消费的 AsyncFileByteSource。
 *
 * ── 为什么需要这一层 ──────────────────────────────────────────────────────
 * AsyncFileLoader 原本只认「一个 OS 文件路径」。包裹（pack）资产、
 * BAKE 后的 .fb 条目等并不是裸文件，读取必须经 OpenAssetStream 的
 * 分派规则（挂载表、索引寻址、回落策略）。本类把这套规则藏在一个
 * 字节源对象里：提交方给出路由用的真实路径（通常是所属 .pack 或
 * 宿主文件），worker 只按 GetSize/Read 顺序取字节。
 *
 * ── 线程约定 ──────────────────────────────────────────────────────────────
 * GetSize 首次调用时惰性打开流；GetSize/Read 只会在处理该请求的
 * 单个 IO worker 线程上被调用，流本身无需加锁。对象由提交方创建，
 * 生命周期必须覆盖到请求完成回调返回（LoadedFileManager 让 ReadGroup
 * 持有并负责删除）。
 */

#include<hgl/asset/AssetURI.h>
#include<hgl/io/AsyncFileLoader.h>
#include<hgl/type/String.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace asset
    {
        /**
         * @brief 按 AssetURI 取字节的 AsyncFileByteSource 实现。
         */
        class AssetURIByteSource:public io::AsyncFileByteSource
        {
            OSString route_path;                  ///< 物理盘路由用的真实路径（如所属 .pack）
            AssetURI uri;                         ///< 资产 URI 值拷贝，本对象持有
            io::InputStream *stream=nullptr;      ///< 惰性打开的资产流，本对象持有并删除

        public:

            /**
             * @param route 路由路径（写入 GetRoutePath，用于物理盘选择）
             * @param uri   资产 URI；对象保存其值拷贝
             */
            AssetURIByteSource(const OSString &route,const AssetURI &asset_uri);
            ~AssetURIByteSource() override;

            const OSString &GetRoutePath()const override { return route_path; }

            /** @brief 惰性打开流并返回长度；打开失败或长度不可用返回 -1。 */
            int64 GetSize() override;

            /** @brief 从流读取至多 size 字节；<=0 视为失败。仅 worker 线程调用。 */
            int64 Read(uint8 *buffer,int64 size) override;
        };
    }//namespace asset
}//namespace hgl
