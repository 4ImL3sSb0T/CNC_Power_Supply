/**
 * @file        codec_vectors.c
 * @brief       在 PC 上把固件的协议编解码跑一遍，输出黄金向量（配合 Python 对比）
 *
 * 目的：固件的 src/lib/proto/psu_proto.c 与上位机的 protocol.py 是各写一份的两套实现，
 * 靠"同一组输入 → 同一串字节"来保证不跑偏。这个程序编出来在 PC 上运行，输出格式与
 * tools/gen_codec_vectors.py 完全一致，直接 diff 两边即可：
 *
 *   clang -I.. -I../../src -o codec_vectors.exe tools/codec_vectors.c src/lib/proto/psu_proto.c
 *   ./codec_vectors.exe > c.txt
 *   uv run python tools/gen_codec_vectors.py > py.txt
 *   diff c.txt py.txt && echo "两边字节一致"
 *
 * 这里照抄 psu_link.c 的 link_send 组帧方式（前置 0x00 + psu_proto_encode + 尾部 0x00）：
 * 真正的 link_send 依赖 TinyUSB，没法在主机端编译。
 */

#include <stdio.h>
#include <string.h>

#include "lib/proto/psu_proto.h"

/* 与 psu_link.c 的 link_send 一致：前置分隔符 + COBS + 尾部分隔符 */
static int frame_to_wire(u8 cmd, u8 seq, const u8 *payload, u8 len, u8 *out, unsigned out_size)
{
    i32 encoded;

    if (out_size < 1u) {
        return -1;
    }
    out[0] = 0x00;
    encoded = psu_proto_encode(cmd, seq, payload, len, &out[1], out_size - 1u);
    if (encoded < 0) {
        return -1;
    }
    return (int)encoded + 1;
}

static void dump(const char *name, u8 cmd, u8 seq, const u8 *payload, u8 len)
{
    u8 wire[64];
    int total = frame_to_wire(cmd, seq, payload, len, wire, sizeof(wire));
    int i;

    if (total < 0) {
        printf("%-9s len=?? 组帧失败\n", name);
        return;
    }
    printf("%-9s len=%3d ", name, total);
    for (i = 0; i < total; i++) {
        printf("%02x", wire[i]);
    }
    printf("\n");
}

int main(void)
{
    u8 payload[PSU_PROTO_MAX_PAYLOAD];
    unsigned i;

    dump("PING", PSU_CMD_PING, 7, (const u8[]){ 0x44, 0x33, 0x22, 0x11 }, 4);

    payload[0] = (u8)PSU_UNIT_MV;
    psu_put_u16(&payload[1], 12350);
    dump("SET_VOUT", PSU_CMD_SET_VOUT, 1, payload, 3);

    dump("SAFE", PSU_CMD_SAFE_STATE, 255, NULL, 0);

    for (i = 0; i < 16u; i++) {
        payload[i] = (u8)i;
    }
    dump("TELEM16", PSU_RSP_TELEM, 0, payload, 16);

    payload[0] = (u8)PSU_CMD_SET_VOUT;
    payload[1] = (u8)(i8)EXIT_BUSY;
    psu_put_u16(&payload[2], 433);
    dump("ACK", PSU_RSP_ACK, 9, payload, 4);

    memset(payload, 0, sizeof(payload));
    dump("ITEM", PSU_RSP_ITEM, 0, payload, PSU_ITEM_PAYLOAD_LEN);

    return 0;
}
