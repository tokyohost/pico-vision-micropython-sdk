/*
 * fn-vision PV1 帧解析与 CRC-16/CCITT-FALSE 原生加速模块。
 *
 * 模块只负责无状态的帧校验和载荷提取，串口接收、JSON 解压与业务分发仍由
 * Python 管理，从而在旧 UF2 中保持完全兼容的 Python 回退路径。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "py/binary.h"
#include "py/obj.h"
#include "py/runtime.h"

#define FN_PROTOCOL_API_VERSION (1)

/** 计算一段字节的 CRC-16/CCITT-FALSE，并允许延续已有 CRC。 */
static uint16_t fn_protocol_crc16_update(uint16_t crc, const uint8_t *data,
    size_t length) {
    for (size_t index = 0; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0
                ? (uint16_t)((crc << 1) ^ 0x1021U)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/** 解析非负十进制字段，拒绝空字段、符号和非数字字符。 */
static bool fn_protocol_parse_decimal(const uint8_t *data, size_t length,
    size_t *value) {
    if (length == 0) {
        return false;
    }
    size_t result = 0;
    for (size_t index = 0; index < length; ++index) {
        if (data[index] < '0' || data[index] > '9') {
            return false;
        }
        const size_t digit = (size_t)(data[index] - '0');
        if (result > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

/** 解析最多四位十六进制 CRC 字段。 */
static bool fn_protocol_parse_hex16(const uint8_t *data, size_t length,
    uint16_t *value) {
    if (length == 0 || length > 4) {
        return false;
    }
    uint16_t result = 0;
    for (size_t index = 0; index < length; ++index) {
        const uint8_t character = data[index];
        uint8_t digit;
        if (character >= '0' && character <= '9') {
            digit = (uint8_t)(character - '0');
        } else if (character >= 'A' && character <= 'F') {
            digit = (uint8_t)(character - 'A' + 10);
        } else if (character >= 'a' && character <= 'f') {
            digit = (uint8_t)(character - 'a' + 10);
        } else {
            return false;
        }
        result = (uint16_t)((result << 4) | digit);
    }
    *value = result;
    return true;
}

/** 返回原生协议接口版本，供 Python 适配层进行兼容性检查。 */
static mp_obj_t fn_protocol_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(FN_PROTOCOL_API_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_protocol_api_version_obj,
    fn_protocol_api_version);

/** 校验并解析一条完整 PV1 行，返回消息类型字符串与载荷字节串。 */
static mp_obj_t fn_protocol_parse_frame(mp_obj_t line_object,
    mp_obj_t maximum_payload_object) {
    mp_buffer_info_t line_buffer;
    mp_get_buffer_raise(line_object, &line_buffer, MP_BUFFER_READ);
    const uint8_t *line = (const uint8_t *)line_buffer.buf;
    const size_t line_length = line_buffer.len;
    const mp_int_t maximum_payload_value = mp_obj_get_int(maximum_payload_object);
    if (maximum_payload_value < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_LENGTH"));
    }
    const size_t maximum_payload = (size_t)maximum_payload_value;

    size_t separators[4];
    size_t separator_count = 0;
    for (size_t index = 0; index < line_length && separator_count < 4; ++index) {
        if (line[index] == ':') {
            separators[separator_count++] = index;
        }
    }
    if (separator_count != 4 || separators[0] != 3
        || memcmp(line, "PV1", 3) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_HEADER"));
    }

    const size_t type_start = separators[0] + 1;
    const size_t type_length = separators[1] - type_start;
    size_t payload_length;
    uint16_t expected_crc;
    const uint8_t *length_field = line + separators[1] + 1;
    const size_t length_field_size = separators[2] - separators[1] - 1;
    if (length_field_size > 0 && length_field[0] == '-') {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_LENGTH"));
    }
    if (!fn_protocol_parse_decimal(length_field, length_field_size, &payload_length)
        || !fn_protocol_parse_hex16(line + separators[2] + 1,
            separators[3] - separators[2] - 1, &expected_crc)) {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_HEADER"));
    }
    const size_t payload_start = separators[3] + 1;
    if (payload_length > maximum_payload
        || payload_length > line_length - payload_start) {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_LENGTH"));
    }
    for (size_t index = payload_start + payload_length;
            index < line_length; ++index) {
        if (line[index] != ' ') {
            mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_TRAILER"));
        }
    }

    uint16_t actual_crc = fn_protocol_crc16_update(0xFFFFU,
        line + type_start, type_length);
    const uint8_t separator = ':';
    actual_crc = fn_protocol_crc16_update(actual_crc, &separator, 1);
    actual_crc = fn_protocol_crc16_update(actual_crc,
        line + payload_start, payload_length);
    if (actual_crc != expected_crc) {
        mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_CRC"));
    }
    for (size_t index = 0; index < type_length; ++index) {
        if (line[type_start + index] >= 0x80U) {
            mp_raise_ValueError(MP_ERROR_TEXT("BAD_FRAME_TYPE"));
        }
    }

    mp_obj_t result[2] = {
        mp_obj_new_str((const char *)line + type_start, type_length),
        mp_obj_new_bytes(line + payload_start, payload_length),
    };
    return mp_obj_new_tuple(2, result);
}
static MP_DEFINE_CONST_FUN_OBJ_2(fn_protocol_parse_frame_obj,
    fn_protocol_parse_frame);

static const mp_rom_map_elem_t fn_protocol_module_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_protocol)},
    {MP_ROM_QSTR(MP_QSTR_API_VERSION), MP_ROM_INT(FN_PROTOCOL_API_VERSION)},
    {MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_protocol_api_version_obj)},
    {MP_ROM_QSTR(MP_QSTR_parse_frame), MP_ROM_PTR(&fn_protocol_parse_frame_obj)},
};
static MP_DEFINE_CONST_DICT(fn_protocol_module_globals,
    fn_protocol_module_globals_table);

const mp_obj_module_t fn_protocol_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&fn_protocol_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_protocol, fn_protocol_user_cmodule);
