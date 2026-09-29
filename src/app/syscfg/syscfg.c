/**
 * @file        syscfg.c
 * @brief       持久化配置实现：32 字节 blob 的打包/解包 + 应用策略
 *
 * 落盘 blob（/cfg/psu.cfg，pstore 再套一层 len+crc16 信封）：
 *
 *   0  u32 magic      0x31554350 ("PCU1")
 *   4  u8  version    结构版本
 *   5  u8  len        本块长度（32）
 *   6  u8  flags      bit0 = 有效
 *   7  u8  reserved
 *   8  u16 vout_permille
 *   10 u16 ilim_permille
 *   12 u16 telem_period_ms
 *   14 u16 wd_timeout_ms
 *   16 u32 boot_count
 *   20 u8[10] reserved
 *   30 u16 crc16       覆盖 0..29
 */

#include "app/syscfg/syscfg.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "app/psu_link/psu_link.h"
#include "lib/proto/psu_proto.h"
#include "service/pstore/pstore.h"

#define SYSCFG_MAGIC     0x31554350u
#define SYSCFG_VERSION   1u
#define SYSCFG_BLOB_LEN  32u

static syscfg_t s_cfg;                  /* 当前有效配置 */
static bool s_ensured;                  /* 已经尝试过加载（无论成功与否） */
static bool s_loaded;                   /* 上电时真的从 Flash 读到了有效配置 */

static bool s_setpoint_pending;         /* 设定值改动待落盘 */
static u32  s_setpoint_deadline_ms;

static u32 now_ms(void)
{
    return (u32)to_ms_since_boot(get_absolute_time());
}

/* ---------------- blob 打包/解包 ---------------- */

static void blob_pack(u8 *b, const syscfg_t *c)
{
    memset(b, 0, SYSCFG_BLOB_LEN);
    psu_put_u32(&b[0], SYSCFG_MAGIC);
    b[4] = (u8)SYSCFG_VERSION;
    b[5] = (u8)SYSCFG_BLOB_LEN;
    b[6] = 0x01u;                       /* 有效 */
    psu_put_u16(&b[8], c->vout_permille);
    psu_put_u16(&b[10], c->ilim_permille);
    psu_put_u16(&b[12], c->telem_period_ms);
    psu_put_u16(&b[14], c->wd_timeout_ms);
    psu_put_u32(&b[16], c->boot_count);
    psu_put_u16(&b[30], psu_crc16(b, 30u));
}

static exit_code_t blob_unpack(const u8 *b, syscfg_t *c)
{
    if (psu_get_u32(&b[0]) != SYSCFG_MAGIC) {
        return EXIT_INVALID_PARAM;
    }
    if (b[4] != (u8)SYSCFG_VERSION || b[5] != (u8)SYSCFG_BLOB_LEN) {
        return EXIT_NOT_SUPPORTED;      /* 别的版本的 blob：不认，也不覆写 */
    }
    if (psu_get_u16(&b[30]) != psu_crc16(b, 30u)) {
        return EXIT_CRC_MISMATCH;
    }

    c->vout_permille = psu_get_u16(&b[8]);
    c->ilim_permille = psu_get_u16(&b[10]);
    c->telem_period_ms = psu_get_u16(&b[12]);
    c->wd_timeout_ms = psu_get_u16(&b[14]);
    c->boot_count = psu_get_u32(&b[16]);
    return EXIT_OK;
}

static void cfg_defaults(syscfg_t *c)
{
    c->telem_period_ms = (u16)PSU_LINK_TELEM_PERIOD_MS;
    c->wd_timeout_ms = 0u;              /* 未武装；上电也不武装 */
    c->vout_permille = 0u;              /* 安全态 */
    c->ilim_permille = 0u;
    c->boot_count = 0u;
}

static exit_code_t syscfg_save(void)
{
    u8 blob[SYSCFG_BLOB_LEN];

    blob_pack(blob, &s_cfg);
    return pstore_cfg_save(blob, sizeof(blob));
}

/* ---------------- 加载与应用 ---------------- */

static void log_boot(void)
{
    pstore_rec_t rec;

    pstore_rec_init(&rec, (u8)PSTORE_REC_BOOT);
    rec.a = (u8)PSU_LINK_FW_MAJOR;
    rec.b = (u8)PSU_LINK_FW_MINOR;
    rec.arg = s_cfg.boot_count;
    (void)pstore_log_append(&rec);
}

