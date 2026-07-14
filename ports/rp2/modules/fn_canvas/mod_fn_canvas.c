/*
 * fn-vision Canvas 的 MicroPython RP2 原生加速模块。
 *
 * 所有颜色均按 LCD 使用的大端 RGB565 字节序写入，避免 Python 层逐列
 * 调用 FrameBuffer，并保持现有 Canvas.buffer 的内容格式不变。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "py/binary.h"
#include "py/obj.h"
#include "py/objtuple.h"
#include "py/runtime.h"
#include "py/unicode.h"

#ifndef FN_CANVAS_BUILTIN_FONTS
#define FN_CANVAS_BUILTIN_FONTS (0)
#endif

#if FN_CANVAS_BUILTIN_FONTS
#include "font_builtin_data.h"
#endif

#define FN_CANVAS_API_VERSION (FN_CANVAS_BUILTIN_FONTS ? 8 : 7)

#define FN_CANVAS_COMMAND_FILL_RECT (0)
#define FN_CANVAS_COMMAND_LINE (1)
#define FN_CANVAS_COMMAND_DRAW_RECT (2)

#define FN_CANVAS_FONT_WQY_8X16 (3)
#define FN_CANVAS_FONT_FUSION_PIXEL_8X16 (4)

#if FN_CANVAS_BUILTIN_FONTS
/** 判断字体编号是否对应编译进固件的双语点阵字体。 */
static bool fn_canvas_is_builtin_font(int font_kind) {
    return font_kind == FN_CANVAS_FONT_WQY_8X16
        || font_kind == FN_CANVAS_FONT_FUSION_PIXEL_8X16;
}

/** 从小端双字节字符索引读取一个 Unicode 基本平面码点。 */
static uint16_t fn_canvas_builtin_codepoint(uint32_t index) {
    const uint32_t offset = index * 2U;
    return (uint16_t)fn_builtin_font_codepoints[offset]
        | ((uint16_t)fn_builtin_font_codepoints[offset + 1U] << 8);
}

/** 通过二分查找返回内置字体字形序号，缺字时回退到问号。 */
static uint32_t fn_canvas_find_builtin_glyph(unichar codepoint) {
    uint32_t left = 0;
    uint32_t right = fn_builtin_font_glyph_count;
    while (left < right) {
        const uint32_t middle = left + (right - left) / 2U;
        const uint16_t current = fn_canvas_builtin_codepoint(middle);
        if (current < codepoint) {
            left = middle + 1U;
        } else {
            right = middle;
        }
    }
    if (left < fn_builtin_font_glyph_count
        && fn_canvas_builtin_codepoint(left) == codepoint) {
        return left;
    }
    /* 问号位于连续 ASCII 表内，索引等于码点减去空格码点。 */
    return (uint32_t)('?' - ' ');
}

/** 返回指定内置字体和 Unicode 码点对应的只读字形地址。 */
static const uint8_t *fn_canvas_builtin_glyph(int font_kind,
    unichar codepoint) {
    const uint32_t index = fn_canvas_find_builtin_glyph(codepoint);
    const uint8_t *font = font_kind == FN_CANVAS_FONT_WQY_8X16
        ? fn_builtin_font_wqy_bitmap : fn_builtin_font_fusion_bitmap;
    return font + index * FN_BUILTIN_FONT_GLYPH_BYTES;
}

/** 返回半角 ASCII 或全角中文字符的固定水平步进。 */
static int fn_canvas_builtin_advance(unichar codepoint) {
    return codepoint < 0x80 ? FN_BUILTIN_FONT_ASCII_ADVANCE
        : FN_BUILTIN_FONT_FULL_WIDTH_ADVANCE;
}
#endif

/** 获取可写画布缓冲区，并校验其容量。 */
static uint8_t *fn_canvas_get_buffer(mp_obj_t object, size_t required_size) {
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(object, &buffer, MP_BUFFER_WRITE);
    if (buffer.len < required_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("canvas buffer is too small"));
    }
    return (uint8_t *)buffer.buf;
}

