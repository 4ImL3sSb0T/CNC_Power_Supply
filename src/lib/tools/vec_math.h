/**
 * @file vec_math.h
 * @brief 向量数学工具函数库 - 完整版
 *
 * 提供高效的 2D/3D 向量运算，针对路径规划和导航模块优化
 * 支持 CMSIS-DSP 硬件加速
 *
 * @version 2.0
 * @date 2025-01-14
 */
#ifndef __VEC_MATH_H__
#define __VEC_MATH_H__

#include "common_def.h"
#include <stdbool.h>

#ifdef USE_CMSIS_DSP
    #include "arm_math.h"
#endif

/* ========================================================================
 * 基础类型定义（保留原有类型以兼容）
 * ======================================================================== */

/**
 * @brief 通用三维向量
 *
 * 语义约定（全工程统一）：
 * 1) 用作位姿时：x 为世界坐标右向，y 为世界坐标前向，z 为航向角（度）。
 * 2) 用作速度时：x 为右向速度，y 为前向速度，z 为角速度 wz（逆时针为正）。
 */
typedef struct vec3f
{
    float x;
    float y;
    float z;
} vec3f;

typedef struct vec3i
{
    int32 x, y, z;
} vec3i;

typedef struct vec3u
{
    uint32 x, y, z;
} vec3u;

/* ========================================================================
 * 扩展数据结构定义
 * ======================================================================== */

/**
 * @brief 2D 向量（平面运算，节省内存）
 */
typedef struct {
    f32 x;
    f32 y;
} vec2f_t;

/**
 * @brief 2D 线段
 */
typedef struct {
    vec2f_t start;
    vec2f_t end;
} line_segment_t;

/**
 * @brief 2D 圆（碰撞检测）
 */
typedef struct {
    vec2f_t center;
    f32 radius;
} circle_t;

/**
 * @brief 2D 轴对齐包围盒（AABB）
 */
typedef struct {
    vec2f_t min;
    vec2f_t max;
} aabb_2d_t;

/**
 * @brief 2D 旋转矩阵（2x2，用于坐标变换）
 */
typedef struct {
    f32 m[2][2];  // [row][col]
} mat2x2_t;

/**
 * @brief 3D 旋转矩阵（3x3，用于姿态变换）
 */
typedef struct {
    f32 m[3][3];  // [row][col]
} mat3x3_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * 基础向量运算 - 2D
 * ======================================================================== */

/**
 * @brief 创建 2D 向量
 */
static inline vec2f_t vec2f_make(f32 x, f32 y) {
    vec2f_t v = {x, y};
    return v;
}

/**
 * @brief 向量加法: c = a + b
 */
vec2f_t vec2f_add(vec2f_t a, vec2f_t b);

/**
 * @brief 向量减法: c = a - b
 */
vec2f_t vec2f_sub(vec2f_t a, vec2f_t b);

/**
 * @brief 标量乘法: c = v * s
 */
vec2f_t vec2f_scale(vec2f_t v, f32 scalar);

/**
 * @brief 标量除法: c = v / s
 */
vec2f_t vec2f_divide(vec2f_t v, f32 scalar);

/**
 * @brief 点积: a · b = |a||b|cos(θ)
 *
 * 应用：
 * - 投影长度计算
 * - 角度计算（cosθ = dot / (|a||b|)）
 * - 垂直判断（dot ≈ 0）
 */
f32 vec2f_dot(vec2f_t a, vec2f_t b);

/**
 * @brief 2D 叉积（返回标量，即 z 分量）
 *
 * a × b = ax*by - ay*bx
 *
 * 应用：
 * - 正值：b 在 a 逆时针方向（左侧）
 * - 负值：b 在 a 顺时针方向（右侧）
 * - 零：共线
 */
f32 vec2f_cross(vec2f_t a, vec2f_t b);

/**
 * @brief 向量长度的平方（避免 sqrt，性能优化）
 */
f32 vec2f_length_squared(vec2f_t v);

/**
 * @brief 向量长度（欧氏范数）
 */
f32 vec2f_length(vec2f_t v);

/**
 * @brief 向量归一化（单位向量）
 *
 * @return 归一化后的向量；如果长度为零则返回原向量
 */
vec2f_t vec2f_normalize(vec2f_t v);

/**
 * @brief 安全归一化（提供默认值）
 *
 * @param v 输入向量
 * @param fallback 当长度为零时的默认返回值
 */
