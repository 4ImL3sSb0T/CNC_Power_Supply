/**
 * @file vec_math.c
 * @brief 向量数学工具函数库实现 - 完整版
 *
 * 提供高效的 2D/3D 向量运算，针对路径规划和导航模块优化
 * 支持 CMSIS-DSP 硬件加速
 *
 * @version 2.0
 * @date 2025-01-14
 */
#include "vec_math.h"
#include <math.h>
#include <string.h>

/* ========================================================================
 * 数学常量
 * ======================================================================== */

#define PI_F        3.14159265358979323846f
#define TWO_PI_F    6.28318530717958647692f
#define HALF_PI_F   1.57079632679489661923f

/* ========================================================================
 * 基础向量运算 - 2D
 * ======================================================================== */

/**
 * @brief 向量加法: c = a + b
 */
vec2f_t vec2f_add(vec2f_t a, vec2f_t b) {
#ifdef USE_CMSIS_DSP
    vec2f_t result;
    arm_add_f32((const f32*)&a, (const f32*)&b, (f32*)&result, 2);
    return result;
#else
    vec2f_t result = {a.x + b.x, a.y + b.y};
    return result;
#endif
}

/**
 * @brief 向量减法: c = a - b
 */
vec2f_t vec2f_sub(vec2f_t a, vec2f_t b) {
#ifdef USE_CMSIS_DSP
    vec2f_t result;
    arm_sub_f32((const f32*)&a, (const f32*)&b, (f32*)&result, 2);
    return result;
#else
    vec2f_t result = {a.x - b.x, a.y - b.y};
    return result;
#endif
}

/**
 * @brief 标量乘法: c = v * s
 */
vec2f_t vec2f_scale(vec2f_t v, f32 scalar) {
#ifdef USE_CMSIS_DSP
    vec2f_t result;
    arm_scale_f32((const f32*)&v, scalar, (f32*)&result, 2);
    return result;
#else
    vec2f_t result = {v.x * scalar, v.y * scalar};
    return result;
#endif
}

/**
 * @brief 标量除法: c = v / s
 */
vec2f_t vec2f_divide(vec2f_t v, f32 scalar) {
    // 避免除以零
    if (scalar == 0.0f) {
        return v;
    }
    f32 inv = 1.0f / scalar;
    return vec2f_scale(v, inv);
}

/**
 * @brief 点积: a · b = |a||b|cos(θ)
 */
f32 vec2f_dot(vec2f_t a, vec2f_t b) {
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_dot_prod_f32((const f32*)&a, (const f32*)&b, 2, &result);
    return result;
#else
    return a.x * b.x + a.y * b.y;
#endif
}

/**
 * @brief 2D 叉积（返回标量，即 z 分量）
 *
 * a × b = ax*by - ay*bx
 */
f32 vec2f_cross(vec2f_t a, vec2f_t b) {
    return a.x * b.y - a.y * b.x;
}

/**
 * @brief 向量长度的平方（避免 sqrt，性能优化）
 */
f32 vec2f_length_squared(vec2f_t v) {
    return v.x * v.x + v.y * v.y;
}

/**
 * @brief 向量长度（欧氏范数）
 */
f32 vec2f_length(vec2f_t v) {
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_sqrt_f32(vec2f_length_squared(v), &result);
    return result;
#else
    return sqrtf(vec2f_length_squared(v));
#endif
}

/**
 * @brief 向量归一化（单位向量）
 */
vec2f_t vec2f_normalize(vec2f_t v) {
    f32 len_sq = vec2f_length_squared(v);

    // 零向量直接返回
    if (len_sq < 1e-8f) {
        return v;
    }

#ifdef USE_CMSIS_DSP
    f32 inv_len;
    arm_sqrt_f32(len_sq, &inv_len);
    inv_len = 1.0f / inv_len;
    return vec2f_scale(v, inv_len);
#else
    f32 inv_len = 1.0f / sqrtf(len_sq);
    return vec2f_scale(v, inv_len);
#endif
}

/**
 * @brief 安全归一化（提供默认值）
 */
vec2f_t vec2f_normalize_safe(vec2f_t v, vec2f_t fallback) {
    f32 len_sq = vec2f_length_squared(v);

    if (len_sq < 1e-8f) {
        return fallback;
    }

#ifdef USE_CMSIS_DSP
    f32 inv_len;
    arm_sqrt_f32(len_sq, &inv_len);
    inv_len = 1.0f / inv_len;
    return vec2f_scale(v, inv_len);
#else
    f32 inv_len = 1.0f / sqrtf(len_sq);
    return vec2f_scale(v, inv_len);
#endif
}

