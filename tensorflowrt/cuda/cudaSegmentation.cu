/* ============================================================================
# [2017] - Robik AI Ltd - Paul Balanca
# All Rights Reserved.

# NOTICE: All information contained herein is, and remains
# the property of Robik AI Ltd, and its suppliers
# if any.  The intellectual and technical concepts contained
# herein are proprietary to Robik AI Ltd
# and its suppliers and may be covered by U.S., European and Foreign Patents,
# patents in process, and are protected by trade secret or copyright law.
# Dissemination of this information or reproduction of this material
# is strictly forbidden unless prior written permission is obtained
# from Robik AI Ltd.
# =========================================================================== */
#include <vector>
#include <array>
#include "cudaUtility.h"

#define ALPHA_COLOR 128

// selected_categories = {
//     Categories.ignore: 0,
//     Categories.car: 1,
//     Categories.van: 2,
//     Categories.truck: 3,
//     Categories.bus: 4,
//     Categories.bicycle: 5,
//     Categories.motorcycle: 6,
//     Categories.person: 7,
//     Categories.road: 8,
//     Categories.sidewalk: 9,
//     Categories.terrain: 10,
//     Categories.traffic_sign: 11,
//     Categories.traffic_light: 12,
//     Categories.vegetation: 13,
//     Categories.building: 14,
//     Categories.sky: 15,
//     Categories.fence: 16,
//     Categories.pole: 17,
//     Categories.parking: 18,
// }

// ========================================================================== //
// Segmentation overlay.
// ========================================================================== //
__global__ void kernel_seg_overlay(
    uint8_t* d_img, uint8_t* d_mask, float2 scale,
    uint32_t img_width, uint32_t img_height,
    uint32_t img_stride_x, uint32_t img_stride_y,
    uint32_t mask_width, uint32_t mask_height,
    uint32_t mask_stride_x, uint32_t mask_stride_y)
{
    // UGLY! I know!
    /* static uchar4 colors[] = {
        {0, 0, 0, ALPHA_COLOR},
        {0, 0, 142, ALPHA_COLOR},
        {0, 110, 100, ALPHA_COLOR},
        {30, 30, 70, ALPHA_COLOR},
        {0, 60, 100, ALPHA_COLOR},
        {119, 11, 32, ALPHA_COLOR},
        {50, 0, 230, ALPHA_COLOR},
        {220, 20, 60, ALPHA_COLOR},
        {128, 64, 128, ALPHA_COLOR},
        {244, 35, 232, ALPHA_COLOR},
        {152, 251, 152, ALPHA_COLOR},
        {220, 220, 0, ALPHA_COLOR},
        {250, 170, 30, ALPHA_COLOR},
        {107, 142, 35, ALPHA_COLOR},
        {70, 70, 70, ALPHA_COLOR},
        {70, 130, 180, ALPHA_COLOR},
        {190, 153, 153, ALPHA_COLOR},
        {153, 53, 153, ALPHA_COLOR},
        {250, 170, 160, ALPHA_COLOR}
    };*/
    // UGLY! I know!
    static uchar4 colors[] = {
        {0, 0, 0, ALPHA_COLOR},
        {0, 0, 142, ALPHA_COLOR},
        {0, 0, 142, ALPHA_COLOR},
        {0, 0, 142, ALPHA_COLOR},
        {0, 0, 142, ALPHA_COLOR},
        {119, 11, 32, ALPHA_COLOR},
        {50, 0, 230, ALPHA_COLOR},
        {220, 20, 60, ALPHA_COLOR},
        {128, 64, 128, ALPHA_COLOR},
        {244, 35, 232, ALPHA_COLOR},
        {152, 251, 152, ALPHA_COLOR},
        {220, 220, 0, ALPHA_COLOR},
        {250, 170, 30, ALPHA_COLOR},
        {107, 142, 35, ALPHA_COLOR},
        {70, 70, 70, ALPHA_COLOR},
        {70, 130, 180, ALPHA_COLOR},
        {190, 153, 153, ALPHA_COLOR},
        {153, 53, 153, ALPHA_COLOR},
        {250, 170, 160, ALPHA_COLOR}
    };

    // Image coordinates.
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if( x >= img_width || y >= img_height ) {
        return;
    }
    const int idx = y * img_stride_y + x * img_stride_x;
    // Convertion to mask coordinates.
    const int mx = max(min(int((float)x * scale.x), mask_width-1), 0);
    const int my = max(min(int((float)y * scale.y), mask_height-1), 0);

    const int mval = d_mask[my * mask_stride_y + mx * mask_stride_x];
    // const uchar3 color = make_uchar3(colors[mval].x, colors[mval].y, colors[mval].z);
    // const float alpha = float(colors[mval].w) / 255.f;
    // const int mval = 1;
    const uchar3 color = make_uchar3(colors[mval].x, colors[mval].y, colors[mval].z);
    const float alpha = float(colors[mval].w) / 255.f;
    // const uchar3 color = make_uchar3(255, 0, 0);
    // const float alpha = 0.3;

    // Mask overlay.
    const uchar3 rgb = make_uchar3(
        d_img[idx + 0] * (1. - alpha) + color.x * alpha,
        d_img[idx + 1] * (1. - alpha) + color.y * alpha,
        d_img[idx + 2] * (1. - alpha) + color.z * alpha);
    d_img[idx + 0] = rgb.x;
    d_img[idx + 1] = rgb.y;
    d_img[idx + 2] = rgb.z;
}
cudaError_t cuda_seg_overlay(
    uint8_t* d_img, uint8_t* d_mask,
    uint32_t img_width, uint32_t img_height,
    uint32_t img_stride_x, uint32_t img_stride_y,
    uint32_t mask_width, uint32_t mask_height,
    uint32_t mask_stride_x, uint32_t mask_stride_y)
{
    if( !d_img || !d_mask ) {
        return cudaErrorInvalidDevicePointer;
    }
    if( img_width == 0 || img_height == 0 || img_stride_x == 0 || img_stride_y == 0 ) {
        return cudaErrorInvalidValue;
    }
    if( mask_width == 0 || mask_height == 0 || mask_stride_x == 0 || mask_stride_y == 0 ) {
        return cudaErrorInvalidValue;
    }
    // Scale between image and mask.
    const float2 scale = make_float2( float(mask_width) / float(img_width),
                                      float(mask_height) / float(img_height) );
    // Launch kernel!
    const dim3 blockDim(8, 8);
    const dim3 gridDim(iDivUp(img_width,blockDim.x), iDivUp(img_height,blockDim.y));
    kernel_seg_overlay<<<gridDim, blockDim>>>(d_img, d_mask, scale,
        img_width, img_height, img_stride_x, img_stride_y,
        mask_width, mask_height, mask_stride_x, mask_stride_y);
    return cudaGetLastError();
}

