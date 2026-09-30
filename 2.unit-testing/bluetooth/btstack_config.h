#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// BTstack has no defaults — every project must supply this file.
// This one is trimmed for an LE *central* (scanner) role.

// Features
#define ENABLE_LE_CENTRAL          // we scan for other devices
#define ENABLE_LE_PERIPHERAL       // ...and advertise ourselves so phones can see us
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// Buffers and limits
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS  1
#define MAX_NR_L2CAP_SERVICES  1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4
#define MAX_ATT_DB_SIZE 512

// Keep the stack from overrunning the CYW43 shared SPI bus
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Non-volatile storage sizing
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// Pico SDK HAL
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT

#endif // BTSTACK_CONFIG_H