/** 校验画布尺寸并计算 RGB565 缓冲区所需字节数，防止整数乘法溢出。 */
static size_t fn_canvas_buffer_size(int width, int height) {
    if (width <= 0 || height <= 0
        || (size_t)width > SIZE_MAX / (size_t)height / 2U) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid canvas size"));
    }
    return (size_t)width * (size_t)height * 2U;
}

/** 在已完成边界裁剪的本地坐标内写入 RGB565 实心矩形。 */
static void fn_canvas_fill_local(uint8_t *buffer, int canvas_width,
    int left, int top, int right, int bottom, uint16_t color) {
    const uint8_t high = (uint8_t)(color >> 8);
    const uint8_t low = (uint8_t)color;
    const size_t row_bytes = (size_t)(right - left) * 2;
    for (int y = top; y < bottom; ++y) {
        uint8_t *pixel = buffer + ((y * canvas_width + left) * 2);
        if (high == low) {
            memset(pixel, high, row_bytes);
            continue;
        }
        if (((uintptr_t)pixel & 3U) == 2U && row_bytes >= 2) {
            *pixel++ = high;
            *pixel++ = low;
        }
        const uint32_t pair = (uint32_t)high
            | ((uint32_t)low << 8)
            | ((uint32_t)high << 16)
            | ((uint32_t)low << 24);
        size_t remaining = row_bytes - (size_t)(pixel
            - (buffer + ((y * canvas_width + left) * 2)));
        if (((uintptr_t)pixel & 3U) == 0U) {
            uint32_t *wide_pixel = (uint32_t *)pixel;
            while (remaining >= 4) {
                *wide_pixel++ = pair;
                remaining -= 4;
            }
            pixel = (uint8_t *)wide_pixel;
        } else {
            /* bytearray 切片可能产生奇地址；RP2350 RISC-V 不允许非对齐字写入。 */
            while (remaining >= 2) {
                *pixel++ = high;
                *pixel++ = low;
                remaining -= 2;
            }
        }
        if (remaining >= 2) {
            pixel[0] = high;
            pixel[1] = low;
        }
    }
}

#if FN_CANVAS_BUILTIN_FONTS
/** 在当前裁剪视口内绘制一个固件内置十六像素字形。 */
static void fn_canvas_draw_builtin_glyph(uint8_t *buffer, int canvas_width,
    int canvas_height, int origin_x, int origin_y, int font_kind,
    unichar codepoint, int x, int y, uint16_t color, int scale) {
    const uint8_t *glyph = fn_canvas_builtin_glyph(font_kind, codepoint);
    const int glyph_width = fn_canvas_builtin_advance(codepoint);
    for (int row = 0; row < FN_BUILTIN_FONT_HEIGHT; ++row) {
        const uint16_t row_bits = ((uint16_t)glyph[row * 2] << 8)
            | glyph[row * 2 + 1];
        for (int column = 0; column < glyph_width; ++column) {
            if ((row_bits & (0x8000U >> column)) == 0) {
                continue;
            }
            int left = x + column * scale - origin_x;
            int top = y + row * scale - origin_y;
            int right = left + scale;
            int bottom = top + scale;
            left = left < 0 ? 0 : left;
            top = top < 0 ? 0 : top;
            right = right > canvas_width ? canvas_width : right;
            bottom = bottom > canvas_height ? canvas_height : bottom;
            if (left < right && top < bottom) {
                fn_canvas_fill_local(buffer, canvas_width, left, top,
                    right, bottom, color);
            }
        }
    }
}
#endif

