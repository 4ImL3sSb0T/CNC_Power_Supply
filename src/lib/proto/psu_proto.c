/**
 * @file        psu_proto.c
 * @brief       上位机 ↔ 测试台 通信协议编解码（纯逻辑，不依赖 RTOS / 硬件）
 *
 * 帧格式与命令定义见 psu_proto.h。这里只有三件事：
 *   1. CRC-16/CCITT-FALSE
 *   2. COBS 编解码（0x00 作为唯一分隔符）
 *   3. 帧的封装与解析（长度字段 + CRC 双重校验）
 */

#include "lib/proto/psu_proto.h"

#include <string.h>

u16 psu_crc16(const u8 *data, u32 len)
{
    u16 crc = 0xFFFFu;
    u32 i;
    u8 bit;

    for (i = 0; i < len; i++) {
        crc ^= (u16)((u16)data[i] << 8);
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (u16)((u16)(crc << 1) ^ 0x1021u);
            } else {
                crc = (u16)(crc << 1);
            }
        }
    }
    return crc;
}

void psu_put_u16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

u16 psu_get_u16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

void psu_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

u32 psu_get_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

i32 psu_cobs_encode(const u8 *in, u32 len, u8 *out, u32 out_size)
{
    u32 read = 0;
    u32 write = 1;      /* out[0] 预留给第一块的 code 字节 */
    u32 code_idx = 0;
    u32 code = 1;       /* 当前块已包含的字节数（含 code 字节自身） */

    if (out == NULL || out_size == 0) {
        return EXIT_INVALID_PARAM;
    }
    if (in == NULL && len != 0) {
        return EXIT_INVALID_PARAM;
    }

    while (read < len) {
        if (write >= out_size) {
            return EXIT_NO_MEMORY;
        }
        if (in[read] == 0x00) {
            /* 遇 0x00：当前块到此为止，下一块从下一字节开始 */
            out[code_idx] = (u8)code;
            code = 1;
            code_idx = write++;
            read++;
        } else {
            out[write++] = in[read++];
            if (++code == 0xFFu) {
                /* 块满 254 字节：强制收尾（此时隐含一个 0x00 语义） */
                out[code_idx] = 0xFFu;
                code = 1;
                code_idx = write++;
            }
        }
    }

    out[code_idx] = (u8)code;
    return (i32)write;
}

exit_code_t psu_cobs_decode(const u8 *in, u32 len, u8 *out, u32 out_size, u32 *out_len)
{
    u32 read = 0;
    u32 write = 0;
    u8 code;
    u8 i;

    if (in == NULL || out == NULL || out_len == NULL) {
        return EXIT_INVALID_PARAM;
    }
    if (len == 0) {
        return EXIT_INVALID_PARAM;
    }

    while (read < len) {
        code = in[read++];
        if (code == 0x00) {
            /* 0x00 只会作为分隔符出现在链路上，不该出现在块内部 */
            return EXIT_INVALID_PARAM;
        }
        for (i = 1; i < code; i++) {
            if (read >= len) {
                return EXIT_INVALID_PARAM;      /* 块被截断 */
            }
            if (write >= out_size) {
                return EXIT_NO_MEMORY;
            }
            out[write++] = in[read++];
        }
        if (code < 0xFFu && read < len) {
            /* code < 0xFF 表示块尾跟着一个被编码掉的 0x00；
             * 只有它位于块末尾（read == len）时才不是真实数据 */
            if (write >= out_size) {
                return EXIT_NO_MEMORY;
            }
            out[write++] = 0x00;
        }
    }

    *out_len = write;
    return EXIT_OK;
}

i32 psu_proto_encode(u8 cmd, u8 seq, const u8 *payload, u8 payload_len, u8 *out, u32 out_size)
{
    u8 body[PSU_PROTO_MAX_BODY];
    i32 encoded;

    if (out == NULL) {
        return EXIT_INVALID_PARAM;
    }
    if (payload_len > PSU_PROTO_MAX_PAYLOAD) {
        return EXIT_INVALID_PARAM;
    }
    if (payload_len != 0 && payload == NULL) {
        return EXIT_INVALID_PARAM;
    }
    /* 一次要够整帧：宁可直接拒绝，也不写出半帧让对端去猜 */
    if (out_size < PSU_PROTO_MAX_ENCODED) {
        return EXIT_NO_MEMORY;
    }

    body[0] = cmd;
    body[1] = seq;
    body[2] = payload_len;
    if (payload_len != 0) {
        memcpy(&body[3], payload, payload_len);
    }
    psu_put_u16(&body[3 + payload_len], psu_crc16(body, 3u + payload_len));

    encoded = psu_cobs_encode(body, 3u + payload_len + 2u, out, out_size - 1u);
    if (encoded < 0) {
        return encoded;
    }
    out[encoded] = 0x00;                        /* 分隔符 */
    return encoded + 1;
}

exit_code_t psu_proto_decode(const u8 *block, u32 block_len, psu_frame_t *frame)
{
    u8 body[PSU_PROTO_MAX_BODY];
    u32 body_len = 0;
    exit_code_t rc;
    u8 len;

    if (block == NULL || frame == NULL) {
        return EXIT_INVALID_PARAM;
    }
    if (block_len < 2u || block_len > PSU_PROTO_MAX_BLOCK) {
        return EXIT_INVALID_PARAM;
    }

    rc = psu_cobs_decode(block, block_len, body, sizeof(body), &body_len);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (body_len < PSU_PROTO_MIN_BODY) {
        return EXIT_INVALID_PARAM;
    }

    len = body[2];
    if (len > PSU_PROTO_MAX_PAYLOAD) {
        return EXIT_INVALID_PARAM;
    }
    if (body_len != (u32)len + PSU_PROTO_MIN_BODY) {
        return EXIT_INVALID_PARAM;
    }
    if (psu_crc16(body, 3u + len) != psu_get_u16(&body[3 + len])) {
        return EXIT_CRC_MISMATCH;
    }

    frame->cmd = body[0];
    frame->seq = body[1];
    frame->len = len;
    if (len != 0) {
        memcpy(frame->payload, &body[3], len);
    }
    return EXIT_OK;
}
