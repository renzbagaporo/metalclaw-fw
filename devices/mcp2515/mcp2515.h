// SPDX-License-Identifier: Apache-2.0 OR MIT
// Microchip MCP2515 stand-alone CAN controller: SPI instructions and the
// registers the model uses. Section and table numbers are the datasheet's,
// DS20001801J.

#ifndef MCLW_DEVICE_MCP2515_H
#define MCLW_DEVICE_MCP2515_H

// Table 12-1, SPI instruction set.
#define MCP2515_RESET 0xc0
#define MCP2515_READ 0x03
#define MCP2515_READ_RX_BUFFER 0x90 // 1001 0nm0
#define MCP2515_WRITE 0x02
#define MCP2515_LOAD_TX_BUFFER 0x40 // 0100 0abc
#define MCP2515_RTS 0x80 // 1000 0nnn
#define MCP2515_READ_STATUS 0xa0
#define MCP2515_RX_STATUS 0xb0
#define MCP2515_BIT_MODIFY 0x05

// Table 11-1, register map (addresses).
#define MCP2515_BFPCTRL 0x0c
#define MCP2515_TXRTSCTRL 0x0d
#define MCP2515_CANSTAT 0x0e // and every xEh
#define MCP2515_CANCTRL 0x0f // and every xFh
#define MCP2515_TEC 0x1c
#define MCP2515_REC 0x1d
#define MCP2515_RXM0SIDH 0x20
#define MCP2515_RXM1SIDH 0x24
#define MCP2515_CNF3 0x28
#define MCP2515_CNF2 0x29
#define MCP2515_CNF1 0x2a
#define MCP2515_CANINTE 0x2b
#define MCP2515_CANINTF 0x2c
#define MCP2515_EFLG 0x2d
#define MCP2515_TXB0CTRL 0x30 // TXBn at 30h + 10h * n
#define MCP2515_RXB0CTRL 0x60
#define MCP2515_RXB1CTRL 0x70

// Register 3-1, TXBnCTRL.
#define MCP2515_TXB_ABTF 0x40
#define MCP2515_TXB_TXERR 0x10
#define MCP2515_TXB_TXREQ 0x08
// Registers 4-1 and 4-2, RXBnCTRL.
#define MCP2515_RXB_RXM_ANY 0x60
#define MCP2515_RXB_RXRTR 0x08
#define MCP2515_RXB0_BUKT 0x04
#define MCP2515_RXB0_BUKT1 0x02
// Register 7-2, CANINTF (and CANINTE, 7-1).
#define MCP2515_INT_RX0 0x01
#define MCP2515_INT_RX1 0x02
#define MCP2515_INT_TX0 0x04
#define MCP2515_INT_ERR 0x20
// Register 6-3, EFLG.
#define MCP2515_EFLG_EWARN 0x01
#define MCP2515_EFLG_TXWAR 0x04
#define MCP2515_EFLG_TXEP 0x10
// Registers 10-1 and 10-2, CANCTRL and CANSTAT.
#define MCP2515_CANCTRL_REQOP_SHIFT 5
#define MCP2515_CANCTRL_ABAT 0x10
#define MCP2515_CANCTRL_RESET 0x87
#define MCP2515_MODE_NORMAL 0
#define MCP2515_MODE_SLEEP 1
#define MCP2515_MODE_LOOPBACK 2
#define MCP2515_MODE_LISTEN_ONLY 3
#define MCP2515_MODE_CONFIG 4
// The SIDL bit that marks an extended identifier, in buffers and filters.
#define MCP2515_SIDL_IDE 0x08
#define MCP2515_SIDL_SRR 0x10
#define MCP2515_DLC_RTR 0x40

#endif