/** 解析每个绘图入口共用的缓冲区、尺寸和原点参数。 */
static uint8_t *fn_canvas_parse_canvas(const mp_obj_t *arguments,
    int *canvas_width, int *canvas_height, int *origin_x, int *origin_y) {
    *canvas_width = mp_obj_get_int(arguments[1]);
    *canvas_height = mp_obj_get_int(arguments[2]);
    *origin_x = mp_obj_get_int(arguments[3]);
    *origin_y = mp_obj_get_int(arguments[4]);
    return fn_canvas_get_buffer(arguments[0],
        fn_canvas_buffer_size(*canvas_width, *canvas_height));
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

/** 使用与原 Canvas 一致的 Bresenham 规则绘制本地坐标线段。 */
static void fn_canvas_line_local(uint8_t *buffer, int width, int height,
    int x0, int y0, int x1, int y1, uint16_t color) {
    if (y0 == y1) {
        if ((unsigned int)y0 >= (unsigned int)height) {
            return;
        }
        int left = x0 < x1 ? x0 : x1;
        int right = x0 < x1 ? x1 : x0;
        left = left < 0 ? 0 : left;
        right = right >= width ? width - 1 : right;
        if (left <= right) {
            fn_canvas_fill_local(buffer, width, left, y0, right + 1, y0 + 1, color);
        }
        return;
    }
    if (x0 == x1) {
        if ((unsigned int)x0 >= (unsigned int)width) {
            return;
        }
        int top = y0 < y1 ? y0 : y1;
        int bottom = y0 < y1 ? y1 : y0;
        top = top < 0 ? 0 : top;
        bottom = bottom >= height ? height - 1 : bottom;
        if (top <= bottom) {
            fn_canvas_fill_local(buffer, width, x0, top, x0 + 1, bottom + 1, color);
        }
        return;
    }
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
}

/** 绘制已经换算为本地坐标并完成尺寸校验的指定粗细矩形边框。 */
static void fn_canvas_rect_local(uint8_t *buffer, int width, int height,
    int x, int y, int rect_width, int rect_height, uint16_t color,
    int thickness) {
    if (rect_width <= 0 || rect_height <= 0 || thickness <= 0) {
        return;
    }
    const int shorter_side = rect_width < rect_height
        ? rect_width : rect_height;
    const int maximum_thickness = shorter_side / 2 + shorter_side % 2;
    if (thickness > maximum_thickness) {
        thickness = maximum_thickness;
    }
    for (int inset = 0; inset < thickness; ++inset) {
        const int inset_width = rect_width - inset * 2;
        const int inset_height = rect_height - inset * 2;
        if (inset_width <= 0 || inset_height <= 0) {
            break;
        }
        fn_canvas_line_local(buffer, width, height, x + inset, y + inset,
            x + inset + inset_width - 1, y + inset, color);
        fn_canvas_line_local(buffer, width, height,
            x + inset, y + inset + inset_height - 1,
            x + inset + inset_width - 1, y + inset + inset_height - 1, color);
        fn_canvas_line_local(buffer, width, height, x + inset, y + inset,
            x + inset, y + inset + inset_height - 1, color);
        fn_canvas_line_local(buffer, width, height,
            x + inset + inset_width - 1, y + inset,
            x + inset + inset_width - 1, y + inset + inset_height - 1, color);
    }
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
    fn_canvas_line_local(buffer, width, height, x0, y0, x1, y1, color);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fn_canvas_line_obj, 10, 10, fn_canvas_line);

/** 一次绘制指定粗细的矩形边框，未传粗细时保持一像素兼容行为。 */
static mp_obj_t fn_canvas_draw_rect(size_t argument_count, const mp_obj_t *arguments) {
    int canvas_width, canvas_height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &canvas_width,
        &canvas_height, &origin_x, &origin_y);
    const int x = mp_obj_get_int(arguments[5]) - origin_x;
    const int y = mp_obj_get_int(arguments[6]) - origin_y;
    const int width = mp_obj_get_int(arguments[7]);
    const int height = mp_obj_get_int(arguments[8]);
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[9]);
    const int thickness = argument_count > 10
        ? mp_obj_get_int(arguments[10]) : 1;
    fn_canvas_rect_local(buffer, canvas_width, canvas_height,
        x, y, width, height, color, thickness);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_rect_obj, 10, 11, fn_canvas_draw_rect);