bool syscfg_ensure_loaded(void)
{
    u8 blob[SYSCFG_BLOB_LEN];
    u32 len = 0u;
    exit_code_t rc;
    bool write_back = false;

    if (s_ensured) {
        return false;
    }
    if (!pstore_ready()) {
        return false;                   /* 文件系统还没挂载好，下一轮再来 */
    }
    s_ensured = true;

    cfg_defaults(&s_cfg);

    rc = pstore_cfg_get(blob, sizeof(blob), &len);
    if (rc == EXIT_OK && len == SYSCFG_BLOB_LEN && blob_unpack(blob, &s_cfg) == EXIT_OK) {
        s_loaded = true;
        write_back = true;              /* 只为把自增后的 boot_count 落下 */
    } else if (rc == EXIT_DOES_NOT_EXIST) {
        s_loaded = false;               /* 首次上电或已被清掉：建一份默认值 */
        write_back = true;
    } else {
        s_loaded = false;               /* 文件在但校验不过：保留原文件当证据，不覆写 */
        printf("[SYSCFG] 配置不可用（%d），本次用默认值\n", rc);
    }

    s_cfg.boot_count++;
    if (write_back) {
        (void)syscfg_save();
    }
    log_boot();

    printf("[SYSCFG] 配置%s：遥测 %ums，预设 VOUT %u‰ / ILIM %u‰，启动序号 %u\n",
           s_loaded ? "已载入" : "用默认值",
           (unsigned)s_cfg.telem_period_ms, (unsigned)s_cfg.vout_permille,
           (unsigned)s_cfg.ilim_permille, (unsigned)s_cfg.boot_count);
    return true;
}

exit_code_t syscfg_get(syscfg_t *out, u8 *flags)
{
    if (!s_ensured) {
        return EXIT_BUSY;
    }

    *out = s_cfg;
    *flags = 0u;
    *flags |= s_loaded ? (u8)PSU_CFG_FLAG_LOADED : (u8)PSU_CFG_FLAG_DEFAULT;
    if (pstore_cfg_dirty() || s_setpoint_pending) {
        *flags |= (u8)PSU_CFG_FLAG_DIRTY;
    }
    return EXIT_OK;
}

exit_code_t syscfg_set_field(u8 field, u16 value, u16 *effected)
{
    u16 permille = 0u;
    exit_code_t rc;

    if (!s_ensured) {
        return EXIT_BUSY;               /* 还没加载完，主机重试即可 */
    }

    switch ((psu_cfg_field_t)field) {
    case PSU_CFG_F_TELEM_PERIOD:
        if (value != 0u &&
            (value < PSU_LINK_TELEM_PERIOD_MIN || value > PSU_LINK_TELEM_PERIOD_MAX)) {
            return EXIT_INVALID_PARAM;
        }
        s_cfg.telem_period_ms = value;
        *effected = value;
        break;

    case PSU_CFG_F_WD_TIMEOUT:
        if (value != 0u &&
            (value < PSU_LINK_WD_TIMEOUT_MIN_MS || value > PSU_LINK_WD_TIMEOUT_MAX_MS)) {
            return EXIT_INVALID_PARAM;
        }
        s_cfg.wd_timeout_ms = value;
        *effected = value;
        break;

    case PSU_CFG_F_VOUT:
        rc = psu_link_vout_to_permille((u8)PSU_UNIT_MV, value, &permille);
        if (rc != EXIT_OK) {
            return rc;
        }
        s_cfg.vout_permille = permille;
        *effected = permille;
        break;

    case PSU_CFG_F_ILIM:
        rc = psu_link_ilim_to_permille((u8)PSU_UNIT_MV, value, &permille);
        if (rc != EXIT_OK) {
            return rc;
        }
        s_cfg.ilim_permille = permille;
        *effected = permille;
        break;

    default:
        return EXIT_INVALID_PARAM;
    }

    /* 异步落盘：返回值只表示"已受理"，标志位 DIRTY 清零才算真写进去。
     * 入队失败（队列满）必须让调用方看到，否则会以为改成功了 */
    rc = syscfg_save();
    s_setpoint_pending = false;
    return rc;
}

exit_code_t syscfg_reset(void)
{
    u32 boot_count = s_cfg.boot_count;

    if (!s_ensured) {
        return EXIT_BUSY;
    }

    cfg_defaults(&s_cfg);
    s_cfg.boot_count = boot_count;      /* 计数器不参与"恢复出厂" */
    s_setpoint_pending = false;
    return syscfg_save();
}

void syscfg_note_setpoint(u8 field, u16 permille)
{
    if (!s_ensured) {
        return;
    }

    if (field == (u8)PSU_CFG_F_VOUT) {
        if (s_cfg.vout_permille == permille) {
            return;                     /* 值没变就不折腾 Flash */
        }
        s_cfg.vout_permille = permille;
    } else if (field == (u8)PSU_CFG_F_ILIM) {
        if (s_cfg.ilim_permille == permille) {
            return;
        }
        s_cfg.ilim_permille = permille;
    } else {
        return;
    }

    s_setpoint_pending = true;
    s_setpoint_deadline_ms = now_ms() + SYSCFG_SETPOINT_SAVE_MS;
}

void syscfg_tick(void)
{
    if (!s_setpoint_pending) {
        return;
    }
    if ((i32)(now_ms() - s_setpoint_deadline_ms) < 0) {
        return;
    }

    /* 落盘失败（队列满）就保持待落盘状态，下一拍再试 */
    if (syscfg_save() == EXIT_OK) {
        s_setpoint_pending = false;
    }
}
