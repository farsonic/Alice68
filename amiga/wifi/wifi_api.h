/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device's own commands, used by C:Alice68-WiFi. They sit in the
 * third-party command area next to netdev's 0x8900 block. The structures are
 * built by the same compiler on both sides (pack 2, as AmigaOS). */
#ifndef ALICE68_WIFI_API_H
#define ALICE68_WIFI_API_H

#define WIFI_CMD_STATUS     0x8a40      /* io_Data -> struct WifiStatus */
#define WIFI_CMD_SCAN       0x8a41      /* io_Data -> struct WifiScanEntry[io_Length / size]; io_Actual = count */
#define WIFI_CMD_LOG        0x8a42      /* io_Data -> text buffer; io_Actual = bytes */
#define WIFI_CMD_RECONNECT  0x8a43      /* read the prefs again and connect */
#define WIFI_CMD_DISCONNECT 0x8a44
#define WIFI_CMD_OFF        0x8a45      /* switch the chip off; the driver ends when the last user closes it */
#define WIFI_CMD_START      0x8a46      /* start the chip (opening the device does not) */

#define WIFI_API_VERSION    1

enum { WS_OFF, WS_STARTING, WS_IDLE, WS_SCANNING, WS_JOINING, WS_CONNECTED, WS_RETRY, WS_FAILED };

struct WifiStatus {
    UWORD ws_Version;
    UWORD ws_State;             /* WS_* */
    UWORD ws_Chip, ws_ChipRev;
    UBYTE ws_Mac[6];
    UBYTE ws_Bssid[6];
    char  ws_Ssid[34];
    WORD  ws_Rssi;
    UWORD ws_Channel;
    UWORD ws_RateMbps;
    UWORD ws_Security;          /* SEC_* of the network in use */
    UWORD ws_Interrupts;        /* 1: card interrupt, 0: polling */
    UWORD ws_SdMhz;
    UWORD ws_Attached;          /* a netdev stack is attached */
    ULONG ws_RxPackets, ws_TxPackets, ws_RxBytes, ws_TxBytes, ws_RxDropped, ws_TxErrors, ws_SdErrors, ws_Irqs;
    char  ws_FwVersion[96];
    char  ws_Message[96];       /* what happened last: why it is not connected, etc. */
};

struct WifiScanEntry {
    UBYTE we_Bssid[6];
    char  we_Ssid[34];
    WORD  we_Rssi;
    UWORD we_Channel;
    UWORD we_Security;
};
#endif