/** 一次调用绘制规则点阵网格，减少 Python 与 C 的跨层调用。 */
static mp_obj_t fn_canvas_draw_grid(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int canvas_width, canvas_height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &canvas_width,
        &canvas_height, &origin_x, &origin_y);
    const int x = mp_obj_get_int(arguments[5]);
    const int y = mp_obj_get_int(arguments[6]);
    const int width = mp_obj_get_int(arguments[7]);
    const int height = mp_obj_get_int(arguments[8]);
    const int step_x = mp_obj_get_int(arguments[9]);
    const int step_y = mp_obj_get_int(arguments[10]);
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[11]);
    if (step_x <= 0 || step_y <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("grid step must be positive"));
    }
    for (int point_x = x; point_x < x + width; point_x += step_x) {
        for (int point_y = y; point_y < y + height; point_y += step_y) {
            fn_canvas_pixel_local(buffer, canvas_width, canvas_height,
                point_x - origin_x, point_y - origin_y, color);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_grid_obj, 12, 12, fn_canvas_draw_grid);

/** 批量连接 Python 已计算好的坐标点，确保缩放和取整效果完全不变。 */
static mp_obj_t fn_canvas_draw_polyline(size_t argument_count, const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[6]);
    mp_obj_iter_buf_t iterator_buffer;
    mp_obj_t iterator = mp_getiter(arguments[5], &iterator_buffer);
    mp_obj_t item;
    bool has_previous = false;
    int previous_x = 0;
    int previous_y = 0;
    while ((item = mp_iternext(iterator)) != MP_OBJ_STOP_ITERATION) {
        size_t coordinate_count;
        mp_obj_t *coordinates;
        mp_obj_get_array(item, &coordinate_count, &coordinates);
        if (coordinate_count != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("point must contain x and y"));
        }
        const int current_x = mp_obj_get_int(coordinates[0]) - origin_x;
        const int current_y = mp_obj_get_int(coordinates[1]) - origin_y;
        if (has_previous) {
            fn_canvas_line_local(buffer, width, height,
                previous_x, previous_y, current_x, current_y, color);
        }
        previous_x = current_x;
        previous_y = current_y;
        has_previous = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_polyline_obj, 7, 7, fn_canvas_draw_polyline);

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
    uint8_t *buffer = fn_canvas_get_buffer(arguments[ARG_BUFFER],
        fn_canvas_buffer_size(canvas_width, canvas_height));

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

/**
 * 一次完成折线图点阵、缩放、插值、分区着色及实心填充。
 * Python 端仅传入图表定义与原始数据，避免创建坐标点和逐列调用。
 */
