/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "engine_fast.h"
#include "engine_common.h"
#include "engine_pt.h"
#include "engine_wb.h"
#include "../ocf_request.h"
#include "../utils/utils_user_part.h"
#include "../utils/utils_io.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "netCAS_splitter.h"
#include "netCAS_common.h"
#include "netCAS_monitor.h"
#include "../utils/pmem_nvme/pmem_nvme_table.h"
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/random.h>

/* NetCAS Splitter - Handles cache/backend request distribution */

// Constants
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL

// Debug flag for this file - can be set to 0 or 1
static int netCAS_debug = 0;

// Function to set debug level for this file
void netcas_set_debug(int debug_level)
{
    netCAS_debug = debug_level;
}

// Local debug macro that uses this file's debug flag
#define NETCAS_SPLITTER_DEBUG_LOG(cache, format, ...) \
    NETCAS_DEBUG_LOG(cache, format, ##__VA_ARGS__)

// Constants from original netCAS implementation
#define MONITOR_INTERVAL_MS 100         /* Check every 0.1 second */
#define LOG_INTERVAL_MS 1000            /* Log every 1 second */
#define RDMA_THRESHOLD 100              /* Threshold for starting warmup */
#define BW_CONGESTION_THRESHOLD 90      /* 9.0% drop threshold for bandwidth congestion */
#define LATENCY_CONGESTION_THRESHOLD 70 /* 7.0% drop threshold for latency congestion */
#define BW_RECOVERY_THRESHOLD 70        /* 7.0% drop threshold for bandwidth recovery */
#define LATENCY_RECOVERY_THRESHOLD 50   /* 5.0% drop threshold for latency recovery */
#define RDMA_LATENCY_THRESHOLD 1000000  /* 1ms in nanoseconds */
#define IOPS_THRESHOLD 1000             /* 1000 IOPS */

/* Latency baseline management constants */
#define LATENCY_STABILIZATION_SAMPLES 40 /* Wait 10 samples before setting baseline */

/* Scale constants for split ratio (0-10000 where 10000 = 100%) - now in netcas_common.h */

/* Test app parameters */
static const uint64_t IO_DEPTH = 16;
static const uint64_t NUM_JOBS = 1;
static const bool CACHING_FAILED = false;

// Static variables for tracking split statistics (simplified for random-based approach)
static uint32_t request_counter = 0;
static uint32_t cache_quota = 0;
static uint32_t backend_quota = 0;
static bool last_request_to_cache = false;
static uint32_t pattern_position = 0;
static uint32_t pattern_cache = 0;
static uint32_t pattern_backend = 0;
static uint32_t pattern_size = 0;
static uint32_t total_requests = 0;
static uint32_t cache_requests = 0;
static uint32_t backend_requests = 0;

// Configuration constants
static const uint32_t WINDOW_SIZE = 100;
static const uint32_t MAX_PATTERN_SIZE = 10;

// Moving average window for RDMA throughput
static uint64_t rdma_throughput_window[RDMA_WINDOW_SIZE] = {0};
static uint64_t rdma_window_index = 0;
static uint64_t rdma_window_sum = 0;
static uint64_t rdma_window_count = 0;
static uint64_t rdma_window_average = 0;
static uint64_t max_average_rdma_throughput = 0;

// Moving average window for RDMA latency
static uint64_t rdma_latency_window[RDMA_WINDOW_SIZE] = {0};
static uint64_t rdma_latency_window_index = 0;
static uint64_t rdma_latency_window_sum = 0;
static uint64_t rdma_latency_window_count = 0;
static uint64_t rdma_latency_window_average = 0;
static uint64_t min_average_rdma_latency = UINT64_MAX;

// Latency baseline management variables
static uint64_t latency_sample_count = 0;
static bool latency_baseline_established = false;

// Mode management variables
static bool netCAS_initialized = false;
static bool split_ratio_calculated_in_stable = false; // Track if split ratio was calculated in stable mode
static netCAS_mode_t current_mode = NETCAS_MODE_IDLE;

// Optimal split ratio management
static uint64_t optimal_split_ratio = SPLIT_RATIO_MAX; // Default 100% to cache
static env_rwlock split_ratio_lock;

// Global variable to track last logged time (unused for now)
// static uint64_t last_logged_time = 0;

// Timing control for monitor updates
static uint64_t last_monitor_update_time = 0;
static uint64_t last_logged_time = 0;

// lookup_bandwidth function is now available from pmem_nvme_table.h

/**
 * @brief Update RDMA throughput window for moving average calculation
 */
static void update_rdma_window(uint64_t curr_rdma_throughput)
{
    // Update window
    if (rdma_window_count < RDMA_WINDOW_SIZE)
    {
        rdma_window_count++;
    }
    else
    {
        rdma_window_sum -= rdma_throughput_window[rdma_window_index];
    }
    rdma_throughput_window[rdma_window_index] = curr_rdma_throughput;
    rdma_window_sum += curr_rdma_throughput;
    rdma_window_average = rdma_window_sum / rdma_window_count;
    rdma_window_index = (rdma_window_index + 1) % RDMA_WINDOW_SIZE;

    if (max_average_rdma_throughput < rdma_window_average)
    {
        max_average_rdma_throughput = rdma_window_average;
        NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: max_average_rdma_throughput: %llu", max_average_rdma_throughput);
    }
}

/**
 * @brief Update RDMA latency window for moving average calculation
 */
static void update_rdma_latency_window(uint64_t curr_rdma_latency)
{
    // Update window
    if (rdma_latency_window_count < RDMA_WINDOW_SIZE)
    {
        rdma_latency_window_count++;
    }
    else
    {
        rdma_latency_window_sum -= rdma_latency_window[rdma_latency_window_index];
    }
    rdma_latency_window[rdma_latency_window_index] = curr_rdma_latency;
    rdma_latency_window_sum += curr_rdma_latency;
    rdma_latency_window_average = rdma_latency_window_sum / rdma_latency_window_count;
    rdma_latency_window_index = (rdma_latency_window_index + 1) % RDMA_WINDOW_SIZE;

    // Increment sample count for baseline stabilization
    latency_sample_count++;

    // Only establish baseline after stabilization period
    if (latency_sample_count >= LATENCY_STABILIZATION_SAMPLES)
    {
        if (!latency_baseline_established)
        {
            // First time establishing baseline
            if (rdma_latency_window_average > 0)
            {
                min_average_rdma_latency = rdma_latency_window_average;
                latency_baseline_established = true;
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Latency baseline established: %llu (after %llu samples)",
                                          min_average_rdma_latency, latency_sample_count);
            }
            else
            {
                // Wait for valid latency value
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Waiting for valid latency value (current: %llu)",
                                          rdma_latency_window_average);
            }
        }
        else
        {
            // Update min latency if current average is lower
            if (rdma_latency_window_average < min_average_rdma_latency)
            {
                min_average_rdma_latency = rdma_latency_window_average;
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: New min latency: %llu", min_average_rdma_latency);
            }
        }
    }

    NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: rdma_latency_window_average: %llu, baseline: %llu, established: %d",
                              rdma_latency_window_average, min_average_rdma_latency, latency_baseline_established);
}

