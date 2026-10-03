// SPDX-License-Identifier: Apache-2.0 OR MIT
// Semtech SX1261/SX1262/SX1268 and LLCC68 sub-GHz transceivers. Opcodes,
// registers and encodings from DS_SX1261-2 rev 2.1, sections 12 and 13.

#ifndef MCLW_SX126X_H
#define MCLW_SX126X_H

#include <stdint.h>

#include "metalclaw/model.h"

// Section 13: commands.
#define SX126X_OP_RESET_STATS 0x00u
#define SX126X_OP_CLEAR_IRQ_STATUS 0x02u
#define SX126X_OP_CLEAR_DEVICE_ERRORS 0x07u
#define SX126X_OP_SET_DIO_IRQ_PARAMS 0x08u
#define SX126X_OP_WRITE_REGISTER 0x0Du
#define SX126X_OP_WRITE_BUFFER 0x0Eu
#define SX126X_OP_GET_STATS 0x10u
#define SX126X_OP_GET_PACKET_TYPE 0x11u
#define SX126X_OP_GET_IRQ_STATUS 0x12u
#define SX126X_OP_GET_RX_BUFFER_STATUS 0x13u
#define SX126X_OP_GET_PACKET_STATUS 0x14u
#define SX126X_OP_GET_RSSI_INST 0x15u
#define SX126X_OP_GET_DEVICE_ERRORS 0x17u
#define SX126X_OP_READ_REGISTER 0x1Du
#define SX126X_OP_READ_BUFFER 0x1Eu
#define SX126X_OP_SET_STANDBY 0x80u
#define SX126X_OP_SET_RX 0x82u
#define SX126X_OP_SET_TX 0x83u
#define SX126X_OP_SET_SLEEP 0x84u
#define SX126X_OP_SET_RF_FREQUENCY 0x86u
#define SX126X_OP_SET_CAD_PARAMS 0x88u
#define SX126X_OP_CALIBRATE 0x89u
#define SX126X_OP_SET_PACKET_TYPE 0x8Au
#define SX126X_OP_SET_MODULATION_PARAMS 0x8Bu
#define SX126X_OP_SET_PACKET_PARAMS 0x8Cu
#define SX126X_OP_SET_TX_PARAMS 0x8Eu
#define SX126X_OP_SET_BUFFER_BASE_ADDRESS 0x8Fu
#define SX126X_OP_SET_RX_TX_FALLBACK_MODE 0x93u
#define SX126X_OP_SET_RX_DUTY_CYCLE 0x94u
#define SX126X_OP_SET_PA_CONFIG 0x95u
#define SX126X_OP_SET_REGULATOR_MODE 0x96u
#define SX126X_OP_SET_DIO3_AS_TCXO_CTRL 0x97u
#define SX126X_OP_CALIBRATE_IMAGE 0x98u
#define SX126X_OP_SET_DIO2_AS_RF_SWITCH_CTRL 0x9Du
#define SX126X_OP_STOP_TIMER_ON_PREAMBLE 0x9Fu
#define SX126X_OP_SET_LORA_SYMB_NUM_TIMEOUT 0xA0u
#define SX126X_OP_GET_STATUS 0xC0u
#define SX126X_OP_SET_FS 0xC1u
#define SX126X_OP_SET_CAD 0xC5u
#define SX126X_OP_SET_TX_CONTINUOUS_WAVE 0xD1u
#define SX126X_OP_SET_TX_INFINITE_PREAMBLE 0xD2u

// Section 13.5.1: status byte, chip mode in bits 6:4, command status in 3:1.
// Bit 7, reserved, reads 1 on a real SX1262 on every byte that carries the
// status, and 0 in the GetStatus response byte.
#define SX126X_STATUS_RESERVED 0x80u
#define SX126X_STATUS_MODE_STBY_RC 0x2u
#define SX126X_STATUS_MODE_STBY_XOSC 0x3u
#define SX126X_STATUS_MODE_FS 0x4u
#define SX126X_STATUS_MODE_RX 0x5u
#define SX126X_STATUS_MODE_TX 0x6u
#define SX126X_STATUS_CMD_OK 0x1u
#define SX126X_STATUS_CMD_DATA_AVAILABLE 0x2u
#define SX126X_STATUS_CMD_PROCESSING_ERROR 0x4u
#define SX126X_STATUS_CMD_EXEC_FAILURE 0x5u
#define SX126X_STATUS_CMD_TX_DONE 0x6u
// What MISO carries while the part is asleep or held in reset.
#define SX126X_STATUS_ABSENT 0xFFu

