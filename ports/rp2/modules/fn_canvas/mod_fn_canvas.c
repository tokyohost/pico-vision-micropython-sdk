/*
 * fn-vision Canvas 的 MicroPython RP2 原生加速模块。
 *
 * 所有颜色均按 LCD 使用的大端 RGB565 字节序写入，避免 Python 层逐列
 * 调用 FrameBuffer，并保持现有 Canvas.buffer 的内容格式不变。
 */

#include <stdbool.h>
#include <stdint.h>

#include "py/binary.h"
#include "py/obj.h"
#include "py/runtime.h"

#define FN_CANVAS_API_VERSION (2)

/** 获取可写画布缓冲区，并校验其容量。 */
static uint8_t *fn_canvas_get_buffer(mp_obj_t object, size_t required_size) {
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(object, &buffer, MP_BUFFER_WRITE);
    if (buffer.len < required_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("canvas buffer is too small"));
    }
    return (uint8_t *)buffer.buf;
}

/** 在已完成边界裁剪的本地坐标内写入 RGB565 实心矩形。 */
static void fn_canvas_fill_local(uint8_t *buffer, int canvas_width,
    int left, int top, int right, int bottom, uint16_t color) {
    const uint8_t high = (uint8_t)(color >> 8);
    const uint8_t low = (uint8_t)color;
    for (int y = top; y < bottom; ++y) {
        uint8_t *pixel = buffer + ((y * canvas_width + left) * 2);
        for (int x = left; x < right; ++x) {
            *pixel++ = high;
            *pixel++ = low;
        }
    }
}

/** 解析每个绘图入口共用的缓冲区、尺寸和原点参数。 */
static uint8_t *fn_canvas_parse_canvas(const mp_obj_t *arguments,
    int *canvas_width, int *canvas_height, int *origin_x, int *origin_y) {
    *canvas_width = mp_obj_get_int(arguments[1]);
    *canvas_height = mp_obj_get_int(arguments[2]);
    *origin_x = mp_obj_get_int(arguments[3]);
    *origin_y = mp_obj_get_int(arguments[4]);
    if (*canvas_width <= 0 || *canvas_height <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid canvas size"));
    }
    return fn_canvas_get_buffer(arguments[0],
        (size_t)*canvas_width * *canvas_height * 2);
}

/** 在画布边界内写入单个本地坐标像素。 */
static inline void fn_canvas_pixel_local(uint8_t *buffer, int canvas_width,
    int canvas_height, int x, int y, uint16_t color) {
    if ((unsigned int)x >= (unsigned int)canvas_width
        || (unsigned int)y >= (unsigned int)canvas_height) {
        return;
    }
    uint8_t *pixel = buffer + ((y * canvas_width + x) * 2);
    pixel[0] = (uint8_t)(color >> 8);
    pixel[1] = (uint8_t)color;
}

/** 返回模块能力版本，供 Python 在运行时判断当前 UF2 是否支持加速。 */
static mp_obj_t fn_canvas_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(FN_CANVAS_API_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_canvas_api_version_obj, fn_canvas_api_version);