static mp_obj_t fn_canvas_draw_line_chart(size_t argument_count,
    const mp_obj_t *arguments) {
    enum {
        ARG_BUFFER, ARG_CANVAS_WIDTH, ARG_CANVAS_HEIGHT, ARG_ORIGIN_X,
        ARG_ORIGIN_Y, ARG_X, ARG_Y, ARG_WIDTH, ARG_HEIGHT, ARG_VALUES,
        ARG_MAXIMUM, ARG_COLOR, ARG_FILLED, ARG_REGIONS, ARG_GRID_STEP_X,
        ARG_GRID_STEP_Y, ARG_GRID_COLOR, ARG_COLOR_CALLBACK,
        ARG_COLOR_CACHE_STEP,
    };
    (void)argument_count;
    int canvas_width, canvas_height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &canvas_width,
        &canvas_height, &origin_x, &origin_y);
    const int chart_x = mp_obj_get_int(arguments[ARG_X]);
    const int chart_y = mp_obj_get_int(arguments[ARG_Y]);
    const int chart_width = mp_obj_get_int(arguments[ARG_WIDTH]);
    const int chart_height = mp_obj_get_int(arguments[ARG_HEIGHT]);
    if (chart_width <= 0 || chart_height <= 0) {
        return mp_const_none;
    }
    const int grid_step_x = mp_obj_get_int(arguments[ARG_GRID_STEP_X]);
    const int grid_step_y = mp_obj_get_int(arguments[ARG_GRID_STEP_Y]);
    const uint16_t grid_color = (uint16_t)mp_obj_get_int(arguments[ARG_GRID_COLOR]);
    if (grid_step_x > 0 && grid_step_y > 0) {
        for (int x = chart_x; x < chart_x + chart_width; x += grid_step_x) {
            for (int y = chart_y; y < chart_y + chart_height; y += grid_step_y) {
                fn_canvas_pixel_local(buffer, canvas_width, canvas_height,
                    x - origin_x, y - origin_y, grid_color);
            }
        }
    }

    size_t value_count;
    mp_obj_t *value_objects;
    mp_obj_get_array(arguments[ARG_VALUES], &value_count, &value_objects);
    if (value_count < 2) {
        return mp_const_none;
    }
    mp_float_t *chart_values = m_new(mp_float_t, value_count);
    for (size_t index = 0; index < value_count; ++index) {
        chart_values[index] = mp_obj_get_float(value_objects[index]);
    }
    mp_float_t maximum = mp_obj_get_float(arguments[ARG_MAXIMUM]);
    if (maximum <= 0) {
        maximum = 1;
        for (size_t index = 0; index < value_count; ++index) {
            const mp_float_t value = chart_values[index];
            if (value > maximum) {
                maximum = value;
            }
        }
    }
    const uint16_t default_color = (uint16_t)mp_obj_get_int(arguments[ARG_COLOR]);
    const bool filled = mp_obj_is_true(arguments[ARG_FILLED]);
    const mp_obj_t color_callback = arguments[ARG_COLOR_CALLBACK];
    const mp_float_t color_cache_step = mp_obj_get_float(
        arguments[ARG_COLOR_CACHE_STEP]);
    volatile mp_obj_t color_cache_object = mp_const_none;
    mp_map_t *color_cache = NULL;
    if (color_callback != mp_const_none && color_cache_step > 0) {
        color_cache_object = mp_obj_new_dict(0);
        color_cache = mp_obj_dict_get_map(color_cache_object);
    }
    size_t region_count = 0;
    mp_float_t *region_limits = NULL;
    uint16_t *region_colors = NULL;
    if (arguments[ARG_REGIONS] != mp_const_none) {
        mp_obj_t *region_objects;
        mp_obj_get_array(arguments[ARG_REGIONS], &region_count, &region_objects);
        region_limits = m_new(mp_float_t, region_count);
        region_colors = m_new(uint16_t, region_count);
        for (size_t index = 0; index < region_count; ++index) {
            size_t item_count;
            mp_obj_t *item_values;
            mp_obj_get_array(region_objects[index], &item_count, &item_values);
            if (item_count != 2) {
                mp_raise_ValueError(MP_ERROR_TEXT("region must contain limit and color"));
            }
            region_limits[index] = mp_obj_get_float(item_values[0]);
            region_colors[index] = (uint16_t)mp_obj_get_int(item_values[1]);
        }
    }
    const int local_bottom = chart_y + chart_height - 1 - origin_y;
    bool has_previous = false;
    int previous_x = 0;
    int previous_y = 0;
    for (int offset_x = 0; offset_x < chart_width; ++offset_x) {
        const size_t scaled = chart_width > 1
            ? (size_t)offset_x * (value_count - 1) : 0;
        const size_t divisor = chart_width > 1 ? (size_t)(chart_width - 1) : 1;
        size_t left_index = scaled / divisor;
        if (left_index >= value_count - 1) {
            left_index = value_count - 1;
        }
        const size_t right_index = left_index + 1 < value_count
            ? left_index + 1 : left_index;
        const size_t remainder = scaled % divisor;
        const mp_float_t left_value = chart_values[left_index];
        const mp_float_t right_value = chart_values[right_index];
        mp_float_t value = left_value + (right_value - left_value)
            * (mp_float_t)remainder / (mp_float_t)divisor;
        if (value < 0) {
            value = 0;
        }
        if (value > maximum) {
            value = maximum;
        }
        const int x = chart_x + offset_x - origin_x;
        const int y = chart_y + chart_height - 1 - origin_y
            - (int)(value * (chart_height - 1) / maximum);
        uint16_t color = default_color;
        if (color_callback != mp_const_none) {
            if (color_cache_step > 0) {
                const int cache_bucket = (int)(value / color_cache_step);
                const mp_obj_t cache_key = mp_obj_new_int(cache_bucket);
                mp_map_elem_t *cached = mp_map_lookup(
                    color_cache, cache_key, MP_MAP_LOOKUP);
                if (cached == NULL) {
                    const mp_obj_t callback_value = mp_obj_new_float(value);
                    color = (uint16_t)mp_obj_get_int(mp_call_function_1(
                        color_callback, callback_value));
                    cached = mp_map_lookup(
                        color_cache, cache_key,
                        MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
                    cached->value = mp_obj_new_int_from_uint(color);
                } else {
                    color = (uint16_t)mp_obj_get_int(cached->value);
                }
            } else {
                color = (uint16_t)mp_obj_get_int(mp_call_function_1(
                    color_callback, mp_obj_new_float(value)));
            }
        } else {
            for (size_t index = 0; index < region_count; ++index) {
                if (value < region_limits[index]) {
                    color = region_colors[index];
                    break;
                }
            }
        }
        if (filled) {
            int top = y < 0 ? 0 : y;
            int bottom = local_bottom >= canvas_height
                ? canvas_height - 1 : local_bottom;
            if (x >= 0 && x < canvas_width && top <= bottom && bottom >= 0
                && top < canvas_height) {
                fn_canvas_fill_local(buffer, canvas_width, x, top, x + 1,
                    bottom + 1, color);
            }
        } else if (has_previous) {
            fn_canvas_line_local(buffer, canvas_width, canvas_height,
                previous_x, previous_y, x, y, color);
        } else {
            fn_canvas_pixel_local(buffer, canvas_width, canvas_height, x, y, color);
        }
        previous_x = x;
        previous_y = y;
        has_previous = true;
    }
    if (region_count > 0) {
        m_del(uint16_t, region_colors, region_count);
        m_del(mp_float_t, region_limits, region_count);
    }
    m_del(mp_float_t, chart_values, value_count);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_line_chart_obj, 19, 19, fn_canvas_draw_line_chart);

