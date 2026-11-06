#include<hgl/asset/AssetDependencyGraph.h>

namespace hgl::asset
{
    namespace
    {
        /// 在 `list` 里找 `id`；找不到返回 -1
        int IndexOf(const ValueArray<AssetID> &list,const AssetID &id)
        {
            const int count=list.GetCount();
            const AssetID *data=list.GetData();

            for(int i=0;i<count;i++)
                if(data[i]==id)
                    return i;

            return -1;
        }

        /// 从 `list` 里删掉 `id`；返回是否真的删掉了
        bool RemoveFrom(ValueArray<AssetID> &list,const AssetID &id)
        {
            const int index=IndexOf(list,id);

            if(index<0)
                return false;

            return list.Delete(index,1);
        }

        /// 空列表就整条删掉，避免图里堆积大量「有键、值为空」的节点。
        void DropIfEmpty(UnorderedMap<AssetID,ValueArray<AssetID>> &map,const AssetID &key)
        {
            const ValueArray<AssetID> *value=map.GetValuePointer(key);

            if(value&&value->GetCount()==0)
                map.DeleteByKey(key);
        }
    }//namespace

    // ── 写入 ────────────────────────────────────────────────────────────────

    bool AssetDependencyGraph::RegisterDependencies(const AssetID &asset,
                                                    const ValueArray<AssetID> &dependencies)
    {
        if(asset.IsZero())
            return false;

        // 先去重、去自身，得到「这一版真实的直接依赖集合」。
        ValueArray<AssetID> cleaned;

        const int count=dependencies.GetCount();
        const AssetID *data=dependencies.GetData();

        for(int i=0;i<count;i++)
        {
            const AssetID &dep=data[i];

            if(dep.IsZero()||dep==asset)
                continue;

            if(IndexOf(cleaned,dep)>=0)
                continue;

            cleaned.Add(dep);
        }

        WriteLockGuard guard(lock);

        // ── 1. 拆掉旧的正向边，并从每个旧依赖的反向表里删除自己 ──────────
        const ValueArray<AssetID> *old=forward.GetValuePointer(asset);

        if(old)
        {
            for(int i=0;i<old->GetCount();i++)
            {
                const AssetID old_dep=(*old)[i];

                ValueArray<AssetID> *rev=reverse.GetValuePointer(old_dep);

                if(!rev)
                    continue;

                RemoveFrom(*rev,asset);
                DropIfEmpty(reverse,old_dep);
            }
        }

        // ── 2. 写入新的正向边 ─────────────────────────────────────────────
        if(cleaned.GetCount()==0)
        {
            forward.DeleteByKey(asset);
            return true;
        }

        if(!forward.Update(asset,cleaned))
            forward.Add(asset,cleaned);

        // ── 3. 补齐新的反向边 ─────────────────────────────────────────────
        for(int i=0;i<cleaned.GetCount();i++)
        {
            const AssetID &dep=cleaned[i];

            ValueArray<AssetID> *rev=reverse.GetValuePointer(dep);

            if(rev)
            {
                if(IndexOf(*rev,asset)<0)
                    rev->Add(asset);
            }
            else
            {
                ValueArray<AssetID> new_list;

                new_list.Add(asset);
                reverse.Add(dep,new_list);
            }
        }

        return true;
    }

    bool AssetDependencyGraph::UnregisterAsset(const AssetID &asset)
    {
        if(asset.IsZero())
            return false;

        WriteLockGuard guard(lock);

        bool existed=false;

        // ── 1. 正向边：从每个依赖的反向表里删掉自己 ──────────────────────
        const ValueArray<AssetID> *deps=forward.GetValuePointer(asset);

        if(deps)
        {
            existed=true;

            for(int i=0;i<deps->GetCount();i++)
            {
                const AssetID dep=(*deps)[i];

                ValueArray<AssetID> *rev=reverse.GetValuePointer(dep);

                if(!rev)
                    continue;

                RemoveFrom(*rev,asset);
                DropIfEmpty(reverse,dep);
            }

            forward.DeleteByKey(asset);
        }

        // ── 2. 反向边：从每个引用方的正向表里删掉自己 ────────────────────
        const ValueArray<AssetID> *referrers=reverse.GetValuePointer(asset);

        if(referrers)
        {
            existed=true;

            // 引用方列表在循环中会被修改（逐个删除），因此先拷一份再遍历。
            const ValueArray<AssetID> snapshot=*referrers;

            for(int i=0;i<snapshot.GetCount();i++)
            {
                const AssetID referrer=snapshot[i];

                ValueArray<AssetID> *fwd=forward.GetValuePointer(referrer);

                if(!fwd)
                    continue;

                RemoveFrom(*fwd,asset);
                DropIfEmpty(forward,referrer);
            }

            reverse.DeleteByKey(asset);
        }

        return existed;
    }

