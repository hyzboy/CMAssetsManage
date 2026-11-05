#pragma once

/**
 * 资产统一取流入口。
 *
 * ── 为什么必须有这一层 ────────────────────────────────────────────────────
 * 引入包裹（`pack(<name>)`）之后，「一个资产字节从哪里读」不再只有一个答案：
 * 可能是挂载目录、也可能是将来的打包文件。如果各 loader 继续自己调
 * `filesystem::FileCanRead` / `FileExist` / `new FileInputStream`，那么从目录
 * 切换到真实包裹文件时，凡是绕过包裹的路径都会静默失效 —— 而且很难定位。
 *
 * 因此**所有需要读取资产字节的代码都必须走这里**，而不是直接摸 filesystem。
 *
 * ── 分派规则 ──────────────────────────────────────────────────────────────
 *   · `source == Pack`      → 按 `pack_name` 查挂载表，再 `AssetSource::Open(target)`
 *   · `source == File`      → 直接当本地文件打开
 *   · `source == Unknown`   → 也按本地文件尝试（兼容裸路径与历史写法）
 *   · `builtin` / `rt`      → 不是字节流来源，返回 nullptr
 *
 * 路径规范化（拒绝 `..`、统一分隔符）在包裹分支里完成，调用方不必预先处理。
 */

#include<hgl/asset/AssetURI.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace asset
    {
        /**
         * 按 URI 打开只读字节流。
         *
         * @param uri      已经解析好的资产 URI
         * @param out_error 失败时写入原因（可为 nullptr）
         * @return 调用方负责 delete；失败返回 nullptr
         */
        io::InputStream *OpenAssetStream(const AssetURI &uri,AnsiString *out_error=nullptr);

        /**
         * 便捷重载：文本 URI 就地解析后取流。
         *
         * @param uri_text     原始 URI 文本
         * @param default_type 文本里省略类型段时补上的类型（只影响诊断，不影响取流）
         * @param out_error    失败时写入原因（可为 nullptr）
         */
        io::InputStream *OpenAssetStream(const OSString &uri_text,
                                         AssetType default_type=AssetType::Unknown,
                                         AnsiString *out_error=nullptr);
    }//namespace asset
}//namespace hgl