/** 使用 Python 字体列数据或固件内置字体绘制整段透明背景文字。 */
static mp_obj_t fn_canvas_draw_text(size_t argument_count,
    const mp_obj_t *arguments) {
    enum {
        ARG_BUFFER, ARG_CANVAS_WIDTH, ARG_CANVAS_HEIGHT, ARG_ORIGIN_X,
        ARG_ORIGIN_Y, ARG_FONT, ARG_FONT_KIND, ARG_X, ARG_Y, ARG_VALUE,
        ARG_COLOR, ARG_SCALE,
    };
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    const int font_kind = mp_obj_get_int(arguments[ARG_FONT_KIND]);
    int cursor_x = mp_obj_get_int(arguments[ARG_X]);
    const int text_y = mp_obj_get_int(arguments[ARG_Y]);
    const uint16_t color = (uint16_t)mp_obj_get_int(arguments[ARG_COLOR]);
    const int scale = mp_obj_get_int(arguments[ARG_SCALE]);
    if (scale <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("text scale must be positive"));
    }
#if FN_CANVAS_BUILTIN_FONTS
    if (fn_canvas_is_builtin_font(font_kind)) {
        size_t text_length;
        const byte *cursor = mp_obj_str_get_data(
            arguments[ARG_VALUE], &text_length);
        const byte *end = cursor + text_length;
        while (cursor < end) {
            const unichar codepoint = utf8_get_char(cursor);
            fn_canvas_draw_builtin_glyph(buffer, width, height,
                origin_x, origin_y, font_kind, codepoint,
                cursor_x, text_y, color, scale);
            cursor_x += fn_canvas_builtin_advance(codepoint) * scale;
            cursor = utf8_next_char(cursor);
        }
        return mp_const_none;
    }