    void AssetDependencyGraph::Clear()
    {
        WriteLockGuard guard(lock);

        forward.ClearAndDeallocate();
        reverse.ClearAndDeallocate();
    }

    // ── 查询 ────────────────────────────────────────────────────────────────

    int AssetDependencyGraph::GetRegisteredCount() const
    {
        ReadLockGuard guard(lock);

        return forward.GetCount();
    }

    int AssetDependencyGraph::GetNodeCount() const
    {
        ReadLockGuard guard(lock);

        // 正向 ∪ 反向。反向里可能包含从未注册依赖的节点（只被引用），
        // 所以不能只看 forward。
        UnorderedMap<AssetID,uint8> nodes;

        for(const auto &item : forward)
            if(!nodes.ContainsKey(item.first))
                nodes.Add(item.first,0);

        for(const auto &item : reverse)
            if(!nodes.ContainsKey(item.first))
                nodes.Add(item.first,0);

        return nodes.GetCount();
    }

    bool AssetDependencyGraph::GetDependencies(const AssetID &asset,ValueArray<AssetID> &out) const
    {
        out.Clear();

        if(asset.IsZero())
            return false;

        ReadLockGuard guard(lock);

        const ValueArray<AssetID> *deps=forward.GetValuePointer(asset);

        if(!deps)
            return false;

        out=*deps;
        return true;
    }

    bool AssetDependencyGraph::GetDependents(const AssetID &asset,ValueArray<AssetID> &out) const
    {
        out.Clear();

        if(asset.IsZero())
            return false;

        ReadLockGuard guard(lock);

        const ValueArray<AssetID> *rev=reverse.GetValuePointer(asset);

        if(!rev)
            return false;

        out=*rev;
        return true;
    }

    int AssetDependencyGraph::GetDependentCount(const AssetID &asset) const
    {
        if(asset.IsZero())
            return 0;

        ReadLockGuard guard(lock);

        const ValueArray<AssetID> *rev=reverse.GetValuePointer(asset);

        return rev?rev->GetCount():0;
    }

    int AssetDependencyGraph::GetDependencyCount(const AssetID &asset) const
    {
        if(asset.IsZero())
            return 0;

        ReadLockGuard guard(lock);

        const ValueArray<AssetID> *deps=forward.GetValuePointer(asset);

        return deps?deps->GetCount():0;
    }

    bool AssetDependencyGraph::HasDependents(const AssetID &asset) const
    {
        return GetDependentCount(asset)>0;
    }

    // ── 传递闭包 ────────────────────────────────────────────────────────────

