/*
 * fn-vision 固件内置双语点阵字体数据声明。
 *
 * 字形统一使用十六行、每行十六位的高位优先格式；ASCII 使用左侧八列，
 * GB2312 中文及全角符号使用完整十六列。
 */

#ifndef FN_CANVAS_FONT_BUILTIN_DATA_H
#define FN_CANVAS_FONT_BUILTIN_DATA_H

#include <stdint.h>

#define FN_BUILTIN_FONT_HEIGHT (16)
#define FN_BUILTIN_FONT_GLYPH_BYTES (32)
#define FN_BUILTIN_FONT_ASCII_ADVANCE (8)
#define FN_BUILTIN_FONT_FULL_WIDTH_ADVANCE (16)

extern const uint32_t fn_builtin_font_glyph_count;
extern const uint8_t fn_builtin_font_codepoints[];
extern const uint8_t fn_builtin_font_wqy_bitmap[];
extern const uint8_t fn_builtin_font_fusion_bitmap[];

#endif