vec2f_t vec2f_normalize_safe(vec2f_t v, vec2f_t fallback);

/**
 * @brief 限制向量长度
 *
 * @param v 输入向量
 * @param max_length 最大长度
 * @return 如果 v.length() > max_length，返回缩放后的向量；否则返回原向量
 */
vec2f_t vec2f_clamp_length(vec2f_t v, f32 max_length);

/**
 * @brief 向量取反
 */
static inline vec2f_t vec2f_negate(vec2f_t v) {
    return vec2f_make(-v.x, -v.y);
}

/**
 * @brief 判断向量是否为零
 */
static inline bool vec2f_is_zero(vec2f_t v) {
    return (v.x == 0.0f) && (v.y == 0.0f);
}

/**
 * @brief 判断两个向量是否相等
 */
static inline bool vec2f_equals(vec2f_t a, vec2f_t b) {
    return (a.x == b.x) && (a.y == b.y);
}

/* ========================================================================
 * 基础向量运算 - 3D
 * ======================================================================== */

/**
 * @brief 3D 向量加法: c = a + b
 */
vec3f vec3f_add(vec3f a, vec3f b);

/**
 * @brief 3D 向量减法: c = a - b
 */
vec3f vec3f_sub(vec3f a, vec3f b);

/**
 * @brief 3D 标量乘法: c = v * s
 */
vec3f vec3f_scale(vec3f v, f32 scalar);

/**
 * @brief 3D 点积
 */
f32 vec3f_dot(vec3f a, vec3f b);

/**
 * @brief 3D 叉积: c = a × b
 */
vec3f vec3f_cross(vec3f a, vec3f b);

/**
 * @brief 3D 向量长度
 */
f32 vec3f_length(vec3f v);

/**
 * @brief 3D 向量归一化
 */
vec3f vec3f_normalize(vec3f v);

/* ========================================================================
 * 距离计算
 * ======================================================================== */

/**
 * @brief 计算两个向量的 2D 平面距离的平方（忽略 z 轴）
 *
 * 这是最优化的版本，避免了昂贵的 sqrt() 调用
 * 适用于距离比较和阈值判断场景
 *
 * @param a 向量 a
 * @param b 向量 b
 * @return f32 距离的平方 (mm^2)
 */
f32 vec2f_distance_squared(vec3f a, vec3f b);

/**
 * @brief 计算两个向量的 2D 平面距离（忽略 z 轴）
 *
 * 适用于需要实际距离值的场景（显示、日志）
 *
 * @param a 向量 a
 * @param b 向量 b
 * @return f32 实际距离 (mm)
 */
f32 vec2f_distance(vec3f a, vec3f b);

/**
 * @brief 计算两个 2D 向量的距离
 */
f32 vec2f_distance_v(vec2f_t a, vec2f_t b);

/**
 * @brief 计算两个向量的 3D 欧氏距离的平方
 *
 * 包含 x, y, z 三个维度
 *
 * @param a 向量 a
 * @param b 向量 b
 * @return f32 距离的平方 (mm^2)
 */
f32 vec3f_distance_squared(vec3f a, vec3f b);

/**
 * @brief 计算两个向量的 3D 欧氏距离
 *
 * 包含 x, y, z 三个维度
 *
 * @param a 向量 a
 * @param b 向量 b
 * @return f32 实际距离 (mm)
 */
f32 vec3f_distance(vec3f a, vec3f b);

/* ========================================================================
 * 角度与方向
 * ======================================================================== */

/**
 * @brief 计算向量的角度（弧度）
 *
 * 返回值范围：(-π, π]
 *
 * @param v 输入向量
 * @return f32 角度（弧度），从 +X 轴逆时针测量
 */
f32 vec2f_angle(vec2f_t v);

/**
 * @brief 从角度创建单位向量
 *
 * @param angle_rad 角度（弧度）
 * @return vec2f_t 单位向量
 */
vec2f_t vec2f_from_angle(f32 angle_rad);

/**
 * @brief 计算角度差（最短路径）
 *
 * 返回值范围：(-π, π]
 * 正值表示 target 在 current 逆时针方向
 *
 * @param target 目标角度
 * @param current 当前角度
 * @return f32 角度差（弧度）
 */
f32 vec2f_angle_diff(f32 target, f32 current);