/**
 * @brief Initialize the netcas splitter
 */
void netcas_splitter_init(void)
{
    int i;

    env_rwlock_init(&split_ratio_lock);
    optimal_split_ratio = SPLIT_RATIO_MAX; // Default 100% to cache

    // Initialize mode management variables
    netCAS_initialized = false;
    split_ratio_calculated_in_stable = false;
    current_mode = NETCAS_MODE_IDLE;

    // Initialize RDMA throughput window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_throughput_window[i] = 0;
    rdma_window_sum = 0;
    rdma_window_index = 0;
    rdma_window_count = 0;
    rdma_window_average = 0;
    max_average_rdma_throughput = 0;

    // Initialize RDMA latency window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_latency_window[i] = 0;
    rdma_latency_window_sum = 0;
    rdma_latency_window_index = 0;
    rdma_latency_window_count = 0;
    rdma_latency_window_average = 0;
    min_average_rdma_latency = UINT64_MAX;

    // Initialize latency baseline management
    latency_sample_count = 0;
    latency_baseline_established = false;

    NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Splitter initialized");
}

/**
 * @brief Set split ratio value with writer lock.
 */
static void split_set_optimal_ratio(uint64_t ratio)
{
    env_rwlock_write_lock(&split_ratio_lock);
    optimal_split_ratio = ratio;
    env_rwlock_write_unlock(&split_ratio_lock);
}

/**
 * @brief Calculate split ratio using the formula A/(A+B) * 10000.
 * This is the core formula for determining optimal split ratio.
 * Uses 0-10000 scale where 10000 = 100% for more accurate calculations.
 */
static uint64_t calculate_split_ratio_formula(uint64_t bandwidth_cache_only, uint64_t bandwidth_backend_only)
{
    uint64_t calculated_split;

    /* Calculate optimal split ratio using formula A/(A+B) * 10000 */
    calculated_split = (bandwidth_cache_only * SPLIT_RATIO_SCALE) / (bandwidth_cache_only + bandwidth_backend_only);

    /* Ensure the result is within valid range (0-10000) */
    if (calculated_split < SPLIT_RATIO_MIN)
        calculated_split = SPLIT_RATIO_MIN;
    if (calculated_split > SPLIT_RATIO_MAX)
        calculated_split = SPLIT_RATIO_MAX;

    return calculated_split;
}

/**
 * @brief Function to find the best split ratio for given IO depth and NumJob.
 * Based on the algorithm from netCAS_split.c
 * Returns split ratio in 0-10000 scale where 10000 = 100%.
 */
static uint64_t find_best_split_ratio(uint64_t io_depth, uint64_t numjob, uint64_t drop_permil, uint64_t latency_increase_permil)
{
    uint64_t bandwidth_cache_only;   /* A: IOPS when split ratio is 100% (all to cache) */
    uint64_t bandwidth_backend_only; /* B: IOPS when split ratio is 0% (all to backend) */
    uint64_t calculated_split;       /* Calculated optimal split ratio */

    /* Get bandwidth for cache only (split ratio 100%) */
    bandwidth_cache_only = (uint64_t)lookup_bandwidth(io_depth, numjob, 100);
    /* Get bandwidth for backend only (split ratio 0%) */
    bandwidth_backend_only = (uint64_t)lookup_bandwidth(io_depth, numjob, 0);

    // Apply latency increase percentage to backend bandwidth if there's congestion
    if (latency_increase_permil > LATENCY_CONGESTION_THRESHOLD)
    {
        bandwidth_backend_only = (uint64_t)((bandwidth_backend_only * (1000 - drop_permil)) / 1000);
    }

    /* Calculate optimal split ratio using the formula */
    calculated_split = calculate_split_ratio_formula(bandwidth_cache_only, bandwidth_backend_only);

    // Log the calculation for debugging
    NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Split ratio calculation - Cache BW: %llu, Backend BW: %llu, Drop: %llu%%, Result: %llu.%02llu%%",
                              bandwidth_cache_only, bandwidth_backend_only, drop_permil / 10,
                              calculated_split / 100, calculated_split % 100);

    return calculated_split;
}

/**
 * @brief Determine the current netCAS mode based on performance metrics
 */
static netCAS_mode_t determine_netcas_mode(uint64_t curr_rdma_throughput, uint64_t curr_rdma_latency, uint64_t curr_iops, uint64_t bw_drop_permil, uint64_t latency_increase_permil)
{
    netCAS_mode_t previous_mode = current_mode;

    // No Active RDMA traffic or no IOPS, set netCAS_mode to IDLE
    if (curr_rdma_throughput <= RDMA_THRESHOLD && curr_iops <= IOPS_THRESHOLD)
    {
        current_mode = NETCAS_MODE_IDLE;
    }
    // Active RDMA traffic, determine the mode
    else
    {
        // First time active RDMA traffic, set netCAS_mode to WARMUP
        if (current_mode == NETCAS_MODE_IDLE)
        {
            // Idle -> Warmup
            NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed from IDLE to WARMUP");
            current_mode = NETCAS_MODE_WARMUP;
            netCAS_initialized = false;
        }
        else if (current_mode == NETCAS_MODE_WARMUP)
        {
            // Warmup -> Stable
            if (rdma_window_count >= RDMA_WINDOW_SIZE)
            {
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed from WARMUP to STABLE (window full)");
                current_mode = NETCAS_MODE_STABLE;
                split_ratio_calculated_in_stable = false; // Reset flag when entering stable mode
            }
            else
            {
                // Still in warmup, do nothing
            }
        }
        else if (current_mode == NETCAS_MODE_CONGESTION &&
                 (latency_increase_permil < LATENCY_RECOVERY_THRESHOLD))
        {
            // Congestion -> Stable (recovery if either metric recovers)
            NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed from CONGESTION to STABLE (BW_Drop: %llu%%, Lat_Drop: %llu%%)",
                                      bw_drop_permil / 10, latency_increase_permil / 10);
            current_mode = NETCAS_MODE_STABLE;
            split_ratio_calculated_in_stable = false; // Reset flag when entering stable mode
        }
        else if (current_mode == NETCAS_MODE_STABLE &&
                 (latency_increase_permil > LATENCY_CONGESTION_THRESHOLD))
        {
            // Stable -> Congestion (enter if either metric exceeds threshold)
            NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed from STABLE to CONGESTION (BW_Drop: %llu%%, Lat_Drop: %llu%%)",
                                      bw_drop_permil / 10, latency_increase_permil / 10);
            current_mode = NETCAS_MODE_CONGESTION;
            split_ratio_calculated_in_stable = true; // Set flag when entering congestion
        }
        else if (CACHING_FAILED)
        {
            NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed to FAILURE");
            current_mode = NETCAS_MODE_FAILURE;
        }
    }

    // Log mode changes
    if (previous_mode != current_mode)
    {
        NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Mode changed from %d to %d (RDMA: %llu, IOPS: %llu, BW_Drop: %llu%%, Lat_Drop: %llu%%)",
                                  previous_mode, current_mode, curr_rdma_throughput, curr_iops, bw_drop_permil / 10, latency_increase_permil / 10);
    }

    return current_mode;
}

