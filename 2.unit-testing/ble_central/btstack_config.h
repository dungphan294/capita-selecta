#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/*
 * BTstack ships no defaults — every project supplies this file.
 *
 * This one is configured for a *central with a GATT client*, which is the
 * difference between this project and the plain scanner next door. The
 * scanner needs only ENABLE_LE_CENTRAL; reading values additionally needs
 * the GATT client, pairing, and the ATT buffers to hold a response.
 */

/* Roles and features */
#define ENABLE_LE_CENTRAL                  /* scan and initiate connections  */
#define ENABLE_GATT_CLIENT_PAIRING         /* pair when a read is refused    */
#define ENABLE_LE_DATA_LENGTH_EXTENSION    /* larger packets where supported */
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

/* No ENABLE_LE_PERIPHERAL here: this build never advertises. A central is a
 * receive-and-initiate role and is deliberately invisible to other scanners. */

/* Buffers and limits */
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS  1
#define MAX_NR_L2CAP_SERVICES  1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4
#define MAX_NR_GATT_CLIENTS 1
#define MAX_ATT_DB_SIZE 512

/* Keep the stack from overrunning the CYW43 shared SPI bus */
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

/* Bonding keys survive a power cycle only if there is somewhere to put them */
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

/* Pico SDK HAL */
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT

#endif /* BTSTACK_CONFIG_H */
