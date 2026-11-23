/* SPDX-License-Identifier: Apache-2.0
 *
 * SAE J2534-1 v04.04 interface for libj2534_openport2.
 *
 * ABI: every J2534 "unsigned long" is 32 bits (uint32_t), as on Windows and as
 * the ~/.passthru drivers (e.g. mongoose) use. Plain cdecl exports.
 */
#ifndef LIBJ2534_OPENPORT2_J2534_H
#define LIBJ2534_OPENPORT2_J2534_H

#include <stdint.h>

#define PM_DATA_LEN 4128                /* max Data[] in a PASSTHRU_MSG */

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum j2534_error {
    ERR_NOERROR,
    ERR_NOT_SUPPORTED,
    ERR_INVALID_CHANNEL_ID,
    ERR_INVALID_PROTOCOL_ID,
    ERR_NULL_PARAMETER,
    ERR_INVALID_IOCTL_VALUE,
    ERR_INVALID_FLAGS,
    ERR_FAILED,
    ERR_DEVICE_NOT_CONNECTED,
    ERR_TIMEOUT,
    ERR_INVALID_MSG,
    ERR_INVALID_TIME_INTERVAL,
    ERR_EXCEEDED_LIMIT,
    ERR_INVALID_MSG_ID,
    ERR_DEVICE_IN_USE,
    ERR_INVALID_IOCTL_ID,
    ERR_BUFFER_EMPTY,
    ERR_BUFFER_FULL,
    ERR_BUFFER_OVERFLOW,
    ERR_PIN_INVALID,
    ERR_CHANNEL_IN_USE,
    ERR_MSG_PROTOCOL_ID,
    ERR_INVALID_FILTER_ID,
    ERR_NO_FLOW_CONTROL,
    ERR_NOT_UNIQUE,
    ERR_INVALID_BAUDRATE,
    ERR_INVALID_DEVICE_ID
};
#define STATUS_NOERROR ERR_NOERROR

/* protocol ids */
enum j2534_protocol {
    J1850VPW = 1, J1850PWM, ISO9141, ISO14230, CAN, ISO15765
};

enum j2534_ioctl {
    GET_CONFIG = 1,
    SET_CONFIG,
    READ_VBATT,
    FIVE_BAUD_INIT,
    FAST_INIT,
    CLEAR_TX_BUFFER = 7,
    CLEAR_RX_BUFFER,
    CLEAR_PERIODIC_MSGS,
    CLEAR_MSG_FILTERS,
    CLEAR_FUNCT_MSG_LOOKUP_TABLE,
    ADD_TO_FUNCT_MSG_LOOKUP_TABLE,
    DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE,
    READ_PROG_VOLTAGE
};

enum j2534_filter {
    PASS_FILTER = 1, BLOCK_FILTER, FLOW_CONTROL_FILTER
};

typedef struct { uint32_t Parameter, Value; } SCONFIG;
typedef struct { uint32_t NumOfParams; SCONFIG *ConfigPtr; } SCONFIG_LIST;

typedef struct {
    uint32_t ProtocolID;
    uint32_t RxStatus;
    uint32_t TxFlags;
    uint32_t Timestamp;
    uint32_t DataSize;
    uint32_t ExtraDataIndex;
    unsigned char Data[PM_DATA_LEN];
} PASSTHRU_MSG;

int32_t PassThruOpen(const void *pName, uint32_t *pDeviceID);
int32_t PassThruClose(uint32_t DeviceID);
int32_t PassThruConnect(uint32_t DeviceID, uint32_t ProtocolID,
                        uint32_t Flags, uint32_t Baudrate,
                        uint32_t *pChannelID);
int32_t PassThruDisconnect(uint32_t ChannelID);
int32_t PassThruReadMsgs(uint32_t ChannelID, PASSTHRU_MSG *pMsg,
                         uint32_t *pNumMsgs, uint32_t Timeout);
int32_t PassThruWriteMsgs(uint32_t ChannelID, const PASSTHRU_MSG *pMsg,
                          uint32_t *pNumMsgs, uint32_t Timeout);
int32_t PassThruStartPeriodicMsg(uint32_t ChannelID, const PASSTHRU_MSG *pMsg,
                                 uint32_t *pMsgID, uint32_t TimeInterval);
int32_t PassThruStopPeriodicMsg(uint32_t ChannelID, uint32_t MsgID);
int32_t PassThruStartMsgFilter(uint32_t ChannelID, uint32_t FilterType,
                               const PASSTHRU_MSG *pMaskMsg, const PASSTHRU_MSG *pPatternMsg,
                               const PASSTHRU_MSG *pFlowControlMsg, uint32_t *pFilterID);
int32_t PassThruStopMsgFilter(uint32_t ChannelID, uint32_t FilterID);
int32_t PassThruSetProgrammingVoltage(uint32_t DeviceID, uint32_t PinNumber,
                                      uint32_t Voltage);
int32_t PassThruReadVersion(uint32_t DeviceID, char *pFirmwareVersion,
                            char *pDllVersion, char *pApiVersion);
int32_t PassThruGetLastError(char *pErrorDescription);
int32_t PassThruIoctl(uint32_t ChannelID, uint32_t IoctlID,
                      const void *pInput, void *pOutput);

#ifdef __cplusplus
}
#endif
#endif