#endif
    mp_map_t *font = mp_obj_dict_get_map(arguments[ARG_FONT]);
    const mp_obj_t fallback_character = mp_obj_new_str("?", 1);
    mp_obj_iter_buf_t iterator_buffer;
    mp_obj_t iterator = mp_getiter(arguments[ARG_VALUE], &iterator_buffer);
    mp_obj_t character;
    while ((character = mp_iternext(iterator)) != MP_OBJ_STOP_ITERATION) {
        mp_map_elem_t *glyph_entry = mp_map_lookup(font, character, MP_MAP_LOOKUP);
        if (glyph_entry == NULL) {
            glyph_entry = mp_map_lookup(font, fallback_character, MP_MAP_LOOKUP);
        }
        if (glyph_entry == NULL) {
            continue;
        }
        size_t column_count;
        mp_obj_t *columns;
        mp_obj_get_array(glyph_entry->value, &column_count, &columns);
        const int offset = font_kind == 1 && scale == 1 ? 1 : 0;
        for (size_t column = 0; column < column_count; ++column) {
            const int bits = mp_obj_get_int(columns[column]);
            for (int row = 0; row < 7; ++row) {
                if ((bits & (1 << row)) != 0) {
                    int left = cursor_x + offset + (int)column * scale - origin_x;
                    int top = text_y + row * scale - origin_y;
                    int right = left + scale;
                    int bottom = top + scale;
                    left = left < 0 ? 0 : left;
                    top = top < 0 ? 0 : top;
                    right = right > width ? width : right;
                    bottom = bottom > height ? height : bottom;
                    if (left < right && top < bottom) {
                        fn_canvas_fill_local(buffer, width, left, top,
                            right, bottom, color);
                    }
                }
            }
        }
        int advance = (int)column_count + 1;
        if (font_kind == 0 || (font_kind == 1 && scale > 1)) {
            advance = advance < 6 ? 6 : advance;
        } else if (font_kind == 1 && scale == 1) {
            advance = advance < 8 ? 8 : advance;
        }
        cursor_x += advance * scale;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_text_obj, 12, 12, fn_canvas_draw_text);

#if FN_CANVAS_BUILTIN_FONTS
/** 计算固件内置字体字符串在指定缩放倍数下的像素宽度。 */
static mp_obj_t fn_canvas_text_width(mp_obj_t font_kind_object,
    mp_obj_t value_object, mp_obj_t scale_object) {
    const int font_kind = mp_obj_get_int(font_kind_object);
    const int scale = mp_obj_get_int(scale_object);
    if (!fn_canvas_is_builtin_font(font_kind)) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown builtin font"));
    }
    if (scale <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("text scale must be positive"));
    }
    size_t text_length;
    const byte *cursor = mp_obj_str_get_data(value_object, &text_length);
    const byte *end = cursor + text_length;
    mp_int_t width = 0;
    while (cursor < end) {
        width += fn_canvas_builtin_advance(utf8_get_char(cursor)) * scale;
        cursor = utf8_next_char(cursor);
    }
    return mp_obj_new_int(width);
}
static MP_DEFINE_CONST_FUN_OBJ_3(
    fn_canvas_text_width_obj, fn_canvas_text_width);

/** 返回固件内置字形的逐列位图，供 Python Canvas 兼容后端使用。 */
static mp_obj_t fn_canvas_font_glyph(mp_obj_t font_kind_object,
    mp_obj_t character_object) {
    const int font_kind = mp_obj_get_int(font_kind_object);
    if (!fn_canvas_is_builtin_font(font_kind)) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown builtin font"));
    }
    size_t character_length;
    const byte *character = mp_obj_str_get_data(
        character_object, &character_length);
    if (character_length == 0
        || utf8_next_char(character) != character + character_length) {
        mp_raise_ValueError(MP_ERROR_TEXT("font glyph requires one character"));
    }
    const unichar codepoint = utf8_get_char(character);
    const uint8_t *glyph = fn_canvas_builtin_glyph(font_kind, codepoint);
    const size_t glyph_width = (size_t)fn_canvas_builtin_advance(codepoint);
    mp_obj_tuple_t *columns = MP_OBJ_TO_PTR(mp_obj_new_tuple(glyph_width, NULL));
    for (size_t column = 0; column < glyph_width; ++column) {
        uint16_t bits = 0;
        for (int row = 0; row < FN_BUILTIN_FONT_HEIGHT; ++row) {
            const uint16_t row_bits = ((uint16_t)glyph[row * 2] << 8)
                | glyph[row * 2 + 1];
            if ((row_bits & (0x8000U >> column)) != 0) {
                bits |= (uint16_t)(1U << row);
            }
        }
        columns->items[column] = MP_OBJ_NEW_SMALL_INT(bits);
    }
    return MP_OBJ_FROM_PTR(columns);
}
static MP_DEFINE_CONST_FUN_OBJ_2(
    fn_canvas_font_glyph_obj, fn_canvas_font_glyph);