/**
 * @brief 限制角度到范围 [-π, π)
 *
 * @param angle 输入角度（任意值）
 * @return f32 归一化后的角度
 */
f32 vec2f_normalize_angle(f32 angle);

/**
 * @brief 向量旋转
 *
 * @param v 输入向量
 * @param angle_rad 旋转角度（弧度，正值表示逆时针）
 * @return vec2f_t 旋转后的向量
 */
vec2f_t vec2f_rotate(vec2f_t v, f32 angle_rad);

/**
 * @brief 向量旋转（使用旋转矩阵）
 */
vec2f_t vec2f_rotate_matrix(vec2f_t v, const mat2x2_t* mat);

/**
 * @brief 计算两个向量的夹角
 *
 * @param a 向量 a
 * @param b 向量 b
 * @return f32 夹角（弧度），范围 [0, π]
 */
f32 vec2f_angle_between(vec2f_t a, vec2f_t b);

/**
 * @brief 判断目标方向是在当前方向的左侧还是右侧
 *
 * @param current 当前朝向（角度，弧度）
 * @param target 目标朝向（角度，弧度）
 * @return f32 角度差：正值表示左侧，负值表示右侧
 */
f32 vec2f_turn_direction(f32 current, f32 target);

/* ========================================================================
 * 批量处理（多目标场景）
 * ======================================================================== */

/**
 * @brief 批量计算 2D 距离平方
 *
 * @param points 点数组
 * @param target 目标点
 * @param outputs 输出数组（需预先分配）
 * @param count 点数量
 */
void vec2f_distance_squared_batch(
    const vec2f_t* points,
    vec2f_t target,
    f32* outputs,
    u32 count
);

/**
 * @brief 批量计算 3D 距离平方
 */
void vec3f_distance_squared_batch(
    const vec3f* points,
    vec3f target,
    f32* outputs,
    u32 count
);

/**
 * @brief 查找最近点（KNN 邻近搜索）
 *
 * @param points 点数组
 * @param target 目标点
 * @param count 点数量
 * @return u32 最近点的索引；如果 count 为 0 则返回 0
 */
u32 vec2f_find_nearest(const vec2f_t* points, vec2f_t target, u32 count);

/**
 * @brief 查找最远点
 */
u32 vec2f_find_farthest(const vec2f_t* points, vec2f_t target, u32 count);

/**
 * @brief 批量点积计算
 */
void vec2f_dot_batch(
    const vec2f_t* vectors_a,
    const vec2f_t* vectors_b,
    f32* outputs,
    u32 count
);

/* ========================================================================
 * 几何工具
 * ======================================================================== */

/**
 * @brief 点到直线的距离
 *
 * @param point 待测点
 * @param line_start 直线起点
 * @param line_end 直线终点
 * @return f32 垂直距离
 */
f32 vec2f_distance_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end);

/**
 * @brief 点到直线的投影点
 *
 * @param point 待测点
 * @param line_start 直线起点
 * @param line_end 直线终点
 * @return vec2f_t 投影点坐标
 */
vec2f_t vec2f_project_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end);

/**
 * @brief 点到线段的最近点
 *
 * 考虑线段端点限制，返回线段上或延长线上的最近点
 *
 * @param point 待测点
 * @param segment 线段
 * @return vec2f_t 最近点
 */
vec2f_t vec2f_closest_point_on_segment(vec2f_t point, line_segment_t segment);

/**
 * @brief 点到线段的距离
 */
f32 vec2f_distance_to_segment(vec2f_t point, line_segment_t segment);

/**
 * @brief 判断点是否在线段上（带容差）
 *
 * @param point 待测点
 * @param segment 线段
 * @param tolerance 容差（默认 1.0 mm）
 * @return bool 是否在线段上
 */
bool vec2f_is_point_on_segment(vec2f_t point, line_segment_t segment, f32 tolerance);

/**
 * @brief 计算点到直线的有向距离
 *
 * 正值表示在直线左侧，负值表示在右侧
 *
 * @param point 待测点
 * @param line_start 直线起点
 * @param line_end 直线终点
 * @return f32 有向距离
 */
f32 vec2f_signed_distance_to_line(vec2f_t point, vec2f_t line_start, vec2f_t line_end);

/**
 * @brief 判断点是否在圆内
 */