// ========================================================================== //
// Segmentation post-processing.
// ========================================================================== //
__global__ void kernel_seg_post_process(
    float* d_raw_prob, uint8_t* d_classes, float* d_scores, 
    float* d_tr_matrix, uint32_t seg_width, uint32_t seg_height, uint32_t num_classes,
    bool empty_class, float threshold)
{
    // Image coordinates.
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if( x >= seg_width || y >= seg_height ) {
        return;
    }
    const int d_idx = y * seg_width + x;
    
    // New coordinates after transformation!
    // const float tx_f = (x+0.) * d_tr_matrix[0] + (y+0.) * d_tr_matrix[1] + d_tr_matrix[2];
    // const float ty_f = (x+0.) * d_tr_matrix[3] + (y+0.) * d_tr_matrix[4] + d_tr_matrix[5];
    // const float tz_f = (x+0.) * d_tr_matrix[6] + (y+0.) * d_tr_matrix[7] + d_tr_matrix[8];
    
    // FUCKING COLUMN MAJOR in OpenVX!!! Bullshit documentation!
    const float tx_f = (x+0.5) * d_tr_matrix[0] + (y+0.5) * d_tr_matrix[3] + d_tr_matrix[6];
    const float ty_f = (x+0.5) * d_tr_matrix[1] + (y+0.5) * d_tr_matrix[4] + d_tr_matrix[7];
    const float tz_f = (x+0.5) * d_tr_matrix[2] + (y+0.5) * d_tr_matrix[5] + d_tr_matrix[8];
    const int tx = floor(tx_f / tz_f);
    const int ty = floor(ty_f / tz_f);

    if( tx >= seg_width || tx < 0 || ty >= seg_height || ty < 0 ) {
        d_classes[d_idx] = 0;
        d_scores[d_idx] = 1.0;
        return;
    }
    // Find the highest probabilty...
    const int drift = !empty_class;
    uint8_t max_idx = 0;
    float max_score = 0.0f;
    float p;
    int idx;
    for (int k = 0 ; k < num_classes ; ++k) {
        // CHW tensor format.
        idx = k*seg_width*seg_height + ty*seg_width + tx;
        p = d_raw_prob[idx];
        if (p > max_score) {
            max_idx = uint8_t(k);
            max_score = p;
        }
    }
    // Assign values.
    if (max_score > threshold) {
        d_classes[d_idx] = max_idx + drift;
        d_scores[d_idx] = max_score;
    }
    else {
        d_classes[d_idx] = 0;
        d_scores[d_idx] = max_score;
    }
    // Testing only.
    // d_classes[d_idx] = 2;
    // d_scores[d_idx] = max_score;
}
cudaError_t cuda_seg_post_process(
    float* d_raw_prob, uint8_t* d_classes, float* d_scores, 
    float* d_tr_matrix, uint32_t seg_width, uint32_t seg_height, uint32_t num_classes,
    bool empty_class, float threshold)
{
    if( !d_raw_prob || !d_classes || !d_scores ) {
        return cudaErrorInvalidDevicePointer;
    }
    if( seg_width == 0 || seg_height == 0 || num_classes == 0 ) {
        return cudaErrorInvalidValue;
    }
    // Launch kernel!
    const dim3 blockDim(8, 8);
    const dim3 gridDim(iDivUp(seg_width,blockDim.x), iDivUp(seg_height,blockDim.y));
    kernel_seg_post_process<<<gridDim, blockDim>>>(
        d_raw_prob, d_classes, d_scores, 
        d_tr_matrix, seg_width, seg_height, num_classes,
        empty_class, threshold);
    return cudaGetLastError();
}