#endif

/** 一次解析并执行矩形填充、线段和矩形边框命令序列。 */
static mp_obj_t fn_canvas_draw_commands(size_t argument_count,
    const mp_obj_t *arguments) {
    (void)argument_count;
    int width, height, origin_x, origin_y;
    uint8_t *buffer = fn_canvas_parse_canvas(arguments, &width, &height,
        &origin_x, &origin_y);
    mp_obj_iter_buf_t iterator_buffer;
    mp_obj_t iterator = mp_getiter(arguments[5], &iterator_buffer);
    mp_obj_t command;
    while ((command = mp_iternext(iterator)) != MP_OBJ_STOP_ITERATION) {
        size_t count;
        mp_obj_t *values;
        mp_obj_get_array(command, &count, &values);
        if (count != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("draw command must contain 6 values"));
        }
        const int operation = mp_obj_get_int(values[0]);
        const int x = mp_obj_get_int(values[1]) - origin_x;
        const int y = mp_obj_get_int(values[2]) - origin_y;
        const int value_a = mp_obj_get_int(values[3]);
        const int value_b = mp_obj_get_int(values[4]);
        const uint16_t color = (uint16_t)mp_obj_get_int(values[5]);
        if (operation == FN_CANVAS_COMMAND_FILL_RECT) {
            int left = x < 0 ? 0 : x;
            int top = y < 0 ? 0 : y;
            int right = x + value_a;
            int bottom = y + value_b;
            right = right > width ? width : right;
            bottom = bottom > height ? height : bottom;
            if (left < right && top < bottom) {
                fn_canvas_fill_local(buffer, width, left, top, right, bottom, color);
            }
        } else if (operation == FN_CANVAS_COMMAND_LINE) {
            fn_canvas_line_local(buffer, width, height, x, y,
                value_a - origin_x, value_b - origin_y, color);
        } else if (operation == FN_CANVAS_COMMAND_DRAW_RECT) {
            fn_canvas_rect_local(buffer, width, height,
                x, y, value_a, value_b, color, 1);
        } else {
            mp_raise_ValueError(MP_ERROR_TEXT("unknown draw command"));
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_canvas_draw_commands_obj, 6, 6, fn_canvas_draw_commands);

static const mp_rom_map_elem_t fn_canvas_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_canvas)},
    {MP_ROM_QSTR(MP_QSTR_API_VERSION), MP_ROM_INT(FN_CANVAS_API_VERSION)},
    {MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_canvas_api_version_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&fn_canvas_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&fn_canvas_pixel_obj)},
    {MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&fn_canvas_fill_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&fn_canvas_line_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_rect), MP_ROM_PTR(&fn_canvas_draw_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_grid), MP_ROM_PTR(&fn_canvas_draw_grid_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_polyline), MP_ROM_PTR(&fn_canvas_draw_polyline_obj)},
    {MP_ROM_QSTR(MP_QSTR_fill_polygon), MP_ROM_PTR(&fn_canvas_fill_polygon_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_columns), MP_ROM_PTR(&fn_canvas_draw_columns_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_line_chart), MP_ROM_PTR(&fn_canvas_draw_line_chart_obj)},
    {MP_ROM_QSTR(MP_QSTR_draw_text), MP_ROM_PTR(&fn_canvas_draw_text_obj)},
#if FN_CANVAS_BUILTIN_FONTS
    {MP_ROM_QSTR(MP_QSTR_text_width), MP_ROM_PTR(&fn_canvas_text_width_obj)},
    {MP_ROM_QSTR(MP_QSTR_font_glyph), MP_ROM_PTR(&fn_canvas_font_glyph_obj)},
#endif
    {MP_ROM_QSTR(MP_QSTR_draw_commands), MP_ROM_PTR(&fn_canvas_draw_commands_obj)},
};
static MP_DEFINE_CONST_DICT(fn_canvas_module_globals, fn_canvas_module_globals_table);

const mp_obj_module_t fn_canvas_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&fn_canvas_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_canvas, fn_canvas_user_cmodule);