/**
 * @brief 限制向量长度
 */
vec2f_t vec2f_clamp_length(vec2f_t v, f32 max_length) {
    f32 len_sq = vec2f_length_squared(v);

    if (len_sq <= max_length * max_length) {
        return v;
    }

#ifdef USE_CMSIS_DSP
    f32 ratio;
    arm_sqrt_f32(len_sq / (max_length * max_length), &ratio);
    return vec2f_scale(v, 1.0f / ratio);
#else
    f32 ratio = sqrtf(len_sq) / max_length;
    return vec2f_scale(v, 1.0f / ratio);
#endif
}

/* ========================================================================
 * 基础向量运算 - 3D
 * ======================================================================== */

/**
 * @brief 3D 向量加法: c = a + b
 */
vec3f vec3f_add(vec3f a, vec3f b) {
    vec3f result;
#ifdef USE_CMSIS_DSP
    arm_add_f32((const f32*)&a, (const f32*)&b, (f32*)&result, 3);
#else
    result.x = a.x + b.x;
    result.y = a.y + b.y;
    result.z = a.z + b.z;
#endif
    return result;
}

/**
 * @brief 3D 向量减法: c = a - b
 */
vec3f vec3f_sub(vec3f a, vec3f b) {
    vec3f result;
#ifdef USE_CMSIS_DSP
    arm_sub_f32((const f32*)&a, (const f32*)&b, (f32*)&result, 3);
#else
    result.x = a.x - b.x;
    result.y = a.y - b.y;
    result.z = a.z - b.z;
#endif
    return result;
}

/**
 * @brief 3D 标量乘法: c = v * s
 */
vec3f vec3f_scale(vec3f v, f32 scalar) {
    vec3f result;
#ifdef USE_CMSIS_DSP
    arm_scale_f32((const f32*)&v, scalar, (f32*)&result, 3);
#else
    result.x = v.x * scalar;
    result.y = v.y * scalar;
    result.z = v.z * scalar;
#endif
    return result;
}

/**
 * @brief 3D 点积
 */
f32 vec3f_dot(vec3f a, vec3f b) {
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_dot_prod_f32((const f32*)&a, (const f32*)&b, 3, &result);
    return result;
#else
    return a.x * b.x + a.y * b.y + a.z * b.z;
#endif
}

/**
 * @brief 3D 叉积: c = a × b
 */
vec3f vec3f_cross(vec3f a, vec3f b) {
    vec3f result;
    result.x = a.y * b.z - a.z * b.y;
    result.y = a.z * b.x - a.x * b.z;
    result.z = a.x * b.y - a.y * b.x;
    return result;
}

/**
 * @brief 3D 向量长度
 */
f32 vec3f_length(vec3f v) {
    f32 len_sq = v.x * v.x + v.y * v.y + v.z * v.z;
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_sqrt_f32(len_sq, &result);
    return result;
#else
    return sqrtf(len_sq);
#endif
}

/**
 * @brief 3D 向量归一化
 */
vec3f vec3f_normalize(vec3f v) {
    f32 len_sq = v.x * v.x + v.y * v.y + v.z * v.z;

    if (len_sq < 1e-8f) {
        return v;
    }

#ifdef USE_CMSIS_DSP
    f32 inv_len;
    arm_sqrt_f32(len_sq, &inv_len);
    inv_len = 1.0f / inv_len;
    return vec3f_scale(v, inv_len);
#else
    f32 inv_len = 1.0f / sqrtf(len_sq);
    return vec3f_scale(v, inv_len);
#endif
}

/* ========================================================================
 * 距离计算
 * ======================================================================== */

/**
 * @brief 计算两个向量的 2D 平面距离的平方（忽略 z 轴）
 */
