#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "allocator.hpp"

namespace msgq {

struct MsgBuffer;
struct MsgQueue;

using msgq_handler_t = void (*)(MsgBuffer &req, MsgBuffer &resp, void *ctx);
using msgq_callback_t = void (*)(MsgBuffer &resp, void *arg);

constexpr static uint8_t INVALID_PADDING_FLAG = 255;

struct MsgHeader final {
    uint8_t rpc_type;
    enum : uint8_t { REQ, RESP } msg_type;
    size_t size;            // 实际数据大小
    msgq_callback_t cb;
    void *arg;

    uint8_t data[0];
};

struct MsgBuffer {
    size_t size();
    void *get_buf();
    void reset(size_t size);

    MsgQueue *m_q;
    MsgHeader *m_msg;      // 指向MsgHeader的地址
    size_t m_size;          // 实际数据大小
};


struct MsgQueue final {
    constexpr static size_t RING_BUF_OFF = 16;

    MsgQueue();
    ~MsgQueue();

    MsgHeader * alloc_msg_buffer(size_t size);
    void enqueue_msg(uint8_t rpc_type, MsgHeader *h, size_t size);
    size_t dequeue_msg(MsgHeader **start_h);
    void free_msg_buffer(MsgHeader *h, size_t size, bool tail_valid);

    struct msg_ring_headtail {
        std::atomic<uint32_t> head; /**< prod/consumer head. */
        std::atomic<uint32_t> tail; /**< prod/consumer tail. */
    };

    msg_ring_headtail buf_head;  // enq
    msg_ring_headtail buf_tail;  // deq

    uint8_t ring_buf[0];
};

struct MsgQueueNexus {
    constexpr static size_t max_msgq_handler = (1 << (sizeof(uint8_t) * 8)) - 1;

    MsgQueueNexus(void *msgq_zone_start_addr);

    void register_req_func(uint8_t rpc_type, msgq_handler_t handler);

    static msgq_handler_t __handlers[max_msgq_handler];

    void *m_msgq_zone_start_addr;
};

struct MsgQueueRPC {

    MsgQueueRPC(MsgQueueNexus *nexus, void *ctx);

    MsgBuffer alloc_msg_buffer(size_t size);
    void enqueue_request(uint8_t rpc_type, MsgBuffer& msg_buf,
                         msgq_callback_t cb, void *arg);
    void run_event_loop_once();
    void free_msg_buffer(MsgBuffer& msg_buf);

    MsgQueueNexus *m_nexus;
    MsgQueue *m_send_queue;
    MsgQueue *m_recv_queue;
    void *m_ctx;
};

struct MsgQueueManager {
    const static size_t MAX_RING_NUM = 16;
    const static size_t RING_BUF_LEN = 2048;
    const static size_t RING_ELEM_SIZE = sizeof(MsgQueue) + RING_BUF_LEN;
    const static size_t RING_BUF_OFF = sizeof(MsgQueue);
    const static size_t RING_BUF_END_OFF = sizeof(MsgQueue) + RING_BUF_LEN;

    uintptr_t start_addr;
    uint32_t ring_cnt;
    uint64_t start_off;  // 下一个可用的偏移位置
    uintptr_t head_off;
    uintptr_t tail_off;  // 相对偏移
    SingleAllocator msgq_allocator;

    MsgQueueManager();
    ~MsgQueueManager();

    MsgQueue *alloc_ring();
    static uint32_t BUF_PLUS(uint32_t p, uint32_t len) {
        return (p + len) % RING_BUF_LEN;
    }
    void free_ring(MsgQueue *msgq);
};

}