/**
 * @brief Update the optimal split ratio based on current conditions
 */
void netcas_update_split_ratio(struct ocf_request *req)
{
    uint64_t new_split_ratio;
    uint64_t curr_rdma_throughput = 0;
    uint64_t curr_rdma_latency = 0;
    uint64_t curr_iops = 0;
    uint64_t elapsed_time = MONITOR_INTERVAL_MS; // Use the same interval as original
    uint64_t bw_drop_permil = 0;
    uint64_t latency_increase_permil = 0;
    struct performance_metrics metrics;
    netCAS_mode_t netCAS_mode;
    uint64_t current_time = jiffies_to_msecs(jiffies);

    // Only update monitor and split ratio at the proper intervals
    if (current_time - last_monitor_update_time >= MONITOR_INTERVAL_MS)
    {
        // Measure current performance metrics using netCAS_monitor
        metrics = measure_performance(elapsed_time);
        curr_rdma_throughput = metrics.rdma_throughput;
        curr_rdma_latency = metrics.rdma_latency;
        curr_iops = metrics.iops;

        // Update RDMA throughput window for moving average calculation
        update_rdma_window(curr_rdma_throughput);
        // Update RDMA latency window for moving average calculation
        update_rdma_latency_window(curr_rdma_latency);

        // Calculate drop percentage if we have enough data
        if (max_average_rdma_throughput > 0)
        {
            bw_drop_permil = ((max_average_rdma_throughput - rdma_window_average) * 1000) / max_average_rdma_throughput;
        }

        // Only calculate latency increase if baseline is established
        if (latency_baseline_established && min_average_rdma_latency < UINT64_MAX)
        {
            latency_increase_permil = ((rdma_latency_window_average - min_average_rdma_latency) * 1000) / min_average_rdma_latency;
        }
        else
        {
            latency_increase_permil = 0; // No baseline yet
        }

        // Determine current mode based on performance metrics
        netCAS_mode = determine_netcas_mode(curr_rdma_throughput, curr_rdma_latency, curr_iops, bw_drop_permil, latency_increase_permil);

        // Update split ratio based on mode
        switch (netCAS_mode)
        {
        case NETCAS_MODE_IDLE:
            if (!netCAS_initialized)
            {
                // Initialize with default values
                optimal_split_ratio = SPLIT_RATIO_MAX;
                split_set_optimal_ratio(optimal_split_ratio);
                netCAS_initialized = true;
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: IDLE mode - initialized with default split ratio");
            }
            break;

        case NETCAS_MODE_WARMUP:
            // In warmup mode, calculate split ratio without drop (assuming no contention in startup)
            new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, 0, 0);
            if (new_split_ratio != optimal_split_ratio)
            {
                optimal_split_ratio = new_split_ratio;
                split_set_optimal_ratio(optimal_split_ratio);
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: WARMUP mode - Updated split ratio to: %llu.%02llu%% (RDMA: %llu, IOPS: %llu)",
                                          new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops);
            }
            break;

        case NETCAS_MODE_STABLE:
            // Only calculate split ratio once in stable mode
            if (!split_ratio_calculated_in_stable && rdma_window_count >= RDMA_WINDOW_SIZE)
            {
                new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, bw_drop_permil, latency_increase_permil);
                optimal_split_ratio = new_split_ratio;
                split_set_optimal_ratio(optimal_split_ratio);
                split_ratio_calculated_in_stable = true; // Mark as calculated
                NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: STABLE mode - Calculated split ratio: %llu.%02llu%% (RDMA: %llu, IOPS: %llu, Drop: %llu%%)",
                                          new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops, bw_drop_permil / 10);
            }
            break;

        case NETCAS_MODE_CONGESTION:
            // Continuously calculate split ratio in congestion mode
            if (rdma_window_count >= RDMA_WINDOW_SIZE)
            {
                new_split_ratio = find_best_split_ratio(IO_DEPTH, NUM_JOBS, bw_drop_permil, latency_increase_permil);

                // Update the split ratio if it changed
                if (new_split_ratio != optimal_split_ratio)
                {
                    optimal_split_ratio = new_split_ratio;
                    split_set_optimal_ratio(new_split_ratio);
                    NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: CONGESTION mode - Updated split ratio to: %llu.%02llu%% (RDMA: %llu, IOPS: %llu, Drop: %llu%%)",
                                              new_split_ratio / 100, new_split_ratio % 100, curr_rdma_throughput, curr_iops, bw_drop_permil / 10);
                }
            }
            break;

        case NETCAS_MODE_FAILURE:
            // In failure mode, keep current ratio or set to safe default
            NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: FAILURE mode - Keeping current split ratio: %llu.%02llu%% (RDMA: %llu, IOPS: %llu)",
                                      optimal_split_ratio / 100, optimal_split_ratio % 100, curr_rdma_throughput, curr_iops);
            break;
        }

        // Update the last monitor update time
        last_monitor_update_time = current_time;
    }
    if (current_time - last_logged_time >= LOG_INTERVAL_MS)
    {
        printk("netCAS: Current metrics - RDMA: %llu, RDMA_Lat: %llu (baseline: %llu), IOPS: %llu, BW_Drop: %llu%%, Lat_Inc: %llu%%, Mode: %d, Split Ratio: %llu.%02llu%%",
               curr_rdma_throughput, rdma_latency_window_average, min_average_rdma_latency, curr_iops, bw_drop_permil / 10, latency_increase_permil / 10, current_mode, (unsigned long long)optimal_split_ratio / 100, (unsigned long long)optimal_split_ratio % 100);
        last_logged_time = current_time;
        printk("MONITOR: query_load_admit returning: %llu\n", (unsigned long long)optimal_split_ratio);
    }
}

