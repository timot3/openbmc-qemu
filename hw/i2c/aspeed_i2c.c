/*
 * ARM Aspeed I2C controller
 *
 * Copyright (C) 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/i2c/aspeed_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "trace.h"

/* I2C Global Register */

#define I2C_CTRL_STATUS         0x00        /* Device Interrupt Status */
#define I2C_CTRL_ASSIGN         0x08        /* Device Interrupt Target
                                               Assignment */
#define I2C_CTRL_GLOBAL         0x0C        /* Global Control Register */
#define   I2C_CTRL_SRAM_EN                 BIT(0)
#define   I2C_CTRL_NEW_REG_MODE            BIT(2)
#define I2C_NEW_DIV_GLOBAL      0x10

/* I2C Device (Bus) Register */

#define I2CD_FUN_CTRL_REG       0x00       /* I2CD Function Control  */
#define   I2CD_POOL_PAGE_SEL(x)            (((x) >> 20) & 0x7)  /* AST2400 */
#define   I2CD_M_SDA_LOCK_EN               (0x1 << 16)
#define   I2CD_MULTI_MASTER_DIS            (0x1 << 15)
#define   I2CD_M_SCL_DRIVE_EN              (0x1 << 14)
#define   I2CD_MSB_STS                     (0x1 << 9)
#define   I2CD_SDA_DRIVE_1T_EN             (0x1 << 8)
#define   I2CD_M_SDA_DRIVE_1T_EN           (0x1 << 7)
#define   I2CD_M_HIGH_SPEED_EN             (0x1 << 6)
#define   I2CD_DEF_ADDR_EN                 (0x1 << 5)
#define   I2CD_DEF_ALERT_EN                (0x1 << 4)
#define   I2CD_DEF_ARP_EN                  (0x1 << 3)
#define   I2CD_DEF_GCALL_EN                (0x1 << 2)
#define   I2CD_SLAVE_EN                    (0x1 << 1)
#define   I2CD_MASTER_EN                   (0x1)

#define I2CD_AC_TIMING_REG1     0x04       /* Clock and AC Timing Control #1 */
#define I2CD_AC_TIMING_REG2     0x08       /* Clock and AC Timing Control #1 */
#define I2CD_INTR_CTRL_REG      0x0c       /* I2CD Interrupt Control */
#define I2CD_INTR_STS_REG       0x10       /* I2CD Interrupt Status */

#define   I2CD_INTR_SLAVE_ADDR_MATCH       (0x1 << 31) /* 0: addr1 1: addr2 */
#define   I2CD_INTR_SLAVE_ADDR_RX_PENDING  (0x1 << 30)
/* bits[19-16] Reserved */

/* All bits below are cleared by writing 1 */
#define   I2CD_INTR_SLAVE_INACTIVE_TIMEOUT (0x1 << 15)
#define   I2CD_INTR_SDA_DL_TIMEOUT         (0x1 << 14)
#define   I2CD_INTR_BUS_RECOVER_DONE       (0x1 << 13)
#define   I2CD_INTR_SMBUS_ALERT            (0x1 << 12) /* Bus [0-3] only */
#define   I2CD_INTR_SMBUS_ARP_ADDR         (0x1 << 11) /* Removed */
#define   I2CD_INTR_SMBUS_DEV_ALERT_ADDR   (0x1 << 10) /* Removed */
#define   I2CD_INTR_SMBUS_DEF_ADDR         (0x1 << 9)  /* Removed */
#define   I2CD_INTR_GCALL_ADDR             (0x1 << 8)  /* Removed */
#define   I2CD_INTR_SLAVE_ADDR_RX_MATCH    (0x1 << 7)  /* use RX_DONE */
#define   I2CD_INTR_SCL_TIMEOUT            (0x1 << 6)
#define   I2CD_INTR_ABNORMAL               (0x1 << 5)
#define   I2CD_INTR_NORMAL_STOP            (0x1 << 4)
#define   I2CD_INTR_ARBIT_LOSS             (0x1 << 3)
#define   I2CD_INTR_RX_DONE                (0x1 << 2)
#define   I2CD_INTR_TX_NAK                 (0x1 << 1)
#define   I2CD_INTR_TX_ACK                 (0x1 << 0)

#define I2CD_CMD_REG            0x14       /* I2CD Command/Status */
#define   I2CD_SDA_OE                      (0x1 << 28)
#define   I2CD_SDA_O                       (0x1 << 27)
#define   I2CD_SCL_OE                      (0x1 << 26)
#define   I2CD_SCL_O                       (0x1 << 25)
#define   I2CD_TX_TIMING                   (0x1 << 24)
#define   I2CD_TX_STATUS                   (0x1 << 23)

#define   I2CD_TX_STATE_SHIFT              19 /* Tx State Machine */
#define   I2CD_TX_STATE_MASK                  0xf
#define     I2CD_IDLE                         0x0
#define     I2CD_MACTIVE                      0x8
#define     I2CD_MSTART                       0x9
#define     I2CD_MSTARTR                      0xa
#define     I2CD_MSTOP                        0xb
#define     I2CD_MTXD                         0xc
#define     I2CD_MRXACK                       0xd
#define     I2CD_MRXD                         0xe
#define     I2CD_MTXACK                       0xf
#define     I2CD_SWAIT                        0x1
#define     I2CD_SRXD                         0x4
#define     I2CD_STXACK                       0x5
#define     I2CD_STXD                         0x6
#define     I2CD_SRXACK                       0x7
#define     I2CD_RECOVER                      0x3

#define   I2CD_SCL_LINE_STS                (0x1 << 18)
#define   I2CD_SDA_LINE_STS                (0x1 << 17)
#define   I2CD_BUS_BUSY_STS                (0x1 << 16)
#define   I2CD_SDA_OE_OUT_DIR              (0x1 << 15)
#define   I2CD_SDA_O_OUT_DIR               (0x1 << 14)
#define   I2CD_SCL_OE_OUT_DIR              (0x1 << 13)
#define   I2CD_SCL_O_OUT_DIR               (0x1 << 12)
#define   I2CD_BUS_RECOVER_CMD_EN          (0x1 << 11)
#define   I2CD_S_ALT_EN                    (0x1 << 10)

/* Command Bit */
#define   I2CD_RX_DMA_ENABLE               (0x1 << 9)
#define   I2CD_TX_DMA_ENABLE               (0x1 << 8)
#define   I2CD_RX_BUFF_ENABLE              (0x1 << 7)
#define   I2CD_TX_BUFF_ENABLE              (0x1 << 6)
#define   I2CD_M_STOP_CMD                  (0x1 << 5)
#define   I2CD_M_S_RX_CMD_LAST             (0x1 << 4)
#define   I2CD_M_RX_CMD                    (0x1 << 3)
#define   I2CD_S_TX_CMD                    (0x1 << 2)
#define   I2CD_M_TX_CMD                    (0x1 << 1)
#define   I2CD_M_START_CMD                 (0x1)

#define I2CD_DEV_ADDR_REG       0x18       /* Slave Device Address */
#define I2CD_POOL_CTRL_REG      0x1c       /* Pool Buffer Control */
#define   I2CD_POOL_RX_COUNT(x)            (((x) >> 24) & 0xff)
#define   I2CD_POOL_RX_SIZE(x)             ((((x) >> 16) & 0xff) + 1)
#define   I2CD_POOL_TX_COUNT(x)            ((((x) >> 8) & 0xff) + 1)
#define   I2CD_POOL_OFFSET(x)              (((x) & 0x3f) << 2)  /* AST2400 */
#define I2CD_BYTE_BUF_REG       0x20       /* Transmit/Receive Byte Buffer */
#define   I2CD_BYTE_BUF_TX_SHIFT           0
#define   I2CD_BYTE_BUF_TX_MASK            0xff
#define   I2CD_BYTE_BUF_RX_SHIFT           8
#define   I2CD_BYTE_BUF_RX_MASK            0xff
#define I2CD_DMA_ADDR           0x24       /* DMA Buffer Address */
#define I2CD_DMA_LEN            0x28       /* DMA Transfer Length < 4KB */

/* New register mode */
#define I2CC_M_S_FUNC_CTRL_REG  0x00
#define   I2CC_SLAVE_ADDR_RX_EN     BIT(20)
#define   I2CC_MASTER_RETRY_MASK    (0x3 << 18)
#define   I2CC_MASTER_RETRY(x)      ((x & 0x3) << 18)
#define   I2CC_BUS_AUTO_RELEASE     BIT(17)
#define   I2CC_M_SDA_LOCK_EN        BIT(16)
#define   I2CC_MULTI_MASTER_DIS     BIT(15)
#define   I2CC_M_SCL_DRIVE_EN       BIT(14)
#define   I2CC_MSB_STS              BIT(9)
#define   I2CC_SDA_DRIVE_1T_EN      BIT(8)
#define   I2CC_M_SDA_DRIVE_1T_EN    BIT(7)
#define   I2CC_M_HIGH_SPEED_EN      BIT(6)
#define   I2CC_SLAVE_EN             BIT(1)
#define   I2CC_MASTER_EN            BIT(0)

#define I2CC_M_S_CLK_AC_TIMING_REG 0x04
#define I2CC_M_S_TX_RX_BUF_REG  0x08
#define I2CC_M_X_POOL_BUF_CTRL_REG 0x0c
#define I2CM_INT_CTRL_REG     0x10
#define I2CM_INT_STS_REG      0x14
#define   I2CM_PKT_OP_SM_SHIFT      28
#define   I2CM_PKT_OP_SM_IDLE       0x0
#define   I2CM_PKT_OP_SM_STARTH     0x1
#define   I2CM_PKT_OP_SM_STARTW     0x2
#define   I2CM_PKT_OP_SM_STARTR     0x3
#define   I2CM_PKT_OP_SM_TXMCODE    0x4
#define   I2CM_PKT_OP_SM_TXAW       0x5
#define   I2CM_PKT_OP_SM_INIT       0x8
#define   I2CM_PKT_OP_SM_TXD        0x9
#define   I2CM_PKT_OP_SM_RXD        0xa
#define   I2CM_PKT_OP_SM_STOP       0xb
#define   I2CM_PKT_OP_SM_RETRY      0xc
#define   I2CM_PKT_OP_SM_FAIL       0xd
#define   I2CM_PKT_OP_SM_WAIT       0xe
#define   I2CM_PKT_OP_SM_PASS       0xf
#define   I2CM_PKT_TIMEOUT          BIT(18)
#define   I2CM_PKT_ERROR            BIT(17)
#define   I2CM_PKT_DONE             BIT(16)
#define   I2CM_BUS_RECOVER_FAIL     BIT(15)
#define   I2CM_SDA_DL_TO            BIT(14)
#define   I2CM_BUS_RECOVER          BIT(13)
#define   I2CM_SMBUS_ALT            BIT(12)
#define   I2CM_SCL_LOW_TO           BIT(6)
#define   I2CM_ABNORMAL             BIT(5)
#define   I2CM_NORMAL_STOP          BIT(4)
#define   I2CM_ARBIT_LOSS           BIT(3)
#define   I2CM_RX_DONE              BIT(2)
#define   I2CM_TX_NAK               BIT(1)
#define   I2CM_TX_ACK               BIT(0)
#define I2CM_CMD_STS_REG      0x18
#define   I2CM_CMD_PKT_MODE           (1 << 16)

#define   I2CM_PKT_EN               BIT(16)
#define   I2CM_SDA_OE_OUT_DIR       BIT(15)
#define   I2CM_SDA_O_OUT_DIR        BIT(14)
#define   I2CM_SCL_OE_OUT_DIR       BIT(13)
#define   I2CM_SCL_O_OUT_DIR        BIT(12)
#define   I2CM_RECOVER_CMD_EN       BIT(11)
#define   I2CM_RX_DMA_EN            BIT(9)
#define   I2CM_TX_DMA_EN            BIT(8)
/* Command Bit */
#define   I2CM_RX_BUFF_EN           BIT(7)
#define   I2CM_TX_BUFF_EN           BIT(6)
#define   I2CM_STOP_CMD             BIT(5)
#define   I2CM_RX_CMD_LAST          BIT(4)
#define   I2CM_RX_CMD               BIT(3)
#define   I2CM_TX_CMD               BIT(1)
#define   I2CM_START_CMD            BIT(0)
#define   I2CM_PKT_ADDR(x)          ((x & 0x7f) << 24)

#define I2CM_DMA_LEN          0x1c
#define I2CS_INT_CTRL_REG     0x20
#define I2CS_INT_STS_REG      0x24
#define I2CS_CMD_STS_REG      0x28
#define I2CS_DMA_LEN          0x2c
#define I2CM_DMA_TX_BUF       0x30
#define I2CM_DMA_RX_BUF       0x34
#define I2CS_DMA_TX_BUF       0x38
#define I2CS_DMA_RX_BUF       0x3c
#define I2CS_SA_REG           0x40
#define I2CM_DMA_LEN_STS_REG  0x48
#define I2CS_DMA_LEN_STS_REG  0x4c
#define I2CC_DMA_OP_ADDR_REG  0x50
#define I2CC_DMA_OP_LEN_REG   0x54

static inline bool aspeed_i2c_ctrl_is_new_mode(AspeedI2CState *controller)
{
    return (controller->ctrl_global & I2C_CTRL_NEW_REG_MODE) == I2C_CTRL_NEW_REG_MODE;
}

static inline bool aspeed_i2c_bus_is_new_mode(AspeedI2CBus *bus)
{
    return aspeed_i2c_ctrl_is_new_mode(bus->controller);
}

static inline bool aspeed_i2c_bus_is_master(AspeedI2CBus *bus)
{
    return bus->ctrl & I2CD_MASTER_EN;
}

static inline bool aspeed_i2c_bus_is_enabled(AspeedI2CBus *bus)
{
    return bus->ctrl & (I2CD_MASTER_EN | I2CD_SLAVE_EN);
}

static inline void aspeed_i2c_bus_raise_interrupt_new(AspeedI2CBus *bus)
{
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);

    trace_aspeed_i2c_bus_raise_interrupt_new(bus->intr_status,
          bus->intr_status & I2CM_TX_NAK ? "nak|" : "",
          bus->intr_status & I2CM_TX_ACK ? "ack|" : "",
          bus->intr_status & I2CM_RX_DONE ? "done|" : "",
          bus->intr_status & I2CM_NORMAL_STOP ? "normal|" : "",
          bus->intr_status & I2CM_ABNORMAL ? "abnormal|" : "",
          bus->intr_status & I2CM_PKT_DONE ? "pkt" : "");

    if (bus->intr_status) {
        bus->controller->intr_status |= 1 << bus->id;
        qemu_irq_raise(aic->bus_get_irq(bus));
    }
}

static inline void aspeed_i2c_bus_raise_interrupt(AspeedI2CBus *bus)
{
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);

    trace_aspeed_i2c_bus_raise_interrupt(bus->intr_status,
          bus->intr_status & I2CD_INTR_TX_NAK ? "nak|" : "",
          bus->intr_status & I2CD_INTR_TX_ACK ? "ack|" : "",
          bus->intr_status & I2CD_INTR_RX_DONE ? "done|" : "",
          bus->intr_status & I2CD_INTR_SLAVE_ADDR_RX_MATCH ? "slave-match|" : "",
          bus->intr_status & I2CD_INTR_NORMAL_STOP ? "normal|" : "",
          bus->intr_status & I2CD_INTR_ABNORMAL ? "abnormal" : "");

    /*
     * WORKAROUND: the Linux Aspeed I2C driver masks SLAVE_ADDR_RX_MATCH for
     * some reason, not sure if it is a bug...
     */
    bus->intr_status &= (bus->intr_ctrl | I2CD_INTR_SLAVE_ADDR_RX_MATCH);
    if (bus->intr_status) {
        bus->controller->intr_status |= 1 << bus->id;
        qemu_irq_raise(aic->bus_get_irq(bus));
    }
}

static uint64_t aspeed_i2c_bus_read_new(void *opaque, hwaddr offset,
                                    unsigned size)
{
    AspeedI2CBus *bus = opaque;
    uint64_t value = -1;

    switch (offset) {
    case I2CC_M_S_FUNC_CTRL_REG:
        value = bus->ctrl;
        break;
    case I2CC_M_S_CLK_AC_TIMING_REG:
        value = ((bus->timing[1] & 0x1F) << 24) | (bus->timing[1] & 0xFFFFF);
        break;
    case I2CM_INT_CTRL_REG:
        value = bus->intr_ctrl;
        break;
    case I2CM_INT_STS_REG:
        value = bus->intr_status;
        break;
    case I2CM_CMD_STS_REG:
        value = bus->cmd;
        break;
    case I2CM_DMA_LEN:
        value = bus->dma_len & 0x0fff;
        break;
    case I2CM_DMA_TX_BUF: case I2CM_DMA_RX_BUF:
        value = bus->dma_addr;
        break;
    case I2CM_DMA_LEN_STS_REG:
        value = bus->dma_len_tx | (bus->dma_len_rx << 16);
        break;
    case I2CC_M_S_TX_RX_BUF_REG:
        /*
         * TODO:
         * [31:16] RO  Same as I2CD14[31:16]
         * [15: 0] RW  Same as I2CD20[15: 0]
         */
        value = (i2c_bus_busy(bus->bus) << 16);
        break;
    case I2CC_M_X_POOL_BUF_CTRL_REG:
    case I2CS_INT_CTRL_REG:
    case I2CS_INT_STS_REG:
    case I2CS_CMD_STS_REG:
    case I2CS_DMA_LEN:
    case I2CS_DMA_TX_BUF:
    case I2CS_DMA_RX_BUF:
    case I2CS_SA_REG:
    case I2CS_DMA_LEN_STS_REG:
    case I2CC_DMA_OP_ADDR_REG:
    case I2CC_DMA_OP_LEN_REG:
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        value = -1;
        break;
    }

    trace_aspeed_i2c_bus_read_new(bus->id, offset, size, value);
    return value;
}

static uint64_t aspeed_i2c_bus_read_old(void *opaque, hwaddr offset,
                                    unsigned size)
{
    AspeedI2CBus *bus = opaque;
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);
    uint64_t value = -1;

    switch (offset) {
    case I2CD_FUN_CTRL_REG:
        value = bus->ctrl;
        break;
    case I2CD_AC_TIMING_REG1:
        value = bus->timing[0];
        break;
    case I2CD_AC_TIMING_REG2:
        value = bus->timing[1];
        break;
    case I2CD_INTR_CTRL_REG:
        value = bus->intr_ctrl;
        break;
    case I2CD_INTR_STS_REG:
        value = bus->intr_status;
        break;
    case I2CD_DEV_ADDR_REG:
        value = bus->dev_addr;
        break;
    case I2CD_POOL_CTRL_REG:
        value = bus->pool_ctrl;
        break;
    case I2CD_BYTE_BUF_REG:
        value = bus->buf;
        break;
    case I2CD_CMD_REG:
        value = bus->cmd | (i2c_bus_busy(bus->bus) << 16);
        break;
    case I2CD_DMA_ADDR:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }
        value = bus->dma_addr;
        break;
    case I2CD_DMA_LEN:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }
        value = bus->dma_len;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        value = -1;
        break;
    }

    trace_aspeed_i2c_bus_read_old(bus->id, offset, size, value);
    return value;
}

static uint64_t aspeed_i2c_bus_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    AspeedI2CBus *bus = opaque;
    if (aspeed_i2c_bus_is_new_mode(bus)) {
        return aspeed_i2c_bus_read_new(opaque, offset, size);
    } else {
        return aspeed_i2c_bus_read_old(opaque, offset, size);
    }
}

static void aspeed_i2c_set_state(AspeedI2CBus *bus, uint8_t state)
{
    if (aspeed_i2c_bus_is_new_mode(bus)) {
        bus->tx_state_machine = state;
    } else {
        bus->cmd &= ~(I2CD_TX_STATE_MASK << I2CD_TX_STATE_SHIFT);
        bus->cmd |= (state & I2CD_TX_STATE_MASK) << I2CD_TX_STATE_SHIFT;
    }
}

static uint8_t aspeed_i2c_get_state(AspeedI2CBus *bus)
{
    if (aspeed_i2c_bus_is_new_mode(bus)) {
        return bus->tx_state_machine;
    } else {
        return (bus->cmd >> I2CD_TX_STATE_SHIFT) & I2CD_TX_STATE_MASK;
    }
}

static int aspeed_i2c_dma_read(AspeedI2CBus *bus, uint8_t *data)
{
    MemTxResult result;
    AspeedI2CState *s = bus->controller;

    result = address_space_read(&s->dram_as, bus->dma_addr,
                                MEMTXATTRS_UNSPECIFIED, data, 1);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DRAM read failed @%08x\n",
                      __func__, bus->dma_addr);
        return -1;
    }

    bus->dma_addr++;
    bus->dma_len--;
    bus->dma_len_tx++;
    return 0;
}

static int aspeed_i2c_dma_write(AspeedI2CBus *bus, uint8_t *data)
{
    MemTxResult result;
    AspeedI2CState *s = bus->controller;

    result = address_space_write(&s->dram_as, bus->dma_addr,
                                 MEMTXATTRS_UNSPECIFIED, data, 1);

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DRAM read failed @%08x\n",
                      __func__, bus->dma_addr);
        return -1;
    }

    bus->dma_addr++;
    bus->dma_len--;
    bus->dma_len_rx++;
    return 0;
}

static int aspeed_i2c_bus_send(AspeedI2CBus *bus, uint8_t pool_start)
{
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);
    int ret = -1;
    int i;

    if (bus->cmd & I2CD_TX_BUFF_ENABLE) {
        for (i = pool_start; i < I2CD_POOL_TX_COUNT(bus->pool_ctrl); i++) {
            uint8_t *pool_base = aic->bus_pool_base(bus);

            trace_aspeed_i2c_bus_send("BUF", i + 1,
                                      I2CD_POOL_TX_COUNT(bus->pool_ctrl),
                                      pool_base[i]);
            ret = i2c_send(bus->bus, pool_base[i]);
            if (ret) {
                break;
            }
        }
        bus->cmd &= ~I2CD_TX_BUFF_ENABLE;
    } else if (bus->cmd & I2CD_TX_DMA_ENABLE) {
        while (bus->dma_len) {
            uint8_t data;
            aspeed_i2c_dma_read(bus, &data);
            trace_aspeed_i2c_bus_send("DMA", bus->dma_len, bus->dma_len, data);
            ret = i2c_send(bus->bus, data);
            if (ret) {
                break;
            }
        }
        bus->cmd &= ~I2CD_TX_DMA_ENABLE;
    } else {
        trace_aspeed_i2c_bus_send("BYTE", pool_start, 1, bus->buf);
        ret = i2c_send(bus->bus, bus->buf);
    }

    return ret;
}

static void aspeed_i2c_bus_recv(AspeedI2CBus *bus)
{
    AspeedI2CState *s = bus->controller;
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(s);
    uint8_t data;
    int i;

    if (bus->cmd & I2CD_RX_BUFF_ENABLE) {
        uint8_t *pool_base = aic->bus_pool_base(bus);

        for (i = 0; i < I2CD_POOL_RX_SIZE(bus->pool_ctrl); i++) {
            pool_base[i] = i2c_recv(bus->bus);
            trace_aspeed_i2c_bus_recv("BUF", i + 1,
                                      I2CD_POOL_RX_SIZE(bus->pool_ctrl),
                                      pool_base[i]);
        }

        /* Update RX count */
        bus->pool_ctrl &= ~(0xff << 24);
        bus->pool_ctrl |= (i & 0xff) << 24;
        bus->cmd &= ~I2CD_RX_BUFF_ENABLE;
    } else if (bus->cmd & I2CD_RX_DMA_ENABLE) {
        uint8_t data;

        while (bus->dma_len) {
            MemTxResult result;

            data = i2c_recv(bus->bus);
            trace_aspeed_i2c_bus_recv("DMA", bus->dma_len, bus->dma_len, data);
            result = address_space_write(&s->dram_as, bus->dma_addr,
                                         MEMTXATTRS_UNSPECIFIED, &data, 1);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DRAM write failed @%08x\n",
                              __func__, bus->dma_addr);
                return;
            }
            bus->dma_addr++;
            bus->dma_len--;
        }
        bus->cmd &= ~I2CD_RX_DMA_ENABLE;
    } else {
        data = i2c_recv(bus->bus);
        trace_aspeed_i2c_bus_recv("BYTE", 1, 1, bus->buf);
        bus->buf = (data & I2CD_BYTE_BUF_RX_MASK) << I2CD_BYTE_BUF_RX_SHIFT;
    }
}

static void aspeed_i2c_handle_rx_cmd(AspeedI2CBus *bus)
{
    aspeed_i2c_set_state(bus, I2CD_MRXD);
    aspeed_i2c_bus_recv(bus);
    bus->intr_status |= I2CD_INTR_RX_DONE;
    if (bus->cmd & I2CD_M_S_RX_CMD_LAST) {
        i2c_nack(bus->bus);
    }
    bus->cmd &= ~(I2CD_M_RX_CMD | I2CD_M_S_RX_CMD_LAST);
    aspeed_i2c_set_state(bus, I2CD_MACTIVE);
}

static uint8_t aspeed_i2c_get_addr(AspeedI2CBus *bus)
{
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);

    if (aspeed_i2c_bus_is_new_mode(bus)) {
        if (bus->cmd & I2CM_CMD_PKT_MODE) {
            return (bus->cmd & 0x7F000000) >> 23 |
                   (bus->cmd & I2CM_RX_CMD ? 0x01 : 0x00);
        } else {
            /* TODO: Support other mode */
            qemu_log_mask(LOG_UNIMP, "%s: New register mode with cmd=%08x\n",
                          __func__, bus->cmd);
            return 0xFF;
        }
    } else {
        if (bus->cmd & I2CD_TX_BUFF_ENABLE) {
            uint8_t *pool_base = aic->bus_pool_base(bus);

            return pool_base[0];
        } else if (bus->cmd & I2CD_TX_DMA_ENABLE) {
            uint8_t data;

            aspeed_i2c_dma_read(bus, &data);
            return data;
        } else {
            return bus->buf;
        }
    }
}

static bool aspeed_i2c_check_sram(AspeedI2CBus *bus)
{
    AspeedI2CState *s = bus->controller;
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(s);

    if (!aic->check_sram) {
        return true;
    }

    /*
     * AST2500: SRAM must be enabled before using the Buffer Pool or
     * DMA mode.
     */
    if (!(s->ctrl_global & I2C_CTRL_SRAM_EN) &&
        (bus->cmd & (I2CD_RX_DMA_ENABLE | I2CD_TX_DMA_ENABLE |
                     I2CD_RX_BUFF_ENABLE | I2CD_TX_BUFF_ENABLE))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: SRAM is not enabled\n", __func__);
        return false;
    }

    return true;
}

static void aspeed_i2c_bus_cmd_dump(AspeedI2CBus *bus)
{
    g_autofree char *cmd_flags = NULL;
    uint32_t count;

    if (bus->cmd & (I2CD_RX_BUFF_ENABLE | I2CD_RX_BUFF_ENABLE)) {
        count = I2CD_POOL_TX_COUNT(bus->pool_ctrl);
    } else if (bus->cmd & (I2CD_RX_DMA_ENABLE | I2CD_RX_DMA_ENABLE)) {
        count = bus->dma_len;
    } else { /* BYTE mode */
        count = 1;
    }

    cmd_flags = g_strdup_printf("%s%s%s%s%s%s%s%s%s",
                                bus->cmd & I2CD_M_START_CMD ? "start|" : "",
                                bus->cmd & I2CD_RX_DMA_ENABLE ? "rxdma|" : "",
                                bus->cmd & I2CD_TX_DMA_ENABLE ? "txdma|" : "",
                                bus->cmd & I2CD_RX_BUFF_ENABLE ? "rxbuf|" : "",
                                bus->cmd & I2CD_TX_BUFF_ENABLE ? "txbuf|" : "",
                                bus->cmd & I2CD_M_TX_CMD ? "tx|" : "",
                                bus->cmd & I2CD_M_RX_CMD ? "rx|" : "",
                                bus->cmd & I2CD_M_S_RX_CMD_LAST ? "last|" : "",
                                bus->cmd & I2CD_M_STOP_CMD ? "stop" : "");

    trace_aspeed_i2c_bus_cmd(bus->cmd, cmd_flags, count, bus->intr_status);
}

/*
 * This cmd handler only process new register set with packet mode
 */
static void aspeed_i2c_bus_handle_cmd_new(AspeedI2CBus *bus, uint64_t value)
{
    uint32_t cmd_done = 0;

    if (bus->cmd & I2CM_CMD_PKT_MODE) {
        bus->intr_status |= I2CM_PKT_DONE;
        bus->dma_len_tx = 0;
        bus->dma_len_rx = 0;
    }

    if (bus->cmd & I2CM_START_CMD) {
        /* Send I2C_START event */
        uint8_t addr = aspeed_i2c_get_addr(bus);
        if (aspeed_i2c_get_state(bus) == I2CM_PKT_OP_SM_IDLE) {
            if (i2c_start_transfer(bus->bus, extract32(addr, 1, 7),
                                   extract32(addr, 0, 1))) {
                bus->intr_status |= I2CM_TX_NAK | I2CM_PKT_ERROR;
            }

            if (addr & 0x01) {
                aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_STARTR);
            } else {
                aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_STARTW);
            }
        }
        cmd_done |= I2CM_START_CMD;
    }

    if (bus->cmd & I2CM_TX_CMD) {
        /* Send through DMA */
        if (bus->cmd & I2CM_TX_DMA_EN) {
            while (bus->dma_len) {
                uint8_t data;
                int ret;
                aspeed_i2c_dma_read(bus, &data);
                trace_aspeed_i2c_bus_send("DMA", bus->dma_len, bus->dma_len, data);
                ret = i2c_send(bus->bus, data);
                if (ret) {
                    break;
                }
            }
            bus->intr_status |= I2CM_TX_ACK;
            cmd_done |= I2CM_TX_DMA_EN;
        } else {
            /* TODO: Support Byte/Buffer mode */
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Only support DMA\n",  __func__);
        }
        aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_TXD);
        cmd_done |= I2CM_TX_CMD;
    }

    if (bus->cmd & I2CM_RX_CMD) {
        uint8_t addr = aspeed_i2c_get_addr(bus);
        if (bus->cmd & I2CM_START_CMD &&
            aspeed_i2c_get_state(bus) != I2CM_PKT_OP_SM_STARTR) {
            /* Repeated Start */
            i2c_start_transfer(bus->bus, extract32(addr, 1, 7), extract32(addr, 0, 1));
            aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_STARTR);
        }
        if (bus->cmd & I2CM_RX_DMA_EN) {
            /* Write to DMA */
            while (bus->dma_len) {
                uint8_t data;
                data = i2c_recv(bus->bus);
                if (aspeed_i2c_dma_write(bus, &data)) {
                    break;
                }
            }
            cmd_done |= I2CM_RX_DMA_EN;
        }
        aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_RXD);
        cmd_done |= I2CM_RX_CMD;
    }

    if (bus->cmd & I2CM_RX_CMD_LAST) {
        i2c_nack(bus->bus);
        bus->intr_status |= I2CM_RX_DONE;
        cmd_done |= I2CM_RX_CMD_LAST;
    }

    if (bus->cmd & I2CM_STOP_CMD) {
        aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_STOP);
        /* Send I2C_END Event */
        i2c_end_transfer(bus->bus);
        aspeed_i2c_set_state(bus, I2CM_PKT_OP_SM_IDLE);
        bus->intr_status |= I2CM_NORMAL_STOP;
        cmd_done |= I2CM_STOP_CMD;
    }

    if (bus->cmd & I2CM_CMD_PKT_MODE) {
        bus->intr_status |= I2CM_PKT_DONE;
        cmd_done |= 0x7F000000 | I2CM_CMD_PKT_MODE;
    }

    bus->cmd &= ~cmd_done;
}

/*
 * The state machine needs some refinement. It is only used to track
 * invalid STOP commands for the moment.
 */
static void aspeed_i2c_bus_handle_cmd(AspeedI2CBus *bus, uint64_t value)
{
    uint8_t pool_start = 0;

    bus->cmd &= ~0xFFFF;
    bus->cmd |= value & 0xFFFF;

    if (!aspeed_i2c_check_sram(bus)) {
        return;
    }

    if (trace_event_get_state_backends(TRACE_ASPEED_I2C_BUS_CMD)) {
        aspeed_i2c_bus_cmd_dump(bus);
    }

    if (bus->cmd & I2CD_M_START_CMD) {
        uint8_t state = aspeed_i2c_get_state(bus) & I2CD_MACTIVE ?
            I2CD_MSTARTR : I2CD_MSTART;
        uint8_t addr;

        aspeed_i2c_set_state(bus, state);

        addr = aspeed_i2c_get_addr(bus);

        if (i2c_start_transfer(bus->bus, extract32(addr, 1, 7),
                               extract32(addr, 0, 1))) {
            bus->intr_status |= I2CD_INTR_TX_NAK;
        } else {
            bus->intr_status |= I2CD_INTR_TX_ACK;
        }

        bus->cmd &= ~I2CD_M_START_CMD;

        /*
         * The START command is also a TX command, as the slave
         * address is sent on the bus. Drop the TX flag if nothing
         * else needs to be sent in this sequence.
         */
        if (bus->cmd & I2CD_TX_BUFF_ENABLE) {
            if (I2CD_POOL_TX_COUNT(bus->pool_ctrl) == 1) {
                bus->cmd &= ~I2CD_M_TX_CMD;
            } else {
                /*
                 * Increase the start index in the TX pool buffer to
                 * skip the address byte.
                 */
                pool_start++;
            }
        } else if (bus->cmd & I2CD_TX_DMA_ENABLE) {
            if (bus->dma_len == 0) {
                bus->cmd &= ~I2CD_M_TX_CMD;
            }
        } else {
            bus->cmd &= ~I2CD_M_TX_CMD;
        }

        /* No slave found */
        if (!i2c_bus_busy(bus->bus)) {
            return;
        }
        aspeed_i2c_set_state(bus, I2CD_MACTIVE);
    }

    if (bus->cmd & I2CD_M_TX_CMD) {
        aspeed_i2c_set_state(bus, I2CD_MTXD);
        if (aspeed_i2c_bus_send(bus, pool_start)) {
            bus->intr_status |= (I2CD_INTR_TX_NAK);
            i2c_end_transfer(bus->bus);
        } else {
            bus->intr_status |= I2CD_INTR_TX_ACK;
        }
        bus->cmd &= ~I2CD_M_TX_CMD;
        aspeed_i2c_set_state(bus, I2CD_MACTIVE);
    }

    if ((bus->cmd & (I2CD_M_RX_CMD | I2CD_M_S_RX_CMD_LAST)) &&
        !(bus->intr_status & I2CD_INTR_RX_DONE)) {
        aspeed_i2c_handle_rx_cmd(bus);
    }

    if (bus->cmd & I2CD_M_STOP_CMD) {
        if (!(aspeed_i2c_get_state(bus) & I2CD_MACTIVE)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: abnormal stop\n", __func__);
            bus->intr_status |= I2CD_INTR_ABNORMAL;
        } else {
            aspeed_i2c_set_state(bus, I2CD_MSTOP);
            i2c_end_transfer(bus->bus);
            bus->intr_status |= I2CD_INTR_NORMAL_STOP;
        }
        bus->cmd &= ~I2CD_M_STOP_CMD;
        aspeed_i2c_set_state(bus, I2CD_IDLE);
    }
}

static void aspeed_i2c_bus_write_new(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    AspeedI2CBus *bus = opaque;
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);
    trace_aspeed_i2c_bus_write_new(bus->id, offset, size, value);

    switch (offset) {
    case I2CC_M_S_FUNC_CTRL_REG:
        if (value & I2CD_SLAVE_EN) {
            qemu_log_mask(LOG_UNIMP, "%s: slave mode not implemented\n",
                          __func__);
            break;
        }
        bus->ctrl = value & 0x007FFFFF;
        break;
    case I2CC_M_S_CLK_AC_TIMING_REG:
        bus->timing[0] = value & 0x000FFF0F;
        bus->timing[1] = (value & 0x1F000000) >> 24;
        break;
    case I2CC_M_S_TX_RX_BUF_REG:
        bus->buf = value & 0xFF;
        break;
    case I2CM_INT_CTRL_REG:
        bus->intr_ctrl = value & 0x77FFF;
        break;
    case I2CM_INT_STS_REG:
        if (value & I2CM_PKT_DONE) {
            bus->intr_status &= ~(0x7E07F);
        } else {
            bus->intr_status &= ~(value & 0x7F07F);
        }
        if (1 || !(bus->intr_status & 0x7F07F)) {
            bus->controller->intr_status &= ~(1 << bus->id);
            qemu_irq_lower(aic->bus_get_irq(bus));
        }
        break;
    case I2CM_CMD_STS_REG:
        if (!aspeed_i2c_bus_is_enabled(bus)) {
            break;
        }
        if (!aspeed_i2c_bus_is_master(bus)) {
            qemu_log_mask(LOG_UNIMP, "%s: slave mode not implemented\n",
                            __func__);
            break;
        }
        bus->cmd = value;
        aspeed_i2c_bus_handle_cmd_new(bus, value);
        aspeed_i2c_bus_raise_interrupt_new(bus);
        break;
    case I2CM_DMA_LEN:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }
        /* 0 = 1 byte, 1 = 2 bytes, 4095 = 4096 bytes */
        if (value & 0x00008000) {
            bus->dma_len = (value & 0xfff) + 1;
        } else if (value & 0x80000000) {
            bus->dma_len = ((value >> 16) & 0xfff) + 1;
        }
        break;
    case I2CM_DMA_TX_BUF: case I2CM_DMA_RX_BUF:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }
        bus->dma_addr = value & 0x7fffffff;
        break;
    case I2CM_DMA_LEN_STS_REG:
        bus->dma_len_tx = 0;
        bus->dma_len_rx = 0;
        break;
    case I2CC_M_X_POOL_BUF_CTRL_REG:
    case I2CS_INT_CTRL_REG:
    case I2CS_INT_STS_REG:
    case I2CS_CMD_STS_REG:
    case I2CS_DMA_LEN:
    case I2CS_DMA_TX_BUF:
    case I2CS_DMA_RX_BUF:
    case I2CS_SA_REG:
    case I2CS_DMA_LEN_STS_REG:
    case I2CC_DMA_OP_ADDR_REG:
    case I2CC_DMA_OP_LEN_REG:
    default:
        break;
    }
}

static void aspeed_i2c_bus_write_old(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    AspeedI2CBus *bus = opaque;
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(bus->controller);
    bool handle_rx;

    trace_aspeed_i2c_bus_write_old(bus->id, offset, size, value);

    switch (offset) {
    case I2CD_FUN_CTRL_REG:
        if (value & I2CD_SLAVE_EN) {
            i2c_slave_set_address(&bus->slave->i2c, bus->dev_addr);
        }

        bus->ctrl = value & 0x0071C3FF;
        break;
    case I2CD_AC_TIMING_REG1:
        bus->timing[0] = value & 0xFFFFF0F;
        break;
    case I2CD_AC_TIMING_REG2:
        bus->timing[1] = value & 0x7;
        break;
    case I2CD_INTR_CTRL_REG:
        bus->intr_ctrl = value & 0x7FFF;
        break;
    case I2CD_INTR_STS_REG:
        handle_rx = (bus->intr_status & I2CD_INTR_RX_DONE) &&
                (value & I2CD_INTR_RX_DONE);
        bus->intr_status &= ~(value & 0x7FFF);
        if (!bus->intr_status) {
            bus->controller->intr_status &= ~(1 << bus->id);
            qemu_irq_lower(aic->bus_get_irq(bus));
        }

        if (handle_rx) {
            if (bus->cmd & (I2CD_M_RX_CMD | I2CD_M_S_RX_CMD_LAST)) {
                aspeed_i2c_handle_rx_cmd(bus);
                aspeed_i2c_bus_raise_interrupt(bus);
            } else if (aspeed_i2c_get_state(bus) == I2CD_STXD) {
                i2c_ack(bus->bus);
            }
        }

        break;
    case I2CD_DEV_ADDR_REG:
        bus->dev_addr = value;
        break;
    case I2CD_POOL_CTRL_REG:
        bus->pool_ctrl &= ~0xffffff;
        bus->pool_ctrl |= (value & 0xffffff);
        break;

    case I2CD_BYTE_BUF_REG:
        bus->buf = (value & I2CD_BYTE_BUF_TX_MASK) << I2CD_BYTE_BUF_TX_SHIFT;
        break;
    case I2CD_CMD_REG:
        if (!aspeed_i2c_bus_is_enabled(bus)) {
            break;
        }

        if (!aspeed_i2c_bus_is_master(bus)) {
            qemu_log_mask(LOG_UNIMP, "%s: slave mode not implemented\n",
                          __func__);
            break;
        }

        if (!aic->has_dma &&
            value & (I2CD_RX_DMA_ENABLE | I2CD_TX_DMA_ENABLE)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }

        aspeed_i2c_bus_handle_cmd(bus, value);
        aspeed_i2c_bus_raise_interrupt(bus);
        break;
    case I2CD_DMA_ADDR:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }

        bus->dma_addr = value & 0x3ffffffc;
        break;

    case I2CD_DMA_LEN:
        if (!aic->has_dma) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: No DMA support\n",  __func__);
            break;
        }

        bus->dma_len = value & 0xfff;
        if (!bus->dma_len) {
            qemu_log_mask(LOG_UNIMP, "%s: invalid DMA length\n",  __func__);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static void aspeed_i2c_bus_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    AspeedI2CBus *bus = opaque;
    if (aspeed_i2c_bus_is_new_mode(bus)) {
        return aspeed_i2c_bus_write_new(opaque, offset, value, size);
    } else {
        return aspeed_i2c_bus_write_old(opaque, offset, value, size);
    }
}

static uint64_t aspeed_i2c_ctrl_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    AspeedI2CState *s = opaque;

    switch (offset) {
    case I2C_CTRL_STATUS:
        return s->intr_status;
    case I2C_CTRL_GLOBAL:
        return s->ctrl_global;
    case I2C_NEW_DIV_GLOBAL:
        return s->new_divider;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }

    return -1;
}

static void aspeed_i2c_ctrl_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    AspeedI2CState *s = opaque;

    switch (offset) {
    case I2C_CTRL_GLOBAL:
        s->ctrl_global = value;
        break;
    case I2C_NEW_DIV_GLOBAL:
        s->new_divider = value;
        break;
    case I2C_CTRL_STATUS:
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps aspeed_i2c_bus_ops = {
    .read = aspeed_i2c_bus_read,
    .write = aspeed_i2c_bus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps aspeed_i2c_ctrl_ops = {
    .read = aspeed_i2c_ctrl_read,
    .write = aspeed_i2c_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t aspeed_i2c_pool_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    AspeedI2CState *s = opaque;
    uint64_t ret = 0;
    int i;

    for (i = 0; i < size; i++) {
        ret |= (uint64_t) s->pool[offset + i] << (8 * i);
    }

    return ret;
}

static void aspeed_i2c_pool_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    AspeedI2CState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        s->pool[offset + i] = (value >> (8 * i)) & 0xFF;
    }
}

static const MemoryRegionOps aspeed_i2c_pool_ops = {
    .read = aspeed_i2c_pool_read,
    .write = aspeed_i2c_pool_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const VMStateDescription aspeed_i2c_bus_vmstate = {
    .name = TYPE_ASPEED_I2C,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(id, AspeedI2CBus),
        VMSTATE_UINT32(ctrl, AspeedI2CBus),
        VMSTATE_UINT32_ARRAY(timing, AspeedI2CBus, 2),
        VMSTATE_UINT32(intr_ctrl, AspeedI2CBus),
        VMSTATE_UINT32(intr_status, AspeedI2CBus),
        VMSTATE_UINT32(cmd, AspeedI2CBus),
        VMSTATE_UINT32(buf, AspeedI2CBus),
        VMSTATE_UINT32(pool_ctrl, AspeedI2CBus),
        VMSTATE_UINT32(dma_addr, AspeedI2CBus),
        VMSTATE_UINT32(dma_len, AspeedI2CBus),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription aspeed_i2c_vmstate = {
    .name = TYPE_ASPEED_I2C,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(intr_status, AspeedI2CState),
        VMSTATE_STRUCT_ARRAY(busses, AspeedI2CState,
                             ASPEED_I2C_NR_BUSSES, 1, aspeed_i2c_bus_vmstate,
                             AspeedI2CBus),
        VMSTATE_UINT8_ARRAY(pool, AspeedI2CState, ASPEED_I2C_MAX_POOL_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_i2c_reset(DeviceState *dev)
{
    AspeedI2CState *s = ASPEED_I2C(dev);

    s->intr_status = 0;
}

static void aspeed_i2c_instance_init(Object *obj)
{
    AspeedI2CState *s = ASPEED_I2C(obj);
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(s);
    int i;

    for (i = 0; i < aic->num_busses; i++) {
        object_initialize_child(obj, "bus[*]", &s->busses[i],
                                TYPE_ASPEED_I2C_BUS);
    }
}

/*
 * Address Definitions (AST2400 and AST2500)
 *
 *   0x000 ... 0x03F: Global Register
 *   0x040 ... 0x07F: Device 1
 *   0x080 ... 0x0BF: Device 2
 *   0x0C0 ... 0x0FF: Device 3
 *   0x100 ... 0x13F: Device 4
 *   0x140 ... 0x17F: Device 5
 *   0x180 ... 0x1BF: Device 6
 *   0x1C0 ... 0x1FF: Device 7
 *   0x200 ... 0x2FF: Buffer Pool  (unused in linux driver)
 *   0x300 ... 0x33F: Device 8
 *   0x340 ... 0x37F: Device 9
 *   0x380 ... 0x3BF: Device 10
 *   0x3C0 ... 0x3FF: Device 11
 *   0x400 ... 0x43F: Device 12
 *   0x440 ... 0x47F: Device 13
 *   0x480 ... 0x4BF: Device 14
 *   0x800 ... 0xFFF: Buffer Pool  (unused in linux driver)
 */
static void aspeed_i2c_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AspeedI2CState *s = ASPEED_I2C(dev);
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(s);

    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_i2c_ctrl_ops, s,
                          "aspeed.i2c", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < aic->num_busses; i++) {
        Object *bus = OBJECT(&s->busses[i]);
        int offset = i < aic->gap ? 1 : 5;

        if (!object_property_set_link(bus, "controller", OBJECT(s), errp)) {
            return;
        }

        if (!object_property_set_uint(bus, "bus-id", i, errp)) {
            return;
        }

        if (!sysbus_realize(SYS_BUS_DEVICE(bus), errp)) {
            return;
        }

        memory_region_add_subregion(&s->iomem, aic->reg_size * (i + offset),
                                    &s->busses[i].mr);
    }

    memory_region_init_io(&s->pool_iomem, OBJECT(s), &aspeed_i2c_pool_ops, s,
                          "aspeed.i2c-pool", aic->pool_size);
    memory_region_add_subregion(&s->iomem, aic->pool_base, &s->pool_iomem);

    if (aic->has_dma) {
        if (!s->dram_mr) {
            error_setg(errp, TYPE_ASPEED_I2C ": 'dram' link not set");
            return;
        }

        address_space_init(&s->dram_as, s->dram_mr,
                           TYPE_ASPEED_I2C "-dma-dram");
    }
}

static Property aspeed_i2c_properties[] = {
    DEFINE_PROP_LINK("dram", AspeedI2CState, dram_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void aspeed_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &aspeed_i2c_vmstate;
    dc->reset = aspeed_i2c_reset;
    device_class_set_props(dc, aspeed_i2c_properties);
    dc->realize = aspeed_i2c_realize;
    dc->desc = "Aspeed I2C Controller";
}

static const TypeInfo aspeed_i2c_info = {
    .name          = TYPE_ASPEED_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = aspeed_i2c_instance_init,
    .instance_size = sizeof(AspeedI2CState),
    .class_init    = aspeed_i2c_class_init,
    .class_size = sizeof(AspeedI2CClass),
    .abstract   = true,
};

static int aspeed_i2c_slave_event(I2CSlave *slave, enum i2c_event event)
{
    AspeedI2CSlave *s = ASPEED_I2C_SLAVE(slave);
    AspeedI2CBus *bus = s->bus;

    switch (event) {
    case I2C_START_SEND:
        bus->buf = bus->dev_addr << 1;

        bus->buf &= I2CD_BYTE_BUF_RX_MASK;
        bus->buf <<= I2CD_BYTE_BUF_RX_SHIFT;

        bus->intr_status |= (I2CD_INTR_SLAVE_ADDR_RX_MATCH | I2CD_INTR_RX_DONE);
        aspeed_i2c_set_state(bus, I2CD_STXD);

        break;

    case I2C_FINISH:
        bus->intr_status |= I2CD_INTR_NORMAL_STOP;
        aspeed_i2c_set_state(bus, I2CD_IDLE);

        break;

    default:
        return -1;
    }

    aspeed_i2c_bus_raise_interrupt(bus);

    return 0;
}

static void aspeed_i2c_slave_send_async(I2CSlave *slave, uint8_t data)
{
    AspeedI2CSlave *s = ASPEED_I2C_SLAVE(slave);
    AspeedI2CBus *bus = s->bus;

    bus->buf = (data & I2CD_BYTE_BUF_RX_MASK) << I2CD_BYTE_BUF_RX_SHIFT;
    bus->intr_status |= I2CD_INTR_RX_DONE;

    aspeed_i2c_bus_raise_interrupt(bus);
}

static void aspeed_i2c_slave_class_init(ObjectClass *klass, void *Data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    dc->desc = "Aspeed I2C Bus Slave";

    sc->event = aspeed_i2c_slave_event;
    sc->send_async = aspeed_i2c_slave_send_async;
}

static const TypeInfo aspeed_i2c_slave_info = {
    .name          = TYPE_ASPEED_I2C_SLAVE,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AspeedI2CSlave),
    .class_init    = aspeed_i2c_slave_class_init,
};

static void aspeed_i2c_bus_reset(DeviceState *dev)
{
    AspeedI2CBus *s = ASPEED_I2C_BUS(dev);

    s->intr_ctrl = 0;
    s->intr_status = 0;
    s->dev_addr = 0;
    s->cmd = 0;
    s->buf = 0;
    s->dma_addr = 0;
    s->dma_len = 0;
    i2c_end_transfer(s->bus);
}

static void aspeed_i2c_bus_realize(DeviceState *dev, Error **errp)
{
    AspeedI2CBus *s = ASPEED_I2C_BUS(dev);
    AspeedI2CClass *aic;
    g_autofree char *name = g_strdup_printf(TYPE_ASPEED_I2C_BUS ".%d", s->id);

    if (!s->controller) {
        error_setg(errp, TYPE_ASPEED_I2C_BUS ": 'controller' link not set");
        return;
    }

    aic = ASPEED_I2C_GET_CLASS(s->controller);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->bus = i2c_init_bus(dev, name);
    s->slave = ASPEED_I2C_SLAVE(i2c_slave_create_simple(s->bus, TYPE_ASPEED_I2C_SLAVE, 0xff));
    s->slave->bus = s;

    memory_region_init_io(&s->mr, OBJECT(s), &aspeed_i2c_bus_ops,
                          s, name, aic->reg_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static Property aspeed_i2c_bus_properties[] = {
    DEFINE_PROP_UINT8("bus-id", AspeedI2CBus, id, 0),
    DEFINE_PROP_LINK("controller", AspeedI2CBus, controller, TYPE_ASPEED_I2C,
                     AspeedI2CState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void aspeed_i2c_bus_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Aspeed I2C Bus";
    dc->realize = aspeed_i2c_bus_realize;
    dc->reset = aspeed_i2c_bus_reset;
    device_class_set_props(dc, aspeed_i2c_bus_properties);
}

static const TypeInfo aspeed_i2c_bus_info = {
    .name           = TYPE_ASPEED_I2C_BUS,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(AspeedI2CBus),
    .class_init     = aspeed_i2c_bus_class_init,
};

static qemu_irq aspeed_2400_i2c_bus_get_irq(AspeedI2CBus *bus)
{
    return bus->controller->irq;
}

static uint8_t *aspeed_2400_i2c_bus_pool_base(AspeedI2CBus *bus)
{
    uint8_t *pool_page =
        &bus->controller->pool[I2CD_POOL_PAGE_SEL(bus->ctrl) * 0x100];

    return &pool_page[I2CD_POOL_OFFSET(bus->pool_ctrl)];
}

static void aspeed_2400_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AspeedI2CClass *aic = ASPEED_I2C_CLASS(klass);

    dc->desc = "ASPEED 2400 I2C Controller";

    aic->num_busses = 14;
    aic->reg_size = 0x40;
    aic->gap = 7;
    aic->bus_get_irq = aspeed_2400_i2c_bus_get_irq;
    aic->pool_size = 0x800;
    aic->pool_base = 0x800;
    aic->bus_pool_base = aspeed_2400_i2c_bus_pool_base;
}

static const TypeInfo aspeed_2400_i2c_info = {
    .name = TYPE_ASPEED_2400_I2C,
    .parent = TYPE_ASPEED_I2C,
    .class_init = aspeed_2400_i2c_class_init,
};

static qemu_irq aspeed_2500_i2c_bus_get_irq(AspeedI2CBus *bus)
{
    return bus->controller->irq;
}

static uint8_t *aspeed_2500_i2c_bus_pool_base(AspeedI2CBus *bus)
{
    return &bus->controller->pool[bus->id * 0x10];
}

static void aspeed_2500_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AspeedI2CClass *aic = ASPEED_I2C_CLASS(klass);

    dc->desc = "ASPEED 2500 I2C Controller";

    aic->num_busses = 14;
    aic->reg_size = 0x40;
    aic->gap = 7;
    aic->bus_get_irq = aspeed_2500_i2c_bus_get_irq;
    aic->pool_size = 0x100;
    aic->pool_base = 0x200;
    aic->bus_pool_base = aspeed_2500_i2c_bus_pool_base;
    aic->check_sram = true;
    aic->has_dma = true;
}

static const TypeInfo aspeed_2500_i2c_info = {
    .name = TYPE_ASPEED_2500_I2C,
    .parent = TYPE_ASPEED_I2C,
    .class_init = aspeed_2500_i2c_class_init,
};

static qemu_irq aspeed_2600_i2c_bus_get_irq(AspeedI2CBus *bus)
{
    return bus->irq;
}

static uint8_t *aspeed_2600_i2c_bus_pool_base(AspeedI2CBus *bus)
{
   return &bus->controller->pool[bus->id * 0x20];
}

static void aspeed_2600_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AspeedI2CClass *aic = ASPEED_I2C_CLASS(klass);

    dc->desc = "ASPEED 2600 I2C Controller";

    aic->num_busses = 16;
    aic->reg_size = 0x80;
    aic->gap = -1; /* no gap */
    aic->bus_get_irq = aspeed_2600_i2c_bus_get_irq;
    aic->pool_size = 0x200;
    aic->pool_base = 0xC00;
    aic->bus_pool_base = aspeed_2600_i2c_bus_pool_base;
    aic->has_dma = true;
}

static const TypeInfo aspeed_2600_i2c_info = {
    .name = TYPE_ASPEED_2600_I2C,
    .parent = TYPE_ASPEED_I2C,
    .class_init = aspeed_2600_i2c_class_init,
};

static void aspeed_1030_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AspeedI2CClass *aic = ASPEED_I2C_CLASS(klass);

    dc->desc = "ASPEED 1030 I2C Controller";

    aic->num_busses = 14;
    aic->reg_size = 0x80;
    aic->gap = -1; /* no gap */
    aic->bus_get_irq = aspeed_2600_i2c_bus_get_irq;
    aic->pool_size = 0x200;
    aic->pool_base = 0xC00;
    aic->bus_pool_base = aspeed_2600_i2c_bus_pool_base;
    aic->has_dma = true;
}

static const TypeInfo aspeed_1030_i2c_info = {
    .name = TYPE_ASPEED_1030_I2C,
    .parent = TYPE_ASPEED_I2C,
    .class_init = aspeed_1030_i2c_class_init,
};

static void aspeed_i2c_register_types(void)
{
    type_register_static(&aspeed_i2c_bus_info);
    type_register_static(&aspeed_i2c_slave_info);
    type_register_static(&aspeed_i2c_info);
    type_register_static(&aspeed_2400_i2c_info);
    type_register_static(&aspeed_2500_i2c_info);
    type_register_static(&aspeed_2600_i2c_info);
    type_register_static(&aspeed_1030_i2c_info);
}

type_init(aspeed_i2c_register_types)


I2CBus *aspeed_i2c_get_bus(AspeedI2CState *s, int busnr)
{
    AspeedI2CClass *aic = ASPEED_I2C_GET_CLASS(s);
    I2CBus *bus = NULL;

    if (busnr >= 0 && busnr < aic->num_busses) {
        bus = s->busses[busnr].bus;
    }

    return bus;
}