static inline bool vec2f_is_point_in_circle(vec2f_t point, circle_t circle) {
    f32 dx = point.x - circle.center.x;
    f32 dy = point.y - circle.center.y;
    return (dx * dx + dy * dy) <= (circle.radius * circle.radius);
}

/**
 * @brief 判断点是否在 AABB 内
 */
static inline bool vec2f_is_point_in_aabb(vec2f_t point, aabb_2d_t aabb) {
    return (point.x >= aabb.min.x && point.x <= aabb.max.x &&
            point.y >= aabb.min.y && point.y <= aabb.max.y);
}

/**
 * @brief 两个圆的碰撞检测
 */
bool vec2f_circle_collision(circle_t a, circle_t b);

/**
 * @brief 两个 AABB 的碰撞检测
 */
static inline bool vec2f_aabb_collision(aabb_2d_t a, aabb_2d_t b) {
    return (a.min.x < b.max.x && a.max.x > b.min.x &&
            a.min.y < b.max.y && a.max.y > b.min.y);
}

/* ========================================================================
 * 矩阵运算
 * ======================================================================== */

/**
 * @brief 创建 2D 旋转矩阵
 *
 * @param angle_rad 旋转角度（弧度）
 * @return mat2x2_t 旋转矩阵
 */
mat2x2_t mat2x2_rotation(f32 angle_rad);

/**
 * @brief 矩阵向量乘法: v' = M * v
 */
vec2f_t mat2x2_transform(const mat2x2_t* mat, vec2f_t v);

/**
 * @brief 矩阵乘法: C = A * B
 */
mat2x2_t mat2x2_multiply(const mat2x2_t* a, const mat2x2_t* b);

/**
 * @brief 矩阵转置
 */
mat2x2_t mat2x2_transpose(const mat2x2_t* mat);

/* ========================================================================
 * 路径规划辅助函数
 * ======================================================================== */

/**
 * @brief Pure Pursuit 算法：计算 lookahead 点
 *
 * @param current_pos 当前位置
 * @param path_points 路径点数组
 * @param path_size 路径点数量
 * @param lookahead_distance 前瞻距离
 * @param start_index 开始搜索的索引（可传入上次返回值以加速）
 * @return struct { vec2f_t point; u32 index; } lookahead 点和索引
 */
typedef struct {
    vec2f_t point;
    u32 index;
} lookahead_result_t;

lookahead_result_t vec2f_pure_pursuit_lookahead(
    vec2f_t current_pos,
    const vec2f_t* path_points,
    u32 path_size,
    f32 lookahead_distance,
    u32 start_index
);

/**
 * @brief 计算点到路径的最小距离和最近点索引
 *
 * @param point 待测点
 * @param path_points 路径点数组
 * @param path_size 路径点数量
 * @return struct { f32 distance; u32 index; } 最小距离和对应索引
 */
typedef struct {
    f32 distance;
    u32 index;
} path_distance_result_t;

path_distance_result_t vec2f_distance_to_path(
    vec2f_t point,
    const vec2f_t* path_points,
    u32 path_size
);

/**
 * @brief 计算路径总长度
 *
 * @param path_points 路径点数组
 * @param path_size 路径点数量
 * @return f32 路径总长度
 */
f32 vec2f_path_length(const vec2f_t* path_points, u32 path_size);

/* ========================================================================
 * 插值与平滑
 * ======================================================================== */

/**
 * @brief 线性插值
 *
 * @param a 起点
 * @param b 终点
 * @param t 插值参数 [0, 1]
 * @return vec2f_t 插值点
 */
vec2f_t vec2f_lerp(vec2f_t a, vec2f_t b, f32 t);

/**
 * @brief 平滑插值（带速度限制）
 *
 * @param current 当前值
 * @param target 目标值
 * @param max_delta 最大变化量
 * @return vec2f_t 平滑后的值
 */
vec2f_t vec2f_smooth_damp(vec2f_t current, vec2f_t target, f32 max_delta);

/* ========================================================================
 * 调试与工具
 * ======================================================================== */

/**
 * @brief 将向量转换为字符串（用于调试输出）
 *
 * @param v 输入向量
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 */
void vec2f_to_string(vec2f_t v, char* buffer, u32 buffer_size);

/**
 * @brief 计算向量的哈希值（用于快速比较）
 */
u32 vec2f_hash(vec2f_t v);

#ifdef __cplusplus
}
#endif

#endif // __VEC_MATH_H__