/**
 * @brief Calculate GCD using Euclidean algorithm
 */
static uint32_t calculate_gcd(uint32_t a, uint32_t b)
{
    uint32_t temp;

    if (a > 0 && b > 0)
    {
        while (b != 0)
        {
            temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }
    return 1;
}

/**
 * @brief Initialize or recalculate the splitting pattern
 */
static void initialize_split_pattern(uint64_t split_ratio)
{
    uint32_t gcd;
    uint32_t a = (uint32_t)(split_ratio / 100); // Convert from 0-10000 scale to 0-100
    uint32_t b = WINDOW_SIZE - a;

    // Calculate GCD
    gcd = calculate_gcd(a, b);

    // Calculate pattern size (limited by MAX_PATTERN_SIZE)
    pattern_size = (a + (WINDOW_SIZE - a)) / gcd;
    if (pattern_size > MAX_PATTERN_SIZE)
    {
        pattern_size = MAX_PATTERN_SIZE;
    }

    // Calculate cache and backend requests in pattern
    pattern_cache = (a * pattern_size) / WINDOW_SIZE;
    pattern_backend = pattern_size - pattern_cache;

    // Reset counters
    total_requests = 0;
    cache_requests = 0;
    backend_requests = 0;

    // Initialize quotas
    cache_quota = a;
    backend_quota = WINDOW_SIZE - a;
    pattern_position = 0;
}

/**
 * @brief Decide whether to send request to cache or backend (Original pattern-based version)
 * @param req The OCF request
 * @return true if request should go to backend, false for cache
 */
bool netcas_should_send_to_backend(struct ocf_request *req)
{
    bool send_to_backend;
    uint32_t expected_cache_ratio;
    uint32_t expected_backend_ratio;
    uint64_t current_split_ratio;
    uint32_t split_ratio_percent;

    // Update split ratio based on current performance metrics
    netcas_update_split_ratio(req);

    // Get current optimal split ratio
    current_split_ratio = optimal_split_ratio;
    // Convert from 0-10000 scale to 0-100 for internal calculations
    split_ratio_percent = (uint32_t)(current_split_ratio / 100);

    // Log current split ratio for debugging
    // NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Current split ratio: %llu.%02llu%% (percent: %u)",
    //                           current_split_ratio / 100, current_split_ratio % 100, split_ratio_percent);

    // Initialize or recalculate pattern when needed
    if (request_counter % WINDOW_SIZE == 0 || pattern_size == 0)
    {
        initialize_split_pattern(current_split_ratio);
    }

    // Increment counters
    request_counter++;
    total_requests++;

    // Check for miss first
    if (ocf_engine_is_miss(req))
    {
        NETCAS_SPLITTER_DEBUG_LOG(NULL, "Backend (miss)");
        return true;
    }

    // Calculate expected ratios
    expected_cache_ratio = (total_requests * split_ratio_percent) / WINDOW_SIZE;
    expected_backend_ratio = total_requests - expected_cache_ratio;

    // Determine where to send request based on current distribution
    if (cache_requests < expected_cache_ratio)
    {
        // Cache requests are below expected ratio
        send_to_backend = false;
    }
    else if (backend_requests < expected_backend_ratio)
    {
        // Backend requests are below expected ratio
        send_to_backend = true;
    }
    else
    {
        // Both are at expected ratios, use pattern-based distribution
        if (pattern_position < pattern_size)
        {
            // Pattern-based distribution
            send_to_backend = (pattern_position >= pattern_cache);
            pattern_position = (pattern_position + 1) % pattern_size;
        }
        else
        {
            // Pattern exhausted, use quota-based distribution
            if (cache_quota == 0)
            {
                send_to_backend = true;
            }
            else if (backend_quota == 0)
            {
                send_to_backend = false;
            }
            else
            {
                // Both quotas available, alternate to maintain balance
                send_to_backend = last_request_to_cache;
            }
        }
    }

    // Update counters and quotas
    if (send_to_backend)
    {
        backend_quota--;
        backend_requests++;
        last_request_to_cache = false;
        // NETCAS_SPLITTER_DEBUG_LOG(NULL, "Backend (hit) - split_ratio: %llu.%02llu%%",
        //                           current_split_ratio / 100, current_split_ratio % 100);
    }
    else
    {
        cache_quota--;
        cache_requests++;
        last_request_to_cache = true;
        // NETCAS_SPLITTER_DEBUG_LOG(NULL, "Cache (hit) - split_ratio: %llu.%02llu%%",
        //                           current_split_ratio / 100, current_split_ratio % 100);
    }

    return send_to_backend;
}

/**
 * @brief Reset all splitter statistics (useful for testing or reconfiguration)
 */
void netcas_reset_splitter(void)
{
    int i;

    request_counter = 0;
    cache_quota = 0;
    backend_quota = 0;
    last_request_to_cache = false;
    pattern_position = 0;
    pattern_cache = 0;
    pattern_backend = 0;
    pattern_size = 0;
    total_requests = 0;
    cache_requests = 0;
    backend_requests = 0;

    // Reset optimal split ratio to default
    split_set_optimal_ratio(SPLIT_RATIO_MAX);

    // Reset mode management variables
    netCAS_initialized = false;
    split_ratio_calculated_in_stable = false;
    current_mode = NETCAS_MODE_IDLE;

    // Reset RDMA throughput window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_throughput_window[i] = 0;
    rdma_window_sum = 0;
    rdma_window_index = 0;
    rdma_window_count = 0;
    rdma_window_average = 0;
    max_average_rdma_throughput = 0;
    last_monitor_update_time = 0;

    // Reset RDMA latency window
    for (i = 0; i < RDMA_WINDOW_SIZE; ++i)
        rdma_latency_window[i] = 0;
    rdma_latency_window_sum = 0;
    rdma_latency_window_index = 0;
    rdma_latency_window_count = 0;
    rdma_latency_window_average = 0;
    min_average_rdma_latency = UINT64_MAX;

    // Reset latency baseline management
    latency_sample_count = 0;
    latency_baseline_established = false;

    NETCAS_SPLITTER_DEBUG_LOG(NULL, "netCAS: Splitter reset");
}