    namespace
    {
        /**
         * 广度优先展开闭包。
         *
         * 不用递归：图的深度由资产结构决定（场景 → 材质 → 贴图通常只有 3~5 层），
         * 但数据损坏造出的长链会把递归栈打穿。BFS 的「已访问」集合天然做了
         * 环保护：绕回来的节点不会再次入队。
         */
        int CollectClosure(const UnorderedMap<AssetID,ValueArray<AssetID>> &graph,
                           const AssetID &start,
                           ValueArray<AssetID> &out,
                           const bool recursive,
                           const int max_depth)
        {
            UnorderedMap<AssetID,uint8> visited;

            visited.Add(start,0);

            ValueArray<AssetID> current;

            {
                const ValueArray<AssetID> *first=graph.GetValuePointer(start);

                if(!first)
                    return 0;

                current=*first;
            }

            int added   =0;
            int depth   =1;

            while(current.GetCount()>0)
            {
                ValueArray<AssetID> next;

                for(int i=0;i<current.GetCount();i++)
                {
                    const AssetID id=current[i];

                    if(visited.ContainsKey(id))
                        continue;

                    visited.Add(id,0);

                    // 结果是「除起点外的闭包」，因此起点再被绕回来时不会重复计入。
                    out.Add(id);
                    ++added;

                    if(!recursive)
                        continue;

                    const ValueArray<AssetID> *sub=graph.GetValuePointer(id);

                    if(!sub)
                        continue;

                    for(int k=0;k<sub->GetCount();k++)
                        if(!visited.ContainsKey((*sub)[k]))
                            next.Add((*sub)[k]);
                }

                if(!recursive)
                    break;

                if(max_depth>0&&depth>=max_depth)
                    break;

                ++depth;

                current=next;
            }

            return added;
        }
    }//namespace

    int AssetDependencyGraph::CollectDependencies(const AssetID &asset,ValueArray<AssetID> &out,
                                                  const bool recursive,const int max_depth) const
    {
        if(asset.IsZero())
            return 0;

        ReadLockGuard guard(lock);

        return CollectClosure(forward,asset,out,recursive,max_depth);
    }

    int AssetDependencyGraph::CollectDependents(const AssetID &asset,ValueArray<AssetID> &out,
                                                const bool recursive,const int max_depth) const
    {
        if(asset.IsZero())
            return 0;

        ReadLockGuard guard(lock);

        return CollectClosure(reverse,asset,out,recursive,max_depth);
    }

    // ── 拓扑排序 ────────────────────────────────────────────────────────────

    bool AssetDependencyGraph::TopologicalOrder(ValueArray<AssetID> &out) const
    {
        out.Clear();

        ReadLockGuard guard(lock);

        // ── 1. 收集节点集合 ────────────────────────────────────────────────
        UnorderedMap<AssetID,uint32> pending;   ///< 尚未满足的依赖数（入度）

        for(const auto &item : forward)
            if(!pending.ContainsKey(item.first))
                pending.Add(item.first,0);

        for(const auto &item : reverse)
            if(!pending.ContainsKey(item.first))
                pending.Add(item.first,0);

        if(pending.GetCount()==0)
            return true;

        // ── 2. 计算入度：每个资产「有多少个已知的依赖尚未就位」 ────────────
        for(const auto &item : forward)
        {
            uint32 *degree=pending.GetValuePointer(item.first);

            if(!degree)
                continue;

            uint32 count=0;

            for(int i=0;i<item.second.GetCount();i++)
                if(pending.ContainsKey(item.second[i]))
                    ++count;

            *degree=count;
        }

        // ── 3. 入度为 0 的先出队 ──────────────────────────────────────────
        ValueArray<AssetID> ready;

        for(const auto &item : pending)
            if(item.second==0)
                ready.Add(item.first);

        // ── 4. Kahn 主循环 ────────────────────────────────────────────────
        //
        // 出队一个资产时，所有「依赖它」的资产各少一个待满足项。
        // 反向表的用途正是在这里：正向表只能回答「我依赖谁」，
        // 而推进拓扑序需要「谁在等我」。
        int head=0;

        while(head<ready.GetCount())
        {
            const AssetID id=ready[head++];

            out.Add(id);

            const ValueArray<AssetID> *dependents=reverse.GetValuePointer(id);

            if(!dependents)
                continue;

            for(int i=0;i<dependents->GetCount();i++)
            {
                uint32 *degree=pending.GetValuePointer((*dependents)[i]);

                if(!degree||*degree==0)
                    continue;

                --(*degree);

                if(*degree==0)
                    ready.Add((*dependents)[i]);
            }
        }

        // ── 5. 没排完 ⇒ 存在环 ────────────────────────────────────────────
        return out.GetCount()==pending.GetCount();
    }

    bool AssetDependencyGraph::IsAcyclic() const
    {
        ValueArray<AssetID> order;

        return TopologicalOrder(order);
    }
}//namespace hgl::asset
