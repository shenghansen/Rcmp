#pragma once

#include <cstddef>

#include "rchms.hpp"

#define MSGQ_SINGLE_FIFO_ON 0

/**
 * @brief 页面大小2MB
 */
constexpr static size_t page_size = 2ul << 20;
constexpr static size_t cache_line_size = 64;
constexpr static size_t min_slab_size = 64;
constexpr static size_t mem_region_aligned_size = 2ul << 30;

constexpr static size_t offset_bits = __builtin_ffsl(page_size) - 1;
constexpr static size_t page_id_bits = sizeof(rchms::GAddr) * 8 - offset_bits;

/**
 * @brief 当水位大于等于该值，则进行page swap，否则dio
 */
constexpr static size_t page_hot_dio_swap_watermark = 4;

constexpr static size_t msgq_ring_buf_len = 20ul << 20;

constexpr static size_t get_page_cxl_ref_or_proxy_write_raw_max_size = UINT64_MAX;

/**
 * @brief 热度统计前后间隔us
 * 
 */
constexpr static size_t hot_stat_freq_timeout_interval = 50;