// ResetStats takes six zero bytes; RadioLib clocks the opcode alone to wake
// the part, and the part ignores it.
#define SX126X_RESET_STATS_PARAMS 6u

// Section 13.3.1: IRQ bits.
#define SX126X_IRQ_TX_DONE 0x0001u
#define SX126X_IRQ_RX_DONE 0x0002u
#define SX126X_IRQ_PREAMBLE_DETECTED 0x0004u
#define SX126X_IRQ_HEADER_VALID 0x0010u
#define SX126X_IRQ_HEADER_ERR 0x0020u
#define SX126X_IRQ_CRC_ERR 0x0040u
#define SX126X_IRQ_CAD_DONE 0x0080u
#define SX126X_IRQ_CAD_DETECTED 0x0100u
#define SX126X_IRQ_TIMEOUT 0x0200u
#define SX126X_IRQ_ALL 0x43FFu

// GetDeviceErrors (OpError) bits.
#define SX126X_DEVICE_ERROR_XOSC_START 0x0020u

#define SX126X_PACKET_TYPE_GFSK 0x00u
#define SX126X_PACKET_TYPE_LORA 0x01u

// SetRx/SetTx timeouts and RxDutyCycle periods count 15.625 us steps.
#define SX126X_TIMEOUT_STEP_NUM 125u
#define SX126X_TIMEOUT_STEP_DEN 8u
#define SX126X_RX_CONTINUOUS 0xFFFFFFu

// RF frequency step: 32 MHz / 2^25 = 15625 / 16384 Hz.
#define SX126X_FREQ_STEP_NUM 15625u
#define SX126X_FREQ_STEP_SHIFT 14u

// Section 12: registers the drivers touch, and their reset values.
#define SX126X_REG_RETENTION_LIST 0x029Fu
#define SX126X_REG_VERSION 0x0320u
#define SX126X_VERSION_LENGTH 16u
#define SX126X_REG_WHITENING 0x06B8u
#define SX126X_REG_CRC_INIT 0x06BCu
#define SX126X_REG_CRC_POLY 0x06BEu
#define SX126X_REG_GFSK_SYNC_WORD 0x06C0u
#define SX126X_REG_IQ_POLARITY 0x0736u
#define SX126X_IQ_POLARITY_RESET 0x0Du
#define SX126X_REG_LORA_SYNC_WORD 0x0740u
#define SX126X_LORA_SYNC_WORD_PRIVATE 0x1424u
#define SX126X_REG_RANDOM 0x0819u
#define SX126X_RANDOM_LENGTH 4u
#define SX126X_REG_TX_MODULATION 0x0889u
#define SX126X_TX_MODULATION_RESET 0x04u
#define SX126X_REG_RX_GAIN 0x08ACu
#define SX126X_RX_GAIN_RESET 0x94u
// Undocumented. Meshtastic sets bit 0 for its RX fix; on a real SX1262 it
// reads 0x04 and every Calibrate clears bit 0 again.
#define SX126X_REG_RX_PATCH 0x08B5u
#define SX126X_RX_PATCH_RESET 0x04u
#define SX126X_RX_PATCH_CALIBRATE_CLEARS 0x01u
#define SX126X_REG_TX_CLAMP 0x08D8u
#define SX126X_TX_CLAMP_RESET 0xC8u
#define SX126X_REG_OCP 0x08E7u
#define SX126X_OCP_SX1261 0x18u
#define SX126X_OCP_SX1262 0x38u
#define SX126X_REG_XTA_TRIM 0x0911u
#define SX126X_REG_XTB_TRIM 0x0912u
#define SX126X_XTAL_TRIM_RESET 0x05u
#define SX126X_XTAL_TRIM_TCXO 0x2Fu

// Section 13.4.5: LoRa bandwidth codes.
#define SX126X_BW_7 0x00u
#define SX126X_BW_10 0x08u
#define SX126X_BW_15 0x01u
#define SX126X_BW_20 0x09u
#define SX126X_BW_31 0x02u
#define SX126X_BW_41 0x0Au
#define SX126X_BW_62 0x03u
#define SX126X_BW_125 0x04u
#define SX126X_BW_250 0x05u
#define SX126X_BW_500 0x06u

#define SX126X_PAYLOAD_MAX 255u

extern const struct mclw_model mclw_model_sx1262;
extern const struct mclw_model mclw_model_sx1261;
extern const struct mclw_model mclw_model_sx1268;
extern const struct mclw_model mclw_model_llcc68;

#endif