f32 vec2f_distance_squared(vec3f a, vec3f b) {
    f32 dx = a.x - b.x;
    f32 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/**
 * @brief 计算两个向量的 2D 平面距离（忽略 z 轴）
 */
f32 vec2f_distance(vec3f a, vec3f b) {
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_sqrt_f32(vec2f_distance_squared(a, b), &result);
    return result;
#else
    return sqrtf(vec2f_distance_squared(a, b));
#endif
}

/**
 * @brief 计算两个 2D 向量的距离
 */
f32 vec2f_distance_v(vec2f_t a, vec2f_t b) {
    vec2f_t diff = vec2f_sub(a, b);
    return vec2f_length(diff);
}

/**
 * @brief 计算两个向量的 3D 欧氏距离的平方
 */
f32 vec3f_distance_squared(vec3f a, vec3f b) {
    f32 dx = a.x - b.x;
    f32 dy = a.y - b.y;
    f32 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

/**
 * @brief 计算两个向量的 3D 欧氏距离
 */
f32 vec3f_distance(vec3f a, vec3f b) {
#ifdef USE_CMSIS_DSP
    f32 result;
    arm_sqrt_f32(vec3f_distance_squared(a, b), &result);
    return result;
#else
    return sqrtf(vec3f_distance_squared(a, b));
#endif
}

/* ========================================================================
 * 角度与方向
 * ======================================================================== */

/**
 * @brief 计算向量的角度（弧度）
 *
 * 返回值范围：(-π, π]
 */
f32 vec2f_angle(vec2f_t v) {
    // CMSIS-DSP 没有 atan2 函数，使用标准库
    return atan2f(v.y, v.x);
}

/**
 * @brief 从角度创建单位向量
 */
vec2f_t vec2f_from_angle(f32 angle_rad) {
    vec2f_t result;
#ifdef USE_CMSIS_DSP
    arm_sin_cos_f32(angle_rad, &result.y, &result.x);
#else
    result.x = cosf(angle_rad);
    result.y = sinf(angle_rad);
#endif
    return result;
}

/**
 * @brief 计算角度差（最短路径）
 *
 * 返回值范围：(-π, π]
 */
f32 vec2f_angle_diff(f32 target, f32 current) {
    f32 diff = target - current;

    // 归一化到 [-π, π]
    while (diff > PI_F) {
        diff -= TWO_PI_F;
    }
    while (diff < -PI_F) {
        diff += TWO_PI_F;
    }

    return diff;
}

/**
 * @brief 限制角度到范围 [-π, π)
 */
f32 vec2f_normalize_angle(f32 angle) {
    while (angle >= PI_F) {
        angle -= TWO_PI_F;
    }
    while (angle < -PI_F) {
        angle += TWO_PI_F;
    }
    return angle;
}

/**
 * @brief 向量旋转
 */
vec2f_t vec2f_rotate(vec2f_t v, f32 angle_rad) {
    f32 sin_a, cos_a;

#ifdef USE_CMSIS_DSP
    arm_sin_cos_f32(angle_rad, &sin_a, &cos_a);
#else
    sin_a = sinf(angle_rad);
    cos_a = cosf(angle_rad);
#endif

    vec2f_t result;
    result.x = v.x * cos_a - v.y * sin_a;
    result.y = v.x * sin_a + v.y * cos_a;

    return result;
}

/**
 * @brief 向量旋转（使用旋转矩阵）
 */
vec2f_t vec2f_rotate_matrix(vec2f_t v, const mat2x2_t* mat) {
    vec2f_t result;
    result.x = mat->m[0][0] * v.x + mat->m[0][1] * v.y;
    result.y = mat->m[1][0] * v.x + mat->m[1][1] * v.y;
    return result;
}

/**
 * @brief 计算两个向量的夹角
 */
f32 vec2f_angle_between(vec2f_t a, vec2f_t b) {
    f32 dot = vec2f_dot(a, b);
    f32 len_a = vec2f_length(a);
    f32 len_b = vec2f_length(b);

    if (len_a < 1e-8f || len_b < 1e-8f) {
        return 0.0f;
    }

    f32 cos_angle = dot / (len_a * len_b);

    // 钳制到 [-1, 1] 避免数值误差
    if (cos_angle > 1.0f) cos_angle = 1.0f;
    if (cos_angle < -1.0f) cos_angle = -1.0f;

    // 使用标准库 acos 函数
    return acosf(cos_angle);
}

/**
 * @brief 判断目标方向是在当前方向的左侧还是右侧
 */
f32 vec2f_turn_direction(f32 current, f32 target) {
    return vec2f_angle_diff(target, current);
}

/* ========================================================================
 * 批量处理（多目标场景）
 * ======================================================================== */

/**
 * @brief 批量计算 2D 距离平方
 */
void vec2f_distance_squared_batch(
    const vec2f_t* points,
    vec2f_t target,
    f32* outputs,
    u32 count
) {
    for (u32 i = 0; i < count; i++) {
        vec2f_t diff = vec2f_sub(points[i], target);
        outputs[i] = vec2f_length_squared(diff);
    }
}

/**
 * @brief 批量计算 3D 距离平方
 */
void vec3f_distance_squared_batch(
    const vec3f* points,
    vec3f target,
    f32* outputs,
    u32 count
) {
    for (u32 i = 0; i < count; i++) {
        vec3f diff = vec3f_sub(points[i], target);
        outputs[i] = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
    }
}

/**
 * @brief 查找最近点（KNN 邻近搜索）
 */
u32 vec2f_find_nearest(const vec2f_t* points, vec2f_t target, u32 count) {
    if (count == 0) {
        return 0;
    }

    u32 nearest_idx = 0;
    f32 min_dist_sq = vec2f_length_squared(vec2f_sub(points[0], target));

    for (u32 i = 1; i < count; i++) {
        f32 dist_sq = vec2f_length_squared(vec2f_sub(points[i], target));
        if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

/**
 * @brief 查找最远点
 */
u32 vec2f_find_farthest(const vec2f_t* points, vec2f_t target, u32 count) {
    if (count == 0) {
        return 0;
    }

    u32 farthest_idx = 0;
    f32 max_dist_sq = vec2f_length_squared(vec2f_sub(points[0], target));

    for (u32 i = 1; i < count; i++) {
        f32 dist_sq = vec2f_length_squared(vec2f_sub(points[i], target));
        if (dist_sq > max_dist_sq) {
            max_dist_sq = dist_sq;
            farthest_idx = i;
        }
    }

    return farthest_idx;
}

/**
 * @brief 批量点积计算
 */
void vec2f_dot_batch(
    const vec2f_t* vectors_a,
    const vec2f_t* vectors_b,
    f32* outputs,
    u32 count
) {
#ifdef USE_CMSIS_DSP
    for (u32 i = 0; i < count; i++) {
        arm_dot_prod_f32(
            (const f32*)&vectors_a[i],
            (const f32*)&vectors_b[i],
            2,
            &outputs[i]
        );
    }
#else
    for (u32 i = 0; i < count; i++) {
        outputs[i] = vec2f_dot(vectors_a[i], vectors_b[i]);
    }
#endif
}

/* ========================================================================
 * 几何工具
 * ======================================================================== */

/**
 * @brief 点到直线的距离
 */
f32 vec2f_distance_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end) {
    return fabsf(vec2f_signed_distance_to_line(point, line_start, line_end));
}

/**
 * @brief 点到直线的投影点
 */
vec2f_t vec2f_project_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end) {
    vec2f_t line_vec = vec2f_sub(line_end, line_start);
    vec2f_t point_vec = vec2f_sub(point, line_start);

    f32 line_len_sq = vec2f_length_squared(line_vec);

    if (line_len_sq < 1e-8f) {
        return line_start;  // 线段退化为点
    }

    // 投影参数 t = (point · line) / |line|^2
    f32 t = vec2f_dot(point_vec, line_vec) / line_len_sq;

    // 投影点 = start + t * line_vec
    return vec2f_add(line_start, vec2f_scale(line_vec, t));
}

/**
 * @brief 点到线段的最近点
 */
vec2f_t vec2f_closest_point_on_segment(vec2f_t point, line_segment_t segment) {
    vec2f_t line_vec = vec2f_sub(segment.end, segment.start);
    vec2f_t point_vec = vec2f_sub(point, segment.start);

    f32 line_len_sq = vec2f_length_squared(line_vec);

    if (line_len_sq < 1e-8f) {
        return segment.start;
    }

    // 投影参数 t = (point · line) / |line|^2
    f32 t = vec2f_dot(point_vec, line_vec) / line_len_sq;

    // 钳制到 [0, 1]
    if (t < 0.0f) {
        return segment.start;
    } else if (t > 1.0f) {
        return segment.end;
    }

    // 投影点在线段上
    return vec2f_add(segment.start, vec2f_scale(line_vec, t));
}

/**
 * @brief 点到线段的距离
 */
f32 vec2f_distance_to_segment(vec2f_t point, line_segment_t segment) {
    vec2f_t closest = vec2f_closest_point_on_segment(point, segment);
    return vec2f_length(vec2f_sub(point, closest));
}

/**
 * @brief 判断点是否在线段上（带容差）
 */
bool vec2f_is_point_on_segment(vec2f_t point, line_segment_t segment, f32 tolerance) {
    f32 dist = vec2f_distance_to_segment(point, segment);
    return dist <= tolerance;
}

/**
 * @brief 计算点到直线的有向距离
 *
 * 正值表示在直线左侧，负值表示在右侧
 */
f32 vec2f_signed_distance_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end) {
    // 使用叉积计算有向距离
    // d = (line × point) / |line|
    vec2f_t line_vec = vec2f_sub(line_end, line_start);
    vec2f_t point_vec = vec2f_sub(point, line_start);

    f32 cross = vec2f_cross(line_vec, point_vec);
    f32 line_len = vec2f_length(line_vec);

    if (line_len < 1e-8f) {
        return 0.0f;
    }

    return cross / line_len;
}

/**
 * @brief 两个圆的碰撞检测
 */
bool vec2f_circle_collision(circle_t a, circle_t b) {
    f32 dx = a.center.x - b.center.x;
    f32 dy = a.center.y - b.center.y;
    f32 dist_sq = dx * dx + dy * dy;
    f32 radius_sum = a.radius + b.radius;
    return dist_sq <= (radius_sum * radius_sum);
}

/* ========================================================================
 * 矩阵运算
 * ======================================================================== */

/**
 * @brief 创建 2D 旋转矩阵
 */
mat2x2_t mat2x2_rotation(f32 angle_rad) {
    mat2x2_t mat;
    f32 sin_a, cos_a;

#ifdef USE_CMSIS_DSP
    arm_sin_cos_f32(angle_rad, &sin_a, &cos_a);
#else
    sin_a = sinf(angle_rad);
    cos_a = cosf(angle_rad);
#endif

    mat.m[0][0] = cos_a;  mat.m[0][1] = -sin_a;
    mat.m[1][0] = sin_a;  mat.m[1][1] = cos_a;

    return mat;
}

/**
 * @brief 矩阵向量乘法: v' = M * v
 */
vec2f_t mat2x2_transform(const mat2x2_t* mat, vec2f_t v) {
    vec2f_t result;
    result.x = mat->m[0][0] * v.x + mat->m[0][1] * v.y;
    result.y = mat->m[1][0] * v.x + mat->m[1][1] * v.y;
    return result;
}

/**
 * @brief 矩阵乘法: C = A * B
 */
mat2x2_t mat2x2_multiply(const mat2x2_t* a, const mat2x2_t* b) {
    mat2x2_t result;

    result.m[0][0] = a->m[0][0] * b->m[0][0] + a->m[0][1] * b->m[1][0];
    result.m[0][1] = a->m[0][0] * b->m[0][1] + a->m[0][1] * b->m[1][1];
    result.m[1][0] = a->m[1][0] * b->m[0][0] + a->m[1][1] * b->m[1][0];
    result.m[1][1] = a->m[1][0] * b->m[0][1] + a->m[1][1] * b->m[1][1];

    return result;
}

/**
 * @brief 矩阵转置
 */
mat2x2_t mat2x2_transpose(const mat2x2_t* mat) {
    mat2x2_t result;

    result.m[0][0] = mat->m[0][0];
    result.m[0][1] = mat->m[1][0];
    result.m[1][0] = mat->m[0][1];
    result.m[1][1] = mat->m[1][1];

    return result;
}

/* ========================================================================
 * 路径规划辅助函数
 * ======================================================================== */

/**
 * @brief Pure Pursuit 算法：计算 lookahead 点
 */
lookahead_result_t vec2f_pure_pursuit_lookahead(
    vec2f_t current_pos,
    const vec2f_t* path_points,
    u32 path_size,
    f32 lookahead_distance,
    u32 start_index
) {
    lookahead_result_t result = {{0, 0}, 0};

    if (path_size < 2) {
        return result;
    }

    // 从 start_index 开始搜索，避免重复计算
    for (u32 i = start_index; i < path_size - 1; i++) {
        vec2f_t segment_start = path_points[i];
        vec2f_t segment_end = path_points[i + 1];

        // 计算点到线段的最近点
        vec2f_t closest = vec2f_closest_point_on_segment(current_pos, (line_segment_t){segment_start, segment_end});
        f32 dist = vec2f_length(vec2f_sub(current_pos, closest));

        if (dist >= lookahead_distance) {
            // 找到满足条件的线段
            vec2f_t line_vec = vec2f_sub(segment_end, segment_start);
            f32 line_len = vec2f_length(line_vec);

            if (line_len < 1e-8f) {
                result.point = segment_start;
                result.index = i;
                return result;
            }

            // 解二次方程找到 lookahead 点
            vec2f_t to_current = vec2f_sub(current_pos, segment_start);
            f32 t = vec2f_dot(to_current, line_vec) / (line_len * line_len);

            // 计算 lookahead 点到最近点的距离
            f32 dist_to_closest = lookahead_distance - dist;

            // 沿线段方向移动
            f32 offset = dist_to_closest / line_len;
            f32 t_lookahead = t + offset;

            // 钳制到线段
            if (t_lookahead < 0.0f) t_lookahead = 0.0f;
            if (t_lookahead > 1.0f) t_lookahead = 1.0f;

            result.point = vec2f_add(segment_start, vec2f_scale(line_vec, t_lookahead));
            result.index = i;
            return result;
        }
    }

    // 未找到合适的点，返回路径终点
    result.point = path_points[path_size - 1];
    result.index = path_size - 1;
    return result;
}

/**
 * @brief 计算点到路径的最小距离和最近点索引
 */
path_distance_result_t vec2f_distance_to_path(
    vec2f_t point,
    const vec2f_t* path_points,
    u32 path_size
) {
    path_distance_result_t result = {0, 0};

    if (path_size == 0) {
        return result;
    }

    f32 min_dist = vec2f_length(vec2f_sub(point, path_points[0]));
    u32 min_idx = 0;

    for (u32 i = 0; i < path_size - 1; i++) {
        line_segment_t segment = {path_points[i], path_points[i + 1]};
        f32 dist = vec2f_distance_to_segment(point, segment);

        if (dist < min_dist) {
            min_dist = dist;
            min_idx = i;
        }
    }

    result.distance = min_dist;
    result.index = min_idx;
    return result;
}

/**
 * @brief 计算路径总长度
 */
f32 vec2f_path_length(const vec2f_t* path_points, u32 path_size) {
    if (path_size < 2) {
        return 0.0f;
    }

    f32 total_length = 0.0f;
    for (u32 i = 0; i < path_size - 1; i++) {
        total_length += vec2f_length(vec2f_sub(path_points[i + 1], path_points[i]));
    }

    return total_length;
}

/* ========================================================================
 * 插值与平滑
 * ======================================================================== */

/**
 * @brief 线性插值
 */
vec2f_t vec2f_lerp(vec2f_t a, vec2f_t b, f32 t) {
    vec2f_t diff = vec2f_sub(b, a);
    return vec2f_add(a, vec2f_scale(diff, t));
}

/**
 * @brief 平滑插值（带速度限制）
 */
vec2f_t vec2f_smooth_damp(vec2f_t current, vec2f_t target, f32 max_delta) {
    vec2f_t diff = vec2f_sub(target, current);
    f32 dist = vec2f_length(diff);

    if (dist <= max_delta) {
        return target;
    }

    return vec2f_add(current, vec2f_scale(vec2f_normalize(diff), max_delta));
}

/* ========================================================================
 * 调试与工具
 * ======================================================================== */

/**
 * @brief 将向量转换为字符串（用于调试输出）
 */
void vec2f_to_string(vec2f_t v, char* buffer, u32 buffer_size) {
    if (buffer_size < 32) {
        if (buffer_size > 0) {
            buffer[0] = '\0';
        }
        return;
    }

    // 使用简单的格式化
    // 注意：这里避免使用 sprintf 以减少依赖
    // 实际使用时可以根据需要启用 sprintf
    sprintf(buffer, "(%.2f, %.2f)", v.x, v.y);
}

/**
 * @brief 计算向量的哈希值（用于快速比较）
 */
u32 vec2f_hash(vec2f_t v) {
    // 简单的哈希函数
    u32 hx = (u32)(v.x * 1000.0f);  // 保留 3 位小数精度
    u32 hy = (u32)(v.y * 1000.0f);
    return (hx << 16) | (hy & 0xFFFF);
}
