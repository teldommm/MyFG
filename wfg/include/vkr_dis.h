// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: MIT
//
// WFG frame generation: a Vulkan compute realisation of Dense Inverse Search
// optical flow. The algorithm and its reference implementation come from
// OpenCV's DISOpticalFlow, which adopted Till Kroeger's original OF_DIS.
// See LICENSE in the wfg directory for the full attribution.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Plain Vulkan types only: the renderer that includes this header calls Vulkan through its own
// table, and vk_dispatch.h would redirect its bare vkFoo names to WFG's.
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_DIS_MAX_GENERATIONS 7u

typedef struct VkrDis VkrDis;

typedef struct VkrDisContentRect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} VkrDisContentRect;

// Resolves WFG's Vulkan entry points through the renderer's driver (the adrenotools driver or
// the system loader). Call once the instance exists, before vkr_dis_create.
bool vkr_dis_bind_dispatch(PFN_vkGetInstanceProcAddr gipa, VkInstance instance);

VkrDis* vkr_dis_create(VkDevice device, VkPhysicalDevice physical_device);
void vkr_dis_destroy(VkrDis* dis);

void vkr_dis_configure(VkrDis* dis, uint32_t flow_min_side, uint32_t target_fps,
                       float refresh_rate);

void vkr_dis_set_debug_flow(VkrDis* dis, bool debug_flow);

bool vkr_dis_needs_rebuild(const VkrDis* dis, uint32_t width, uint32_t height,
                           VkFormat format, VkrDisContentRect content);

bool vkr_dis_prepare(VkrDis* dis, uint32_t width, uint32_t height, VkFormat format,
                     VkrDisContentRect content);

uint32_t vkr_dis_plan(VkrDis* dis, uint32_t capacity, uint64_t source_frames);

/* Fixed multiplier mode: n generated frames per source frame (n = multiplier - 1). 0 = plan from the
 * refresh/target ratio as before. Still limited by capacity, by the source-rate floor and by what
 * the display can show. */
void vkr_dis_set_fixed_generations(VkrDis* dis, uint32_t n);

void vkr_dis_process(VkrDis* dis, VkCommandBuffer cmd, VkImage source,
                     uint32_t width, uint32_t height, uint32_t generations);

void vkr_dis_generate_into(VkrDis* dis, VkCommandBuffer cmd, uint32_t generation,
                           uint32_t target_index, VkImage target_image,
                           VkImageView target_view, uint32_t width, uint32_t height,
                           VkImage base_image);

void vkr_dis_debug_into(VkrDis* dis, VkCommandBuffer cmd, VkImage target_image,
                        uint32_t width, uint32_t height);

void vkr_dis_forget_targets(VkrDis* dis);

void vkr_dis_reset(VkrDis* dis);

/* ---- P2: external flow (hardware motion estimation) ----------------------------------------
 * Supplies a per-block vector field computed outside WFG (e.g. Adreno's GL_QCOM_motion_
 * estimation, the OpenFG donor's glTexEstimateMotionQCOM path) for the NEXT source pair.
 * `vectors` is grid_w*grid_h pairs (x, y) in pixels of the full source frame, forward
 * (ref -> target), in the frame's content coordinate space (top-left origin, y down).
 * It is resampled to the flow resolution, its magnitudes rescaled to flow texels, and
 * written into level 0 of the refined field, replacing DIS/Farneback's search+VR chain for
 * that pair; pack/splat/interpolate run unchanged. Consume-once: the next
 * vkr_dis_process() uses it and clears the request. Call between vkr_dis_prepare() and
 * vkr_dis_process(). Returns false if the engine is not built (the field is dropped).
 * Passing NULL/0 clears any pending request (the pair falls back to DIS). */
bool vkr_dis_supply_flow(VkrDis* dis, const float* vectors, uint32_t grid_w, uint32_t grid_h,
                          uint32_t full_w, uint32_t full_h);

/* ---- P0: counters of the external-flow path (for the stats line) ----------------------------
 * Reports how many supplied fields were actually consumed by frame generation (ext_used) and
 * how many were lost (ext_dropped): rejected at supply time (no staging memory / grid coarser
 * than 4 blocks) or expired unused on a frame with no generation planned. used + dropped is the
 * full fate of every supplied field; the module prints them as me=used/dropped. */
void vkr_dis_flow_stats(const VkrDis* dis, uint64_t* ext_used, uint64_t* ext_dropped);

#ifdef __cplusplus
}
#endif
