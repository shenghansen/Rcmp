#include "proto/rpc_master.hpp"

#include "promise.hpp"
#include "proto/rpc_adaptor.hpp"
#include "proto/rpc_register.hpp"

namespace rpc_master {

void joinDaemon(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
                JoinDaemonRequest& req, ResponseHandle<JoinDaemonReply>& resp_handle) {
    mac_id_t mac_id = master_context.m_cluster_manager.mac_id_allocator->Gen();
    size_t old_rack_count = master_context.m_cluster_manager.cluster_rack_table.size();

    resp_handle.Init(sizeof(JoinDaemonReply::RackInfo) * old_rack_count);
    auto& reply = resp_handle.Get();

    auto local_addr = master_context.m_listen_conn.get_local_addr();

    reply.daemon_mac_id = mac_id;
    reply.master_mac_id = master_context.m_master_id;
    reply.rdma_port = local_addr.second;

    reply.other_rack_count = old_rack_count;
    master_context.m_cluster_manager.cluster_rack_table.foreach_all(
        [&, i = 0](std::pair<const rack_id_t, RackMacTable*>& p) mutable {
            MasterToDaemonConnection* conn = p.second->daemon_connect;
            auto& info = reply.other_rack_infos[i];
            auto peer_addr = conn->rdma_conn->get_peer_addr();
            info.rack_id = conn->rack_id;
            info.daemon_id = conn->daemon_id;
            info.daemon_ipv4 = conn->ip;
            info.daemon_erpc_port = conn->port;
            info.daemon_rdma_port = peer_addr.second;
            i++;
            return true;
        });

    RackMacTable* rack_table;
    auto it = master_context.m_cluster_manager.cluster_rack_table.find(req.rack_id);
    DLOG_ASSERT(it == master_context.m_cluster_manager.cluster_rack_table.end(),
                "Reconnect rack %u daemon", req.rack_id);

    rack_table = new RackMacTable();
    rack_table->with_cxl = req.with_cxl;
    rack_table->max_free_page_num = req.free_page_num;
    rack_table->current_allocated_page_num = 0;

    rack_table->daemon_connect = &daemon_connection;
    rack_table->daemon_connect->rack_id = req.rack_id;
    rack_table->daemon_connect->daemon_id = mac_id;
    rack_table->daemon_connect->ip = req.ip.get_string();
    rack_table->daemon_connect->port = req.port;

    rack_table->daemon_connect->erpc_conn = std::make_unique<ErpcClient>(
        master_context.GetErpc(), rack_table->daemon_connect->ip, rack_table->daemon_connect->port);

    master_context.m_cluster_manager.cluster_rack_table.insert(req.rack_id, rack_table);
    master_context.m_cluster_manager.connect_table.insert(daemon_connection.daemon_id,
                                                          &daemon_connection);

    master_context.m_page_directory.page_id_allocator->Expand(rack_table->max_free_page_num);

    DLOG("Connect with daemon [rack:%d --- id:%d]", daemon_connection.rack_id,
         daemon_connection.daemon_id);
}

void joinClient(MasterContext& master_context, MasterToClientConnection& client_connection,
                JoinClientRequest& req, ResponseHandle<JoinClientReply>& resp_handle) {
    RackMacTable* rack_table = master_context.m_cluster_manager.cluster_rack_table[req.rack_id];

    mac_id_t mac_id = master_context.m_cluster_manager.mac_id_allocator->Gen();

    client_connection.rack_id = req.rack_id;
    client_connection.client_id = mac_id;

    rack_table->client_connect_table.push_back(&client_connection);

    DLOG("Connect with client [rack:%d --- id:%d]", client_connection.rack_id,
         client_connection.client_id);

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.mac_id = mac_id;
}

void allocPage(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
               AllocPageRequest& req, ResponseHandle<AllocPageReply>& resp_handle) {
    RackMacTable* rack_table =
        master_context.m_cluster_manager.cluster_rack_table[daemon_connection.rack_id];

    page_id_t new_page_id = master_context.m_page_directory.page_id_allocator->MultiGen(req.count);
    DLOG_ASSERT(new_page_id != invalid_page_id, "no unusable page");

    size_t current_rack_alloc_page_num = std::min(
        req.count, rack_table->GetMaxFreePageNum() - rack_table->GetCurrentAllocatedPageNum());
    size_t other_rack_alloc_page_num = req.count - current_rack_alloc_page_num;

    // 已分配page的序号
    size_t alloced_page_idx = 0;
    // 采取就近原则分配page到daemon所在rack
    while (alloced_page_idx < current_rack_alloc_page_num) {
        master_context.m_page_directory.AddPage(rack_table, new_page_id + alloced_page_idx);
        alloced_page_idx++;
    }

    if (other_rack_alloc_page_num > 0) {
        // 如果current daemon没有多余page，则向其他rack daemon注册allocPageMemory

        struct CTX {
            decltype(((ErpcClient*)0)->call<CortPromise>(rpc_daemon::allocPageMemory, {})) fu;
            RackMacTable* rack_table;
            page_id_t alloc_start_page_id;
            size_t alloc_cnt;
        };

        // 暂存消息上下文
        std::vector<CTX> ctx_vec;

        // 遍历rack表，找出有空闲page的rack分配
        master_context.m_cluster_manager.cluster_rack_table.foreach_all(
            [&](std::pair<const rack_id_t, RackMacTable*>& p) {
                RackMacTable* rack_table = p.second;
                size_t rack_alloc_page_num = std::min(
                    other_rack_alloc_page_num,
                    rack_table->GetMaxFreePageNum() - rack_table->GetCurrentAllocatedPageNum());

                if (rack_alloc_page_num == 0) {
                    return true;
                }

                CTX ctx;
                ctx.alloc_start_page_id = new_page_id + alloced_page_idx;
                ctx.rack_table = rack_table;
                ctx.alloc_cnt = rack_alloc_page_num;
                ctx.fu = rack_table->daemon_connect->erpc_conn->call<CortPromise>(
                    rpc_daemon::allocPageMemory,
                    {
                        .mac_id = master_context.m_master_id,
                        .start_page_id = new_page_id + alloced_page_idx,
                        .count = rack_alloc_page_num,
                    });

                ctx_vec.push_back(std::move(ctx));

                alloced_page_idx += rack_alloc_page_num;

                // 减去在该rack分配的page数
                other_rack_alloc_page_num -= rack_alloc_page_num;
                // 如果到0，则可以停止遍历
                return other_rack_alloc_page_num != 0;
            });

        // join all ctx
        for (auto& ctx : ctx_vec) {
            ctx.fu.get();

            for (size_t i = 0; i < ctx.alloc_cnt; ++i) {
                master_context.m_page_directory.AddPage(ctx.rack_table,
                                                        ctx.alloc_start_page_id + i);
            }
        }
    }

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.start_page_id = new_page_id;
    reply.start_count = current_rack_alloc_page_num;
}

void freePage(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
              FreePageRequest& req, ResponseHandle<FreePageReply>& resp_handle) {
    RackMacTable* rack_table;
    PageRackMetadata* page_meta = master_context.m_page_directory.FindPage(req.start_page_id);

    // TODO: 支持free任意rack上的page
    // 需要删除指定rack上的page meta、cache等元数据
    DLOG_FATAL("Not Support");

    rack_table = master_context.m_cluster_manager.cluster_rack_table[page_meta->rack_id];

    master_context.m_page_directory.RemovePage(rack_table, req.start_page_id);
    master_context.m_page_directory.page_id_allocator->Recycle(req.start_page_id);

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.ret = true;
}

void latchRemotePage(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
                     LatchRemotePageRequest& req,
                     ResponseHandle<LatchRemotePageReply>& resp_handle) {
    DLOG_ASSERT(req.page_id != invalid_page_id, "Invalid Page");
    DLOG_ASSERT(req.page_id != req.page_id_swap, "Can't latch same page %lu", req.page_id);

    PageRackMetadata* page_meta = master_context.m_page_directory.FindPage(req.page_id);

    DLOG_ASSERT(page_meta != nullptr, "Can't find page %lu", req.page_id);

    if (req.isWriteLock) {
        if (req.page_id_swap == invalid_page_id) {
            page_meta->latch.lock();
        } else {
            PageRackMetadata* page_swap_meta =
                master_context.m_page_directory.FindPage(req.page_id_swap);

            DLOG_ASSERT(page_swap_meta != nullptr, "Can't find page %lu", req.page_id_swap);

            // id小的优先上锁，避免死锁
            if (req.page_id < req.page_id_swap) {
                page_meta->latch.lock();
                page_swap_meta->latch.lock();
            } else {
                page_swap_meta->latch.lock();
                page_meta->latch.lock();
            }
        }
    } else {
        if (req.page_id_swap == invalid_page_id) {
            page_meta->latch.lock_shared();
        } else {
            PageRackMetadata* page_swap_meta =
                master_context.m_page_directory.FindPage(req.page_id_swap);

            DLOG_ASSERT(page_swap_meta != nullptr, "Can't find page %lu", req.page_id_swap);

            // id小的优先上锁，避免死锁
            if (req.page_id < req.page_id_swap) {
                page_meta->latch.lock_shared();
                page_swap_meta->latch.lock_shared();
            } else {
                page_swap_meta->latch.lock_shared();
                page_meta->latch.lock_shared();
            }
        }
    }

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.dest_rack_id = page_meta->rack_id;
    reply.dest_daemon_id = page_meta->daemon_id;
}

void unLatchRemotePage(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
                       UnLatchRemotePageRequest& req,
                       ResponseHandle<UnLatchRemotePageReply>& resp_handle) {
    PageRackMetadata* page_meta = master_context.m_page_directory.FindPage(req.page_id);

    page_meta->latch.unlock_shared();

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.ret = true;
}

void unLatchPageAndSwap(MasterContext& master_context, MasterToDaemonConnection& daemon_connection,
                        UnLatchPageAndSwapRequest& req,
                        ResponseHandle<UnLatchPageAndSwapReply>& resp_handle) {
    PageRackMetadata* page_meta = master_context.m_page_directory.FindPage(req.page_id);

    page_meta->rack_id = req.new_rack_id;
    page_meta->daemon_id = req.new_daemon_id;

    page_meta->latch.unlock();
    // DLOG("Swap page %lu to rack: %u, DN:%u. This operation is initiated by DN %u", req.page_id,
    //      req.new_rack_id, req.new_daemon_id, daemon_connection.daemon_id);

    if (req.page_id_swap != invalid_page_id) {
        page_meta = master_context.m_page_directory.FindPage(req.page_id_swap);

        page_meta->rack_id = req.new_rack_id_swap;
        page_meta->daemon_id = req.new_daemon_id_swap;

        page_meta->latch.unlock();
        // DLOG("Swap page %lu to rack: %u, DN:%u. This operation is initiated by DN %u",
        //      req.page_id_swap, req.new_rack_id_swap, req.new_daemon_id_swap,
        //      daemon_connection.daemon_id);
    }

    master_context.m_stats.page_swap++;

    resp_handle.Init();
    auto& reply = resp_handle.Get();
    reply.ret = true;
}

}  // namespace rpc_master