/** 使用指定 RGB565 颜色清空整个当前画布视口。 */
static mp_obj_t fn_canvas_clear(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    (void)origin_x;
    (void)origin_y;
    fn_canvas_fill_local(buffer, width, 0, 0, width, height,
        (uint16_t)mp_obj_get_int(arguments[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fn_canvas_clear_obj, 6, 6, fn_canvas_clear);

/** 绘制经过当前视口裁剪的单个像素。 */
static mp_obj_t fn_canvas_pixel(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    fn_canvas_pixel_local(buffer, width, height,
        mp_obj_get_int(arguments[5]) - origin_x,
        mp_obj_get_int(arguments[6]) - origin_y,
        (uint16_t)mp_obj_get_int(arguments[7]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fn_canvas_pixel_obj, 8, 8, fn_canvas_pixel);

/** 绘制经过当前视口裁剪的实心矩形。 */
static mp_obj_t fn_canvas_fill_rect(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int canvas_width, canvas_height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &canvas_width,
        &canvas_height, &origin_x, &origin_y);
    int left = mp_obj_get_int(arguments[5]) - origin_x;
    int top = mp_obj_get_int(arguments[6]) - origin_y;
    int right = left + mp_obj_get_int(arguments[7]);
    int bottom = top + mp_obj_get_int(arguments[8]);
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > canvas_width) {
        right = canvas_width;
    }
    if (bottom > canvas_height) {
        bottom = canvas_height;
    }
    if (left < right && top < bottom) {
        fn_canvas_fill_local(buffer, canvas_width, left, top, right, bottom,
            (uint16_t)mp_obj_get_int(arguments[9]));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_fill_rect_obj, 10, 10, fn_canvas_fill_rect);

/** 使用整数 Bresenham 算法绘制线段，并逐点执行视口裁剪。 */
static mp_obj_t fn_canvas_line(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    int x0 = mp_obj_get_int(arguments[5]) - origin_x;
    int y0 = mp_obj_get_int(arguments[6]) - origin_y;
    const int x1 = mp_obj_get_int(arguments[7]) - origin_x;
    const int y1 = mp_obj_get_int(arguments[8]) - origin_y;
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[9]);
    const int delta_x = x0 < x1 ? x1 - x0 : x0 - x1;
    const int step_x = x0 < x1 ? 1 : -1;
    const int delta_y_abs = y0 < y1 ? y1 - y0 : y0 - y1;
    const int delta_y = -delta_y_abs;
    const int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;
    for (;;) {
        fn_canvas_pixel_local(buffer, width, height, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = error * 2;
        if (doubled >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fn_canvas_line_obj, 10, 10, fn_canvas_line);

/** 使用偶奇扫描线规则填充任意简单多边形。 */
static mp_obj_t fn_canvas_fill_polygon(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    size_t point_count;
    mp_obj_t *points;
    mp_obj_get_array(arguments[5], &point_count, &points);
    if (point_count < 3) {
        return mp_const_false;
    }
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[6]);
    int *point_x = m_new(int, point_count);
    int *point_y = m_new(int, point_count);
    int *intersections = m_new(int, point_count);
    for (size_t index = 0; index < point_count; ++index) {
        size_t coordinate_count;
        mp_obj_t *coordinates;
        mp_obj_get_array(points[index], &coordinate_count, &coordinates);
        if (coordinate_count != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("point must contain x and y"));
        }
        point_x[index] = mp_obj_get_int(coordinates[0]) - origin_x;
        point_y[index] = mp_obj_get_int(coordinates[1]) - origin_y;
    }
    for (int scan_y = 0; scan_y < height; ++scan_y) {
        size_t intersection_count = 0;
        size_t previous = point_count - 1;
        for (size_t current = 0; current < point_count; ++current) {
            if ((point_y[current] > scan_y) != (point_y[previous] > scan_y)) {
                const int64_t numerator = (int64_t)(point_x[previous] - point_x[current])
                    * (scan_y - point_y[current]);
                intersections[intersection_count++] = point_x[current]
                    + (int)(numerator / (point_y[previous] - point_y[current]));
            }
            previous = current;
        }
        for (size_t left = 1; left < intersection_count; ++left) {
            const int value = intersections[left];
            size_t position = left;
            while (position > 0 && intersections[position - 1] > value) {
                intersections[position] = intersections[position - 1];
                --position;
            }
            intersections[position] = value;
        }
        for (size_t index = 0; index + 1 < intersection_count; index += 2) {
            int left = intersections[index];
            int right = intersections[index + 1] + 1;
            if (left < 0) {
                left = 0;
            }
            if (right > width) {
                right = width;
            }
            if (left < right) {
                fn_canvas_fill_local(buffer, width, left, scan_y, right,
                    scan_y + 1, color);
            }
        }
    }
    m_del(int, intersections, point_count);
    m_del(int, point_y, point_count);
    m_del(int, point_x, point_count);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_fill_polygon_obj, 7, 7, fn_canvas_fill_polygon);

/** 批量绘制采样列；成功时返回真，参数不适用时抛出明确异常。 */
static mp_obj_t fn_canvas_draw_columns(size_t argument_count, const mp_obj_t *arguments) {
    enum {
        ARG_BUFFER, ARG_CANVAS_WIDTH, ARG_CANVAS_HEIGHT, ARG_ORIGIN_X,
        ARG_ORIGIN_Y, ARG_COLUMNS, ARG_BOTTOM,
    };
    (void)argument_count;
    const int canvas_width = mp_obj_get_int(arguments[ARG_CANVAS_WIDTH]);
    const int canvas_height = mp_obj_get_int(arguments[ARG_CANVAS_HEIGHT]);
    const int origin_x = mp_obj_get_int(arguments[ARG_ORIGIN_X]);
    const int origin_y = mp_obj_get_int(arguments[ARG_ORIGIN_Y]);
    uint8_t *buffer = fn_canvas_get_buffer(
        arguments[ARG_BUFFER], (size_t)canvas_width * canvas_height * 2);

    const bool has_bottom = arguments[ARG_BOTTOM] != mp_const_none;
    const int bottom = has_bottom ? mp_obj_get_int(arguments[ARG_BOTTOM]) : 0;
    mp_obj_iter_buf_t iterator_buffer;
    mp_obj_t iterator = mp_getiter(arguments[ARG_COLUMNS], &iterator_buffer);
    mp_obj_t item;
    while ((item = mp_iternext(iterator)) != MP_OBJ_STOP_ITERATION) {
        size_t item_count;
        mp_obj_t *values;
        mp_obj_get_array(item, &item_count, &values);
        if (item_count != 3) {
            mp_raise_ValueError(MP_ERROR_TEXT("column must contain x, y and color"));
        }
        const int x = mp_obj_get_int(values[0]) - origin_x;
        int top = mp_obj_get_int(values[1]) - origin_y;
        int local_bottom = has_bottom ? bottom - origin_y : top;
        if (x < 0 || x >= canvas_width || local_bottom < 0 || top >= canvas_height) {
            continue;
        }
        if (top < 0) {
            top = 0;
        }
        if (local_bottom >= canvas_height) {
            local_bottom = canvas_height - 1;
        }
        if (top > local_bottom) {
            continue;
        }
        fn_canvas_fill_local(buffer, canvas_width, x, top, x + 1,
            local_bottom + 1, (uint16_t)mp_obj_get_int(values[2]));
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_columns_obj, 7, 7, fn_canvas_draw_columns);

static const mp_rom_map_elem_t fn_canvas_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_canvas)},
    {MP_ROM_QSTR(MP_QSTR_API_VERSION), MP_ROM_INT(FN_CANVAS_API_VERSION)},
    {MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_canvas_api_version_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&fn_canvas_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&fn_canvas_pixel_obj)},
    {MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&fn_canvas_fill_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&fn_canvas_line_obj)},
    {MP_ROM_QSTR(MP_QSTR_fill_polygon), MP_ROM_PTR(&fn_canvas_fill_polygon_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_columns), MP_ROM_PTR(&fn_canvas_draw_columns_obj)},
};
static MP_DEFINE_CONST_DICT(fn_canvas_module_globals, fn_canvas_module_globals_table);

const mp_obj_module_t fn_canvas_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&fn_canvas_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_canvas, fn_canvas_user_cmodule);
