#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "lib/tools/vec_math.h"

static void check_point(lookahead_result_t r, float x, float y, unsigned index) {
    assert(fabsf(r.point.x - x) < 0.001f);
    assert(fabsf(r.point.y - y) < 0.001f);
    assert(r.index == index);
}

int main(void) {
    const vec2f_t straight[] = {{0, 0}, {100, 0}};
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, straight, 2, 10, 0), 10, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){50, 0}, straight, 2, 10, 0), 60, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 6}, straight, 2, 10, 0), 8, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){50, 10}, straight, 2, 10, 0), 50, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 20}, straight, 2, 10, 0), 100, 0, 1);
    const vec2f_t bend[] = {{0, 0}, {5, 0}, {5, 20}};
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, bend, 3, 10, 0), 5, sqrtf(75), 1);
    const vec2f_t repeated[] = {{0, 0}, {0, 0}, {100, 0}};
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, repeated, 3, 10, 0), 10, 0, 1);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, straight, 2, 10, 1), 100, 0, 1);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, bend, 1, 10, 0), 0, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, NULL, 2, 10, 0), 0, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, straight, 0, 10, 0), 0, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, straight, 2, 0, 0), 0, 0, 0);
    check_point(vec2f_pure_pursuit_lookahead((vec2f_t){0, 0}, straight, 2, NAN, 0), 0, 0, 0);

    const float half_pi = 1.5707963267948966f;
    const vec2f_t unit = vec2f_from_angle(half_pi);
    const vec2f_t rotated = vec2f_rotate((vec2f_t){1, 0}, half_pi);
    const mat2x2_t matrix = mat2x2_rotation(half_pi);
    const vec2f_t by_matrix = vec2f_rotate_matrix((vec2f_t){1, 0}, &matrix);
    assert(fabsf(unit.x) < 0.001f && fabsf(unit.y - 1) < 0.001f);
    assert(fabsf(rotated.x) < 0.001f && fabsf(rotated.y - 1) < 0.001f);
    assert(fabsf(by_matrix.x) < 0.001f && fabsf(by_matrix.y - 1) < 0.001f);
#ifdef USE_CMSIS_DSP
    puts("PASS: geometry and radians (actual CMSIS-DSP)");
#else
    puts("PASS: geometry and radians (standard math)");
#endif
    return 0;
}
