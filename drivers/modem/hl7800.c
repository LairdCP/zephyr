/*
 * Copyright (c) 2019 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_DOMAIN modem_hl7800
#define LOG_LEVEL CONFIG_MODEM_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_DOMAIN);

#include <zephyr/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <zephyr.h>
#include <gpio.h>
#include <device.h>
#include <init.h>

#include <net/net_if.h>
#include <net/net_context.h>
#include <net/net_offload.h>
#include <net/net_pkt.h>
#include <net/dns_resolve.h>
#if defined(CONFIG_NET_IPV6)
#include "ipv6.h"
#endif
#if defined(CONFIG_NET_IPV4)
#include "ipv4.h"
#endif
#if defined(CONFIG_NET_UDP)
#include "udp_internal.h"
#endif

#include <drivers/modem/modem_receiver.h>
#include <drivers/modem/hl7800.h>

/* Uncomment the #define below to enable a hexdump of all incoming
 * data from the modem receiver
 */
// #define ENABLE_VERBOSE_MODEM_RECV_HEXDUMP 1

#define LOG_UNHANDLED_RX_MSGS 1

enum sleep_state {
	ASLEEP,
	WAKING,
	AWAKE,
};

struct mdm_control_pinconfig {
	char *dev_name;
	u32_t pin;
	int config;
};

#define PINCONFIG(name_, pin_, config_)                                        \
	{                                                                      \
		.dev_name = name_, .pin = pin_, .config = config_              \
	}

/* pin settings */
enum mdm_control_pins {
	MDM_RESET = 0,
	MDM_WAKE,
	MDM_PWR_ON,
	MDM_FAST_SHUTD,
	MDM_UART_DTR,
	MDM_LTE_TX_ON,
	MDM_GPS_EXT_LNA_EN,
	MDM_VGPIO,
	MDM_UART_DSR,
	MAX_MDM_CONTROL_PINS,
};

enum net_operator_status { NO_OPERATOR, REGISTERED };

static const struct mdm_control_pinconfig pinconfig[] = {
	/* MDM_RESET */
	PINCONFIG(DT_SWI_HL7800_0_MDM_RESET_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_RESET_GPIOS_PIN,
		  (GPIO_DIR_OUT | GPIO_DS_DISCONNECT_HIGH)),

	/* MDM_WAKE */
	PINCONFIG(DT_SWI_HL7800_0_MDM_WAKE_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_WAKE_GPIOS_PIN,
		  (GPIO_DIR_OUT | GPIO_DS_DISCONNECT_LOW)),

	/* MDM_PWR_ON */
	PINCONFIG(DT_SWI_HL7800_0_MDM_PWR_ON_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_PWR_ON_GPIOS_PIN,
		  (GPIO_DIR_OUT | GPIO_DS_DISCONNECT_HIGH)),

	/* MDM_FAST_SHUTD */
	PINCONFIG(DT_SWI_HL7800_0_MDM_FAST_SHUTD_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_FAST_SHUTD_GPIOS_PIN,
		  (GPIO_DIR_OUT | GPIO_DS_DISCONNECT_HIGH)),

	/* MDM_UART_DTR */
	PINCONFIG(DT_SWI_HL7800_0_MDM_UART_DTR_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_UART_DTR_GPIOS_PIN,
		  (GPIO_DIR_OUT | GPIO_DS_DISCONNECT_HIGH)),

	/* MDM_LTE_TX_ON */
	PINCONFIG(DT_SWI_HL7800_0_MDM_LTE_TX_ON_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_LTE_TX_ON_GPIOS_PIN,
		  (GPIO_DIR_IN | GPIO_INT | GPIO_INT_EDGE |
		   GPIO_INT_DOUBLE_EDGE)),

	/* MDM_GPS_EXT_LNA_EN */
	PINCONFIG(DT_SWI_HL7800_0_MDM_GPS_EXT_LNA_EN_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_GPS_EXT_LNA_EN_GPIOS_PIN, GPIO_DIR_IN),

	/* MDM_VGPIO */
	PINCONFIG(DT_SWI_HL7800_0_MDM_VGPIO_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_VGPIO_GPIOS_PIN,
		  (GPIO_DIR_IN | GPIO_INT | GPIO_INT_EDGE |
		   GPIO_INT_DOUBLE_EDGE)),

	/* MDM_UART_DSR */
	PINCONFIG(DT_SWI_HL7800_0_MDM_UART_DSR_GPIOS_CONTROLLER,
		  DT_SWI_HL7800_0_MDM_UART_DSR_GPIOS_PIN,
		  (GPIO_DIR_IN | GPIO_INT | GPIO_INT_EDGE |
		   GPIO_INT_DOUBLE_EDGE)),
};

#define MDM_UART_DEV_NAME DT_SWI_HL7800_0_BUS_NAME

#define MDM_WAKE_ASSERTED 1 /* Asserted keeps the module awake */
#define MDM_WAKE_NOT_ASSERTED 0
#define MDM_RESET_ASSERTED 0
#define MDM_RESET_NOT_ASSERTED 1
#define MDM_PWR_ON_ASSERTED 0
#define MDM_PWR_ON_NOT_ASSERTED 1
#define MDM_FAST_SHUTD_ASSERTED 0
#define MDM_FAST_SHUTD_NOT_ASSERTED 1
#define MDM_UART_DTR_ASSERTED 0 /* Asserted keeps the module awake */
#define MDM_UART_DTR_NOT_ASSERTED 1

#define MDM_SEND_OK_ENABLED 0
#define MDM_SEND_OK_DISABLED 1

#define MDM_CMD_SEND_TIMEOUT K_SECONDS(5)
#define MDM_CMD_CONN_TIMEOUT_SECONDS 31
#define MDM_CMD_CONN_TIMEOUT K_SECONDS(MDM_CMD_CONN_TIMEOUT_SECONDS)

#define MDM_MAX_DATA_LENGTH 1500

#define MDM_RECV_MAX_BUF 30
#define MDM_RECV_BUF_SIZE 128

#define MDM_MAX_SOCKETS 10

#define BUF_ALLOC_TIMEOUT K_SECONDS(1)

#define CMD_HANDLER(cmd_, cb_)                                                 \
	{                                                                      \
		.cmd = cmd_, .cmd_len = (u16_t)sizeof(cmd_) - 1,               \
		.func = on_cmd_##cb_                                           \
	}

#define MDM_MANUFACTURER_LENGTH 16
#define MDM_MODEL_LENGTH 7
#define MDM_REVISION_LENGTH 25
#define MDM_IMEI_LENGTH 16
#define MDM_SN_LENGTH 15
#define MDM_NETWORK_STATUS_LENGTH 45

#define MDM_DEFAULT_AT_CMD_RETRIES 3
#define MDM_WAKEUP_TIME K_SECONDS(20)
#define MDM_BOOT_TIME K_SECONDS(12)

#define MDM_WAIT_FOR_DATA_TIME K_MSEC(50)
#define MDM_RESET_LOW_TIME K_MSEC(50)

#define RSSI_TIMEOUT_SECS 30

#define DNS_WORK_DELAY_SECS 1

NET_BUF_POOL_DEFINE(mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE, 0,
		    NULL);

static u8_t mdm_recv_buf[MDM_MAX_DATA_LENGTH];

/* RX thread structures */
K_THREAD_STACK_DEFINE(hl7800_rx_stack, CONFIG_MODEM_HL7800_RX_STACK_SIZE);
struct k_thread hl7800_rx_thread;
#define RX_THREAD_PRIORITY K_PRIO_COOP(7)

/* RX thread work queue */
K_THREAD_STACK_DEFINE(hl7800_workq_stack,
		      CONFIG_MODEM_HL7800_RX_WORKQ_STACK_SIZE);
static struct k_work_q hl7800_workq;
#define WORKQ_PRIORITY K_PRIO_COOP(7)

static const char EOF_PATTERN[] = "--EOF--Pattern--";
static const char CONNECT_STRING[] = "CONNECT";

struct hl7800_socket {
	struct net_context *context;
	sa_family_t family;
	enum net_sock_type type;
	enum net_ip_protocol ip_proto;
	struct sockaddr src;
	struct sockaddr dst;

	bool created;
	int socket_id;

	/** semaphore */
	struct k_sem sock_send_sem;

	/** socket callbacks */
	struct k_work recv_cb_work;
	net_context_recv_cb_t recv_cb;
	struct net_pkt *recv_pkt;
	void *recv_user_data;
};

struct hl7800_iface_ctx {
	struct net_if *iface;
	u8_t mac_addr[6];
	struct in_addr ipv4Addr, subnet, gateway, dns;
	bool restarting;
	bool initialized;
	char dns_string[sizeof("###.###.###.###")];

	/* GPIO PORT devices */
	struct device *gpio_port_dev[MAX_MDM_CONTROL_PINS];
	struct gpio_callback mdm_tx_on_cb;
	struct gpio_callback mdm_vgpio_cb;
	struct gpio_callback mdm_uart_dsr_cb;
	u8_t vgpio_state;
	u8_t dsr_state;

	/* RX specific attributes */
	struct mdm_receiver_context mdm_ctx;

	/* socket data */
	struct hl7800_socket sockets[MDM_MAX_SOCKETS];
	int last_socket_id;
	int last_error;

	/* semaphores */
	struct k_sem response_sem;
	struct k_sem mdm_awake;

	/* RSSI work */
	struct k_delayed_work rssi_query_work;

	/* work */
	struct k_work iface_status_work;
	struct k_delayed_work dns_work;

	/* modem data  */
	/* NOTE: make sure length is +1 for null char */
	char mdm_manufacturer[MDM_MANUFACTURER_LENGTH];
	char mdm_model[MDM_MODEL_LENGTH];
	char mdm_revision[MDM_REVISION_LENGTH];
	char mdm_imei[MDM_IMEI_LENGTH];
	char mdm_sn[MDM_SN_LENGTH];

	u8_t mdm_startup_state;
	char mdm_network_status[MDM_NETWORK_STATUS_LENGTH];

	/* modem state */
	int ev_csps;
	int ev_rrcstate;
	bool allowSleep;
	enum sleep_state sleepState;
	enum hl7800_network_state networkState;
	enum net_operator_status operatorStatus;
};

struct cmd_handler {
	const char *cmd;
	u16_t cmd_len;
	void (*func)(struct net_buf **buf, u16_t len);
};

static struct hl7800_iface_ctx ictx;

static void hl7800_read_rx(struct net_buf **buf);

/*** Verbose Debugging Functions ***/
#if defined(ENABLE_VERBOSE_MODEM_RECV_HEXDUMP)
static inline void _hexdump(const u8_t *packet, size_t length)
{
	char output[sizeof("xxxxyyyy xxxxyyyy")];
	int n = 0, k = 0;
	u8_t byte;

	while (length--) {
		if (n % 16 == 0) {
			printk(" %08X ", n);
		}

		byte = *packet++;

		printk("%02X ", byte);

		if (byte < 0x20 || byte > 0x7f) {
			output[k++] = '.';
		} else {
			output[k++] = byte;
		}

		n++;
		if (n % 8 == 0) {
			if (n % 16 == 0) {
				output[k] = '\0';
				printk(" [%s]\n", output);
				k = 0;
			} else {
				printk(" ");
			}
		}
	}

	if (n % 16) {
		int i;

		output[k] = '\0';

		for (i = 0; i < (16 - (n % 16)); i++) {
			printk("   ");
		}

		if ((n % 16) < 8) {
			printk(" "); /* one extra delimiter after 8 chars */
		}

		printk(" [%s]\n", output);
	}
}
#else
#define _hexdump(...)
#endif

static struct hl7800_socket *socket_get(void)
{
	int i;
	struct hl7800_socket *sock = NULL;

	for (i = 0; i < MDM_MAX_SOCKETS; i++) {
		if (!ictx.sockets[i].context) {
			sock = &ictx.sockets[i];
			break;
		}
	}

	return sock;
}

static struct hl7800_socket *socket_from_id(int socket_id)
{
	int i;
	struct hl7800_socket *sock = NULL;

	if (socket_id < 1) {
		return NULL;
	}

	for (i = 0; i < MDM_MAX_SOCKETS; i++) {
		if (ictx.sockets[i].socket_id == socket_id) {
			sock = &ictx.sockets[i];
			break;
		}
	}

	return sock;
}

static void socket_put(struct hl7800_socket *sock)
{
	if (!sock) {
		return;
	}

	sock->context = NULL;
	sock->socket_id = 0;
	sock->created = false;
	(void)memset(&sock->src, 0, sizeof(struct sockaddr));
	(void)memset(&sock->dst, 0, sizeof(struct sockaddr));
}

char *hl7800_sprint_ip_addr(const struct sockaddr *addr)
{
	static char buf[NET_IPV6_ADDR_LEN];

#if defined(CONFIG_NET_IPV6)
	if (addr->sa_family == AF_INET6) {
		return net_addr_ntop(AF_INET6, &net_sin6(addr)->sin6_addr, buf,
				     sizeof(buf));
	} else
#endif
#if defined(CONFIG_NET_IPV4)
		if (addr->sa_family == AF_INET) {
		return net_addr_ntop(AF_INET, &net_sin(addr)->sin_addr, buf,
				     sizeof(buf));
	} else
#endif
	{
		LOG_ERR("Unknown IP address family:%d", addr->sa_family);
		return NULL;
	}
}

static void modem_assert_wake(bool assert)
{
	if (assert) {
		LOG_DBG("MDM_WAKE_PIN -> ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_WAKE],
			       pinconfig[MDM_WAKE].pin, MDM_WAKE_ASSERTED);
	} else {
		LOG_DBG("MDM_WAKE_PIN -> NOT_ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_WAKE],
			       pinconfig[MDM_WAKE].pin, MDM_WAKE_NOT_ASSERTED);
	}
}

static void modem_assert_pwr_on(bool assert)
{
	if (assert) {
		LOG_DBG("MDM_PWR_ON -> ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_PWR_ON],
			       pinconfig[MDM_PWR_ON].pin, MDM_PWR_ON_ASSERTED);
	} else {
		LOG_DBG("MDM_PWR_ON -> NOT_ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_PWR_ON],
			       pinconfig[MDM_PWR_ON].pin,
			       MDM_PWR_ON_NOT_ASSERTED);
	}
}

static void modem_assert_fast_shutd(bool assert)
{
	if (assert) {
		LOG_DBG("MDM_FAST_SHUTD -> ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_FAST_SHUTD],
			       pinconfig[MDM_FAST_SHUTD].pin,
			       MDM_FAST_SHUTD_ASSERTED);
	} else {
		LOG_DBG("MDM_FAST_SHUTD -> NOT_ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_FAST_SHUTD],
			       pinconfig[MDM_FAST_SHUTD].pin,
			       MDM_FAST_SHUTD_NOT_ASSERTED);
	}
}

static void modem_assert_uart_dtr(bool assert)
{
	if (assert) {
		LOG_DBG("MDM_UART_DTR -> ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_UART_DTR],
			       pinconfig[MDM_UART_DTR].pin,
			       MDM_UART_DTR_ASSERTED);
	} else {
		LOG_DBG("MDM_UART_DTR -> NOT_ASSERTED");
		gpio_pin_write(ictx.gpio_port_dev[MDM_UART_DTR],
			       pinconfig[MDM_UART_DTR].pin,
			       MDM_UART_DTR_NOT_ASSERTED);
	}
}

static void allow_sleep(bool allow)
{
	if (allow) {
		LOG_INF("Allow sleep");
		ictx.allowSleep = true;
		modem_assert_wake(false);
		modem_assert_uart_dtr(false);
	} else {
		LOG_INF("Keep awake");
		ictx.allowSleep = false;
		modem_assert_wake(true);
		modem_assert_uart_dtr(true);
	}
}

void mdm_hl7800_wakeup(bool wakeup)
{
	allow_sleep(!wakeup);
}

/* Send an AT command with a series of response handlers */
static int send_at_cmd(struct hl7800_socket *sock, const u8_t *data,
		       int timeout, int retries)
{
	int ret;

	ictx.last_error = 0;

	do {
		if (!sock) {
			k_sem_reset(&ictx.response_sem);

		} else {
			k_sem_reset(&sock->sock_send_sem);
		}

		LOG_DBG("OUT: [%s]", log_strdup(data));
		mdm_receiver_send(&ictx.mdm_ctx, data, strlen(data));
		mdm_receiver_send(&ictx.mdm_ctx, "\r\n", 2);

		if (timeout == K_NO_WAIT) {
			return 0;
		}

		if (!sock) {
			ret = k_sem_take(&ictx.response_sem, timeout);
		} else {
			ret = k_sem_take(&sock->sock_send_sem, timeout);
		}

		if (ret == 0) {
			ret = ictx.last_error;
		} else if (ret == -EAGAIN) {
			ret = -ETIMEDOUT;
		}

		retries--;
		if (retries < 0) {
			retries = 0;
		}
		if (ret) {
			LOG_ERR("Err send %s: %d, tries left: %d",
				log_strdup(data), ret, retries);
		}
	} while (ret != 0 && retries > 0);

	return ret;
}

#ifdef CONFIG_MODEM_HL7800_LOW_POWER_MODE
static int wakeup_hl7800(void)
{
	int ret;
	allow_sleep(false);
	if (ictx.sleepState != AWAKE) {
		k_sem_reset(&ictx.mdm_awake);
		LOG_DBG("Waiting to wakeup");
		ret = k_sem_take(&ictx.mdm_awake, MDM_WAKEUP_TIME);
		if (ret) {
			LOG_ERR("Err waiting for wakeup: %d", ret);
		}
	}

	return 0;
}
#endif

s32_t mdm_hl7800_send_at_cmd(const u8_t *data)
{
	int ret;

	if (!data) {
		return -EINVAL;
	}

#ifdef CONFIG_MODEM_HL7800_LOW_POWER_MODE
	bool goBackToSleep = ictx.allowSleep;
	wakeup_hl7800();
#endif
	ictx.last_socket_id = 0;
	ret = send_at_cmd(NULL, data, MDM_CMD_SEND_TIMEOUT, 0);
#ifdef CONFIG_MODEM_HL7800_LOW_POWER_MODE
	if (goBackToSleep) {
		allow_sleep(true);
	}
#endif
	return ret;
}

static int send_data(struct hl7800_socket *sock, struct net_pkt *pkt)
{
	int ret, bufSize;
	struct net_buf *frag;
	char dstAddr[sizeof("###.###.###.###")];

	if (sock->type == SOCK_STREAM) {
		bufSize = sizeof("AT+KTCPSND=##,####\r\n");
	} else {
		bufSize = sizeof(
			"AT+KUDPSND=##,\"###.###.###.###\",#####,####\r\n");
	}

	char buf[bufSize];

	if (!sock) {
		return -EINVAL;
	}

	ictx.last_error = 0;

	frag = pkt->frags;

	/* start sending data */
	k_sem_reset(&sock->sock_send_sem);
	if (sock->type == SOCK_STREAM) {
		snprintk(buf, sizeof(buf), "AT+KTCPSND=%d,%u\r\n",
			 sock->socket_id, net_buf_frags_len(frag));
	} else {
		if (!net_addr_ntop(sock->family, &net_sin(&sock->dst)->sin_addr,
				   dstAddr, sizeof(dstAddr))) {
			LOG_ERR("Invalid dst addr");
			return -EINVAL;
		}
		snprintk(buf, sizeof(buf), "AT+KUDPSND=%d,\"%s\",%u,%u\r\n",
			 sock->socket_id, dstAddr,
			 net_sin(&sock->dst)->sin_port,
			 net_buf_frags_len(frag));
	}
	LOG_DBG("OUT: [%s]", log_strdup(buf));
	mdm_receiver_send(&ictx.mdm_ctx, buf, strlen(buf));

	/* wait for CONNECT */
	ret = k_sem_take(&sock->sock_send_sem, MDM_CMD_SEND_TIMEOUT);
	if (ret) {
		LOG_ERR("Err waiting for CONNECT (%d)", ret);
	}

	/* Loop through packet data and send */
	while (frag) {
		mdm_receiver_send(&ictx.mdm_ctx, frag->data, frag->len);
		frag = frag->frags;
	}

	/* Send EOF pattern to terminate data */
	k_sem_reset(&sock->sock_send_sem);
	mdm_receiver_send(&ictx.mdm_ctx, EOF_PATTERN, strlen(EOF_PATTERN));
	ret = k_sem_take(&sock->sock_send_sem, MDM_CMD_SEND_TIMEOUT);
	if (ret == 0) {
		ret = ictx.last_error;
	} else if (ret == -EAGAIN) {
		ret = -ETIMEDOUT;
	}

	return ret;
}

/*** NET_BUF HELPERS ***/

static bool is_crlf(u8_t c)
{
	if (c == '\n' || c == '\r') {
		return true;
	} else {
		return false;
	}
}

static void net_buf_skipcrlf(struct net_buf **buf)
{
	/* chop off any /n or /r */
	while (*buf && is_crlf(*(*buf)->data)) {
		net_buf_pull_u8(*buf);
		if (!(*buf)->len) {
			*buf = net_buf_frag_del(NULL, *buf);
		}
	}
}

static u16_t net_buf_findcrlf(struct net_buf *buf, struct net_buf **frag,
			      u16_t *offset)
{
	u16_t len = 0U, pos = 0U;

	while (buf && !is_crlf(*(buf->data + pos))) {
		if (pos + 1 >= buf->len) {
			len += buf->len;
			buf = buf->frags;
			pos = 0U;
		} else {
			pos++;
		}
	}

	if (buf && is_crlf(*(buf->data + pos))) {
		len += pos;
		*offset = pos;
		*frag = buf;
		return len;
	}

	return 0;
}

static u16_t net_buf_findchar(struct net_buf *buf, char ch,
			      struct net_buf **frag, u16_t *offset)
{
	u16_t len = 0U, pos = 0U;

	while (buf && *(buf->data + pos) != ch) {
		if (pos + 1 >= buf->len) {
			len += buf->len;
			buf = buf->frags;
			pos = 0U;
		} else {
			pos++;
		}
	}

	if (buf && *(buf->data + pos) == ch) {
		len += pos;
		*offset = pos;
		*frag = buf;
		return len;
	}

	return 0;
}

/*** UDP / TCP Helper Function ***/

/* Setup IP header data to be used by some network applications.
 * While much is dummy data, some fields such as dst, port and family are
 * important.
 * Return the IP + protocol header length.
 */
static int pkt_setup_ip_data(struct net_pkt *pkt, struct hl7800_socket *sock)
{
	int hdr_len = 0;
	u16_t src_port = 0U, dst_port = 0U;

#if defined(CONFIG_NET_IPV6)
	if (net_pkt_family(pkt) == AF_INET6) {
		if (net_ipv6_create(
			    pkt,
			    &((struct sockaddr_in6 *)&sock->dst)->sin6_addr,
			    &((struct sockaddr_in6 *)&sock->src)->sin6_addr)) {
			return -1;
		}

		hdr_len = sizeof(struct net_ipv6_hdr);
	} else
#endif
#if defined(CONFIG_NET_IPV4)
		if (net_pkt_family(pkt) == AF_INET) {
		if (net_ipv4_create(
			    pkt, &((struct sockaddr_in *)&sock->dst)->sin_addr,
			    &((struct sockaddr_in *)&sock->src)->sin_addr)) {
			return -1;
		}

		hdr_len = sizeof(struct net_ipv4_hdr);
	}
#endif

#if defined(CONFIG_NET_UDP)
	if (sock->ip_proto == IPPROTO_UDP) {
		if (net_udp_create(pkt, src_port, dst_port)) {
			return -1;
		}

		hdr_len += NET_UDPH_LEN;
	} else
#endif
#if defined(CONFIG_NET_TCP)
		if (sock->ip_proto == IPPROTO_TCP) {
		NET_PKT_DATA_ACCESS_DEFINE(tcp_access, struct net_tcp_hdr);
		struct net_tcp_hdr *tcp;

		tcp = (struct net_tcp_hdr *)net_pkt_get_data(pkt, &tcp_access);
		if (!tcp) {
			return -1;
		}

		(void)memset(tcp, 0, NET_TCPH_LEN);

		/* Setup TCP header */
		tcp->src_port = src_port;
		tcp->dst_port = dst_port;

		if (net_pkt_set_data(pkt, &tcp_access)) {
			return -1;
		}

		hdr_len += NET_TCPH_LEN;
	}
#endif /* CONFIG_NET_TCP */

	return hdr_len;
}

/*** MODEM RESPONSE HANDLERS ***/

/* Last Socket ID Handler */
static void on_cmd_atcmdecho(struct net_buf **buf, u16_t len)
{
	char value[3];
	value[0] = net_buf_pull_u8(*buf);
	/* If there is a double digit session ID */
	if (len == 2) {
		value[1] = net_buf_pull_u8(*buf);
	}
	ictx.last_socket_id = strtol(value, NULL, 0);
}

/* Echo Handler for commands without related sockets */
static void on_cmd_atcmdecho_nosock(struct net_buf **buf, u16_t len)
{
	/* clear last_socket_id */
	ictx.last_socket_id = 0;
}

static void on_cmd_atcmdinfo_manufacturer(struct net_buf **buf, u16_t len)
{
	struct net_buf *frag = NULL;
	u16_t offset;
	size_t out_len;
	int len_no_null = MDM_MANUFACTURER_LENGTH - 1;

	/* make sure revision data is received */
	if (len < MDM_MANUFACTURER_LENGTH) {
		LOG_DBG("Waiting for data");
		/* wait for more data */
		k_sleep(MDM_WAIT_FOR_DATA_TIME);
		hl7800_read_rx(buf);
	}

	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_WRN("Unable to find mfg (net_buf_skipcrlf)");
		return;
	}

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find mfg (net_buf_findcrlf)");
		return;
	}
	if (len < len_no_null) {
		LOG_WRN("mfg too short (len:%d)", len);
	} else if (len > len_no_null) {
		LOG_WRN("mfg too long (len:%d)", len);
		len = MDM_MANUFACTURER_LENGTH;
	}

	out_len = net_buf_linearize(ictx.mdm_manufacturer,
				    sizeof(ictx.mdm_manufacturer) - 1, *buf, 0,
				    len);
	ictx.mdm_manufacturer[out_len] = 0;
	LOG_INF("Manufacturer: %s", ictx.mdm_manufacturer);
}

static void on_cmd_atcmdinfo_model(struct net_buf **buf, u16_t len)
{
	struct net_buf *frag = NULL;
	u16_t offset;
	size_t out_len;
	int len_no_null = MDM_MODEL_LENGTH - 1;

	/* make sure revision data is received */
	if (len < MDM_MODEL_LENGTH) {
		LOG_DBG("Waiting for data");
		/* wait for more data */
		k_sleep(MDM_WAIT_FOR_DATA_TIME);
		hl7800_read_rx(buf);
	}

	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_WRN("Unable to find model (net_buf_skipcrlf)");
		return;
	}

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find model (net_buf_findcrlf)");
		return;
	}
	if (len < len_no_null) {
		LOG_WRN("model too short (len:%d)", len);
	} else if (len > len_no_null) {
		LOG_WRN("model too long (len:%d)", len);
		len = MDM_MODEL_LENGTH;
	}

	out_len = net_buf_linearize(ictx.mdm_model, sizeof(ictx.mdm_model) - 1,
				    *buf, 0, len);
	ictx.mdm_model[out_len] = 0;
	LOG_INF("Model: %s", ictx.mdm_model);
}

static void on_cmd_atcmdinfo_revision(struct net_buf **buf, u16_t len)
{
	struct net_buf *frag = NULL;
	u16_t offset;
	size_t out_len;
	int len_no_null = MDM_REVISION_LENGTH - 1;

	/* make sure revision data is received */
	if (len < MDM_REVISION_LENGTH) {
		LOG_DBG("Waiting for data");
		/* wait for more data */
		k_sleep(MDM_WAIT_FOR_DATA_TIME);
		hl7800_read_rx(buf);
	}

	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_WRN("Unable to find rev (net_buf_skipcrlf)");
		return;
	}

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find rev (net_buf_findcrlf)");
		return;
	}
	if (len < len_no_null) {
		LOG_WRN("revision too short (len:%d)", len);
	} else if (len > len_no_null) {
		LOG_WRN("revision too long (len:%d)", len);
		len = MDM_REVISION_LENGTH;
	}

	out_len = net_buf_linearize(
		ictx.mdm_revision, sizeof(ictx.mdm_revision) - 1, *buf, 0, len);
	ictx.mdm_revision[out_len] = 0;
	LOG_INF("Revision: %s", ictx.mdm_revision);
}

static void on_cmd_atcmdinfo_imei(struct net_buf **buf, u16_t len)
{
	struct net_buf *frag = NULL;
	u16_t offset;
	size_t out_len;
	int len_no_null = MDM_IMEI_LENGTH - 1;

	/* make sure IMEI data is received */
	if (len < MDM_IMEI_LENGTH) {
		LOG_DBG("Waiting for data");
		/* wait for more data */
		k_sleep(MDM_WAIT_FOR_DATA_TIME);
		hl7800_read_rx(buf);
	}

	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_WRN("Unable to find IMEI (net_buf_skipcrlf)");
		return;
	}

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find IMEI (net_buf_findcrlf)");
		return;
	}
	if (len < len_no_null) {
		LOG_WRN("IMEI too short (len:%d)", len);
	} else if (len > len_no_null) {
		LOG_WRN("IMEI too long (len:%d)", len);
		len = MDM_IMEI_LENGTH;
	}

	out_len = net_buf_linearize(ictx.mdm_imei, sizeof(ictx.mdm_imei) - 1,
				    *buf, 0, len);
	ictx.mdm_imei[out_len] = 0;

	LOG_INF("IMEI: %s", ictx.mdm_imei);
}

static void dns_work_cb(struct k_work *work)
{
#ifdef CONFIG_DNS_RESOLVER
	int ret;
	/* set new DNS addr in DNS resolver */
	struct dns_resolve_context *dnsCtx = dns_resolve_get_default();
	dns_resolve_close(dnsCtx);
	const char *dns_servers_str[] = { ictx.dns_string };
	ret = dns_resolve_init(dnsCtx, dns_servers_str, NULL);
	if (ret < 0) {
		LOG_ERR("dns_resolve_init fail (%d)", ret);
		return;
	}
#endif
}

/* Handler: +CGCONTRDP: <cid>,<bearer_id>,<apn>,<local_addr and subnet_mask>,
*			<gw_addr>,<DNS_prim_addr>,<DNS_sec_addr> */
static void on_cmd_atcmdinfo_ipaddr(struct net_buf **buf, u16_t len)
{
	int ret;
	int numDelims = 7;
	char *delims[numDelims];
	size_t out_len;
	char value[len + 1];
	char *searchStart, *addrStart, *smStart, *gwStart, *dnsStart;
	struct in_addr newIpv4Addr;

	out_len = net_buf_linearize(value, sizeof(value), *buf, 0, len);
	value[out_len] = 0;
	searchStart = value;

	/* find all delimiters (,) */
	for (int i = 0; i < numDelims; i++) {
		delims[i] = strchr(searchStart, ',');
		if (!delims[i]) {
			LOG_ERR("Could not find delim %d, val: %s", i,
				log_strdup(value));
			return;
		}
		/* Start next search after current delim location */
		searchStart = delims[i] + 1;
	}

	/* Find start of subnet mask */
	addrStart = delims[2] + 1;
	numDelims = 4;
	searchStart = addrStart;
	for (int i = 0; i < numDelims; i++) {
		smStart = strchr(searchStart, '.');
		if (!smStart) {
			LOG_ERR("Could not find submask start");
			return;
		}
		/* Start next search after current delim location */
		searchStart = smStart + 1;
	}

	/* get new IPv4 addr */
	int ipv4Len = smStart - addrStart;
	char ipv4AddrStr[ipv4Len + 1];
	strncpy(ipv4AddrStr, addrStart, ipv4Len);
	ipv4AddrStr[ipv4Len] = 0;
	ret = net_addr_pton(AF_INET, ipv4AddrStr, &newIpv4Addr);
	if (ret < 0) {
		LOG_ERR("Invalid IPv4 addr");
		return;
	}

	/* move past the '.' */
	smStart += 1;
	/* store new subnet mask */
	int snLen = delims[3] - smStart;
	char smStr[snLen + 1];
	strncpy(smStr, smStart, snLen);
	smStr[snLen] = 0;
	ret = net_addr_pton(AF_INET, smStr, &ictx.subnet);
	if (ret < 0) {
		LOG_ERR("Invalid subnet");
		return;
	}

	/* store new gateway */
	gwStart = delims[3] + 1;
	int gwLen = delims[4] - gwStart;
	char gwStr[gwLen + 1];
	strncpy(gwStr, gwStart, gwLen);
	gwStr[gwLen] = 0;
	ret = net_addr_pton(AF_INET, gwStr, &ictx.gateway);
	if (ret < 0) {
		LOG_ERR("Invalid gateway");
		return;
	}

	/* store new dns */
	dnsStart = delims[4] + 1;
	int dnsLen = delims[5] - dnsStart;
	strncpy(ictx.dns_string, dnsStart, dnsLen);
	ictx.dns_string[dnsLen] = 0;
	ret = net_addr_pton(AF_INET, ictx.dns_string, &ictx.dns);
	if (ret < 0) {
		LOG_ERR("Invalid dns");
		return;
	}

	if (ictx.iface) {
		/* remove the current IPv4 addr before adding a new one.
		*  We dont care if it is successful or not.
		*/
		net_if_ipv4_addr_rm(ictx.iface, &ictx.ipv4Addr);

		if (!net_if_ipv4_addr_add(ictx.iface, &newIpv4Addr,
					  NET_ADDR_DHCP, 0)) {
			LOG_ERR("Cannot set iface IPv4 addr");
			return;
		}

		net_if_ipv4_set_netmask(ictx.iface, &ictx.subnet);
		net_if_ipv4_set_gw(ictx.iface, &ictx.gateway);

		/* store the new IP addr */
		net_ipaddr_copy(&ictx.ipv4Addr, &newIpv4Addr);

		/* start DNS update work */
		s32_t delay = 0;
		if (!ictx.initialized) {
			/* Delay this in case the network
			*  stack is still starting up */
			delay = K_SECONDS(DNS_WORK_DELAY_SECS);
		}
		k_delayed_work_submit_to_queue(&hl7800_workq, &ictx.dns_work,
					       delay);
	} else {
		LOG_ERR("iface NULL");
	}

	/* TODO: IPv6 addr present, store it */
}

/* Handler: +COPS: <mode>[,<format>,<oper>[,<AcT>]] */
static void on_cmd_atcmdinfo_operator_status(struct net_buf **buf, u16_t len)
{
	size_t out_len;
	char value[len + 1];
	int numDelims = 2;
	char *delims[numDelims];
	char *searchStart;

	out_len = net_buf_linearize(value, sizeof(value), *buf, 0, len);
	value[out_len] = 0;
	LOG_DBG("Operator: %s", log_strdup(value));

	if (len == 1) {
		/* only mode was returned, there is no operator info */
		ictx.operatorStatus = NO_OPERATOR;
		return;
	}

	searchStart = value;

	/* find all delimiters (,) */
	for (int i = 0; i < numDelims; i++) {
		delims[i] = strchr(searchStart, ',');
		if (!delims[i]) {
			LOG_ERR("Could not find delim %d, val: %s", i,
				log_strdup(value));
			return;
		}
		/* Start next search after current delim location */
		searchStart = delims[i] + 1;
	}

	/* we found both delimiters, that means we have an operator */
	ictx.operatorStatus = REGISTERED;
}

/* Handler: +KGSN: T5640400011101 */
static void on_cmd_atcmdinfo_serial_number(struct net_buf **buf, u16_t len)
{
	struct net_buf *frag = NULL;
	u16_t offset;
	size_t out_len;
	int start, sn_len;
	int len_no_null = MDM_SN_LENGTH - 1;

	/* make sure SN# data is received */
	if (len < MDM_SN_LENGTH) {
		LOG_DBG("Waiting for data");
		/* wait for more data */
		k_sleep(MDM_WAIT_FOR_DATA_TIME);
		hl7800_read_rx(buf);
	}

	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_WRN("Unable to find sn (net_buf_skipcrlf)");
		return;
	}

	frag = NULL;
	start = net_buf_findchar(*buf, ':', &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find sn (net_buf_findchar)");
		return;
	}
	/* Remove ": " chars */
	start += 2;

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_WRN("Unable to find sn (net_buf_findcrlf)");
		return;
	}
	sn_len = len - start;
	if (sn_len < len_no_null) {
		LOG_WRN("sn too short (len:%d)", sn_len);
	} else if (sn_len > len_no_null) {
		LOG_WRN("sn too long (len:%d)", sn_len);
		len = MDM_SN_LENGTH;
	}

	out_len = net_buf_linearize(ictx.mdm_sn, sizeof(ictx.mdm_sn) - 1, *buf,
				    start, sn_len);
	ictx.mdm_sn[out_len] = 0;
	LOG_INF("Serial #: %s", ictx.mdm_sn);
}

static void on_cmd_startup_report(struct net_buf **buf, u16_t len)
{
	size_t out_len;
	char value[2];

	out_len = net_buf_linearize(value, sizeof(value) - 1, *buf, 0, len);
	value[out_len] = 0;
	ictx.mdm_startup_state = strtol(value, NULL, 0);
	LOG_INF("Startup: %d", ictx.mdm_startup_state);
}

static void hl7800_rssi_query_work(struct k_work *work)
{
	int ret;

	/* query modem RSSI */
	ret = send_at_cmd(NULL, "AT+CESQ", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CESQ ret:%d", ret);
	}

	/* re-start RSSI query work */
	k_delayed_work_submit_to_queue(&hl7800_workq, &ictx.rssi_query_work,
				       K_SECONDS(RSSI_TIMEOUT_SECS));
}

static void iface_status_work_cb(struct k_work *work)
{
	int ret;

	/* bring iface up/down */
	switch (ictx.networkState) {
	case HOME_NETWORK:
	case ROAMING:
		if (ictx.iface && !net_if_is_up(ictx.iface)) {
			/* bring the iface up */
			net_if_up(ictx.iface);
		}
		break;
	case OUT_OF_COVERAGE:
		ret = send_at_cmd(NULL, "AT+COPS?", MDM_CMD_SEND_TIMEOUT, 0);
		if (ret < 0) {
			LOG_ERR("AT+COPS ret:%d", ret);
			break;
		}

		if (ictx.iface && !net_if_is_up(ictx.iface) &&
		    ictx.operatorStatus == REGISTERED) {
			/* bring the iface up */
			net_if_up(ictx.iface);
		}
		break;
	default:
		if (ictx.iface && net_if_is_up(ictx.iface)) {
			/* we dont care if removing IP fails */
			net_if_ipv4_addr_rm(ictx.iface, &ictx.ipv4Addr);
			net_if_down(ictx.iface);
		}
#ifndef CONFIG_MODEM_HL7800_LOW_POWER_MODE
		/* stop periodic RSSI reading */
		k_delayed_work_cancel(&ictx.rssi_query_work);
#endif
		break;
	}

	/* get IP address info */
	if (ictx.iface && net_if_is_up(ictx.iface)) {
#ifndef CONFIG_MODEM_HL7800_LOW_POWER_MODE
		/* start periodic RSSI reading */
		hl7800_rssi_query_work(NULL);
#endif
		ret = send_at_cmd(NULL, "AT+CGCONTRDP=1", MDM_CMD_SEND_TIMEOUT,
				  0);
		if (ret < 0) {
			LOG_ERR("AT+CGCONTRDP ret:%d", ret);
		}
	}
}

/* Handler: +CEREG: <n>,<stat>[,[<lac>],[<ci>],[<AcT>]
*  [,[<cause_type>],[<reject_cause>] [,[<Active-Time>],[<Periodic-TAU>]]]] */
static void on_cmd_network_report(struct net_buf **buf, u16_t len)
{
	size_t out_len;

	out_len = net_buf_linearize(ictx.mdm_network_status,
				    sizeof(ictx.mdm_network_status) - 1, *buf,
				    0, len);
	ictx.mdm_network_status[out_len] = 0;
	char *pos = strchr(ictx.mdm_network_status, ',');
	if (pos) {
		int l = pos - ictx.mdm_network_status;
		char val[l];
		strncpy(val, ictx.mdm_network_status, l);
		val[l] = 0;
		ictx.networkState = strtol(val, NULL, 0);
	} else {
		ictx.networkState = strtol(ictx.mdm_network_status, NULL, 0);
	}

	LOG_INF("Network: %d", ictx.networkState);

	if (ictx.initialized && !ictx.restarting) {
		/* start work to adjust iface */
		k_work_submit_to_queue(&hl7800_workq, &ictx.iface_status_work);
	}
}

static int convert_rsrp_to_dbm(int rsrp)
{
	if (rsrp == 255) {
		return -999;
	} else {
		return rsrp - 141;
	}
}

/* Handler: +CESQ: 99,99,255,255,32,31 */
static void on_cmd_atcmdinfo_rssi(struct net_buf **buf, u16_t len)
{
	int start = 0, i = 0;
	size_t value_size;
	char value[32];

	value_size = sizeof(value);
	(void)memset(value, 0, value_size);
	while (*buf && len > 0 && i < value_size) {
		value[i] = net_buf_pull_u8(*buf);
		if (!(*buf)->len) {
			*buf = net_buf_frag_del(NULL, *buf);
		}

		/* 5th "," marks the beginning of the rsrp value */
		if (start < 5) {
			if (value[i] == ',') {
				start++;
			}

			continue;
		}

		/* '\r' marks the end of the RSSI value */
		if (value[i] == '\r') {
			value[i] = '\0';
			break;
		}

		i++;
	}

	if (i > 0) {
		ictx.mdm_ctx.data_rssi =
			convert_rsrp_to_dbm(strtol(value, NULL, 0));
		LOG_INF("RSSI: %d", ictx.mdm_ctx.data_rssi);
	} else {
		LOG_WRN("Bad format found for RSSI");
	}
}

/* Handler: OK */
static void on_cmd_sockok(struct net_buf **buf, u16_t len)
{
	struct hl7800_socket *sock = NULL;

	ictx.last_error = 0;
	sock = socket_from_id(ictx.last_socket_id);
	if (!sock) {
		k_sem_give(&ictx.response_sem);
	} else {
		k_sem_give(&sock->sock_send_sem);
	}
}

/* Handler: +KTCP_IND/+KUDP_IND */
static void on_cmd_sock_ind(struct net_buf **buf, u16_t len)
{
	struct hl7800_socket *sock = NULL;
	struct net_buf *frag = NULL;
	u16_t offset;
	int pos;
	char *id;

	ictx.last_error = 0;

	pos = net_buf_findchar(*buf, ',', &frag, &offset);
	if (frag) {
		char val[pos + 1];
		net_buf_linearize(val, sizeof(val), *buf, 0, pos);
		id = val;
	} else {
		LOG_ERR("+K**P_IND could not find ','");
		return;
	}

	ictx.last_socket_id = strtol(id, NULL, 0);
	sock = socket_from_id(ictx.last_socket_id);
	if (sock) {
		k_sem_give(&sock->sock_send_sem);
	} else {
		LOG_ERR("Could not find socket id (%d)", ictx.last_socket_id);
	}
}

/* Handler: ERROR */
static void on_cmd_sockerror(struct net_buf **buf, u16_t len)
{
	struct hl7800_socket *sock = NULL;

	ictx.last_error = -EIO;
	sock = socket_from_id(ictx.last_socket_id);
	if (!sock) {
		k_sem_give(&ictx.response_sem);
	} else {
		LOG_ERR("Socket error!");
		k_sem_give(&sock->sock_send_sem);
	}
}

/* Handler: +KTCP_NOTIF/+KUDP_NOTIF */
static void on_cmd_sock_notif(struct net_buf **buf, u16_t len)
{
	struct hl7800_socket *sock = NULL;
	struct net_buf *frag = NULL;
	u16_t offset;
	int pos;
	char *id;

	ictx.last_error = -EIO;

	pos = net_buf_findchar(*buf, ',', &frag, &offset);
	if (frag) {
		char val[pos + 1];
		net_buf_linearize(val, sizeof(val), *buf, 0, pos);
		id = val;
	} else {
		LOG_ERR("+K**P_NOTIF could not find ','");
		return;
	}

	ictx.last_socket_id = strtol(id, NULL, 0);
	sock = socket_from_id(ictx.last_socket_id);
	if (sock) {
		/* Send NULL packet to callback to notify upper stack layers
		*  that the peer closed the connection or there was an error.
		*  This is so an app will not get stuck in recv() forever.
		*  Let's do the callback processing in a different work queue in
		*  case the app takes a long time. sock->recv_pkt should
		*  already be NULL at this point.
		*/
		k_work_submit_to_queue(&hl7800_workq, &sock->recv_cb_work);
		k_sem_give(&sock->sock_send_sem);
	} else {
		LOG_ERR("Could not find socket id (%d)", ictx.last_socket_id);
	}
}

/* Handler: +KTCPCFG/+KUDPCFG: <session_id> */
static void on_cmd_sockcreate(struct net_buf **buf, u16_t len)
{
	char value[3];
	struct hl7800_socket *sock = NULL;

	value[0] = net_buf_pull_u8(*buf);
	/* If there is a double digit session ID */
	if (len == 2) {
		value[1] = net_buf_pull_u8(*buf);
	}
	ictx.last_socket_id = strtol(value, NULL, 0);

	/* check if the socket has been created already */

	sock = socket_from_id(ictx.last_socket_id);
	if (!sock) {
		/* look up new socket by special id */
		sock = socket_from_id(MDM_MAX_SOCKETS + 1);
		if (!sock) {
			LOG_ERR("No matching socket");
			return;
		}
	}

	sock->socket_id = ictx.last_socket_id;
	sock->created = true;
	/* don't give back semaphore -- OK to follow */
}

static void sockreadrecv_cb_work(struct k_work *work)
{
	struct hl7800_socket *sock = NULL;
	struct net_pkt *pkt;

	sock = CONTAINER_OF(work, struct hl7800_socket, recv_cb_work);

	if (!sock) {
		LOG_ERR("Sock not found");
		return;
	}

	/* return data */
	pkt = sock->recv_pkt;
	sock->recv_pkt = NULL;
	if (sock->recv_cb) {
		sock->recv_cb(sock->context, pkt, NULL, NULL, 0,
			      sock->recv_user_data);
	} else {
		net_pkt_unref(pkt);
	}
}

/* Handler: +KTCPRCV/+KUDPRCV: <socket_id>,<num_bytes> */
static void on_cmd_sockread(struct net_buf **buf, u16_t len)
{
	struct hl7800_socket *sock = NULL;
	struct net_buf *frag;
	u8_t c = 0U;
	char *delim;
	int i, num_bytes, socket_id, hdr_len, connectWait = 0;
	char value[sizeof("##,####")];
	size_t value_size, out_len;
	u16_t offset;

	value_size = sizeof(value);
	out_len = net_buf_linearize(value, sizeof(value) - 1, *buf, 0, len);
	value[out_len] = 0;
	/* remove the message from the buffer */
	net_buf_pull(*buf, len);

	/* First comma separator marks the end of socket_id */
	delim = strchr(value, ',');
	if (!delim) {
		LOG_ERR("Missing comma");
		return;
	}

	*delim++ = '\0';
	socket_id = strtol(value, NULL, 0);
	ictx.last_socket_id = socket_id;

	/* second param is for num_bytes */
	num_bytes = strtol(delim, NULL, 0);

	sock = socket_from_id(ictx.last_socket_id);
	if (!sock) {
		LOG_ERR("Socket not found! (%d)", ictx.last_socket_id);
		return;
	}

	/* receive 'CONNECT' */
	/* remove ending \r\n from last message */
	net_buf_skipcrlf(buf);
	if (!*buf) {
		/* No data left in buffer, get more data */
		do {
			k_sleep(MDM_WAIT_FOR_DATA_TIME);
			hl7800_read_rx(buf);
			LOG_DBG("Waiting for CONNECT");
			connectWait++;
		} while ((!*buf || (*buf)->len < strlen(CONNECT_STRING)) &&
			 connectWait < 5);
	}
	/* got CONNECT, remove leading \r\n */
	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_ERR("No CONNECT start");
		return;
	}

	frag = NULL;
	len = net_buf_findcrlf(*buf, &frag, &offset);
	if (!frag) {
		LOG_ERR("Unable to find CONNECT end");
		return;
	}
	char con[sizeof(CONNECT_STRING)];
	out_len = net_buf_linearize(con, sizeof(con), *buf, 0, len);
	con[out_len] = 0;
	/* remove the message from the buffer */
	net_buf_pull(*buf, len);
	if (strcmp(con, CONNECT_STRING)) {
		LOG_ERR("Could not find CONNECT");
		return;
	}

	/* remove the \r\n after 'CONNECT' */
	net_buf_skipcrlf(buf);
	if (!*buf) {
		LOG_ERR("No CONNECT end");
		return;
	}

	/* allocate an RX pkt */
	sock->recv_pkt = net_pkt_rx_alloc_with_buffer(
		net_context_get_iface(sock->context), num_bytes, sock->family,
		sock->ip_proto, BUF_ALLOC_TIMEOUT);
	if (!sock->recv_pkt) {
		LOG_ERR("Failed net_pkt_get_reserve_rx!");
		return;
	}

	/* set pkt data */
	net_pkt_set_context(sock->recv_pkt, sock->context);

	/* add IP / protocol headers */
	hdr_len = pkt_setup_ip_data(sock->recv_pkt, sock);

	/* receive data */
	for (i = 0; i < num_bytes; i++) {
		c = *(*buf)->data;
		if (net_pkt_write_u8(sock->recv_pkt, c)) {
			LOG_ERR("Unable to add data! Aborting! Bytes RXd:%d",
				i);
			net_pkt_unref(sock->recv_pkt);
			sock->recv_pkt = NULL;
			return;
		}

		/* pull data from buf and advance to the next frag if needed */
		net_buf_pull_u8(*buf);
		if (!(*buf)->len) {
			*buf = net_buf_frag_del(NULL, *buf);
		}

		if (!*buf && i < num_bytes) {
			LOG_DBG("RX more data, bytes RXd:%d", i);
			/* wait for more data */
			k_sleep(MDM_WAIT_FOR_DATA_TIME);
			hl7800_read_rx(buf);
			if (!*buf) {
				LOG_ERR("No data in buf!");
				break;
			}
		}
	}

	net_pkt_cursor_init(sock->recv_pkt);
	net_pkt_set_overwrite(sock->recv_pkt, true);

	if (hdr_len > 0) {
		net_pkt_skip(sock->recv_pkt, hdr_len);
	}

	/* Let's do the callback processing in a different work queue in
	* case the app takes a long time.
	*/
	k_work_submit_to_queue(&hl7800_workq, &sock->recv_cb_work);
}

/* Handler: +KTCP_DATA/+KUDP_DATA: <socket_id>,<left_bytes> */
static void on_cmd_sockdataind(struct net_buf **buf, u16_t len)
{
	int socket_id, left_bytes;
	size_t out_len;
	char *delim;
	char value[sizeof("##,####")];
	char sendbuf[sizeof("AT+KTCPRCV=##,####")];
	struct hl7800_socket *sock = NULL;

	out_len = net_buf_linearize(value, sizeof(value) - 1, *buf, 0, len);
	value[out_len] = 0;

	/* First comma separator marks the end of socket_id */
	delim = strchr(value, ',');
	if (!delim) {
		LOG_ERR("Missing comma");
		return;
	}

	*delim++ = '\0';
	socket_id = strtol(value, NULL, 0);
	ictx.last_socket_id = socket_id;

	/* second param is for left_bytes */
	left_bytes = strtol(delim, NULL, 0);

	sock = socket_from_id(socket_id);
	if (!sock) {
		LOG_ERR("Unable to find socket_id:%d", socket_id);
		return;
	}

	if (left_bytes > 0) {
		LOG_DBG("socket_id:%d left_bytes:%d", socket_id, left_bytes);
		if (sock->type == SOCK_DGRAM) {
			snprintk(sendbuf, sizeof(sendbuf), "AT+KUDPRCV=%d,%u",
				 sock->socket_id, left_bytes);
		} else {
			snprintk(sendbuf, sizeof(sendbuf), "AT+KTCPRCV=%d,%u",
				 sock->socket_id, left_bytes);
		}

		/* We entered this trigger due to an unsolicited modem response.
		* When we send the AT+KTCPRCV command it won't generate an
		* "OK" response directly.  The modem will respond with
		* "CONNECT" and the data requested and then "OK" or
		* "ERROR".  Let's not wait here by passing in a timeout to
		* send_at_cmd().  Instead, when the resulting response is
		* received, we trigger on_cmd_sockread() to handle it.
		*/
		send_at_cmd(sock, sendbuf, K_NO_WAIT, 0);
	}
}

static int net_buf_ncmp(struct net_buf *buf, const u8_t *s2, size_t n)
{
	/* TODO: remove the commented items in this function.  Leave them for now for DEBUG purposes */
	struct net_buf *frag = buf;
	u16_t offset = 0U;
	// bool flagMe = false;
	int ret = 0;

	// if (!strncmp(s2, "+KTCP_DATA: ", 12) &&
	//     !strncmp(frag->data, "+KTCP_D", 7)) {
	// 	flagMe = true;
	// }

	while (true) {
		if ((n > 0) && (*(frag->data + offset) == *s2) &&
		    (*s2 != '\0')) {
			if (offset == frag->len) {
				// if (flagMe) {
				// 	flagMe = true;
				// }
				if (!frag->frags) {
					break;
				}
				frag = frag->frags;
				offset = 0U;
			} else {
				offset++;
			}

			s2++;
			n--;
		} /* else if (flagMe && n != 0) {
			flagMe = true;
			break;
		}*/
		else {
			break;
		}
	}
	ret = (n == 0) ? 0 : (*(frag->data + offset) - *s2);
	// if (flagMe && ret != 0) {
	// 	flagMe = false;
	// }
	return ret;
}

static inline struct net_buf *read_rx_allocator(s32_t timeout, void *user_data)
{
	return net_buf_alloc((struct net_buf_pool *)user_data, timeout);
}

static void hl7800_read_rx(struct net_buf **buf)
{
	u8_t uart_buffer[MDM_RECV_BUF_SIZE];
	size_t bytes_read = 0;
	int ret;
	u16_t rx_len;

	/* read all of the data from mdm_receiver */
	while (true) {
		ret = mdm_receiver_recv(&ictx.mdm_ctx, uart_buffer,
					sizeof(uart_buffer), &bytes_read);
		if (ret < 0 || bytes_read == 0) {
			/* mdm_receiver buffer is empty */
			break;
		}

		_hexdump(uart_buffer, bytes_read);

		/* make sure we have storage */
		if (!*buf) {
			*buf = net_buf_alloc(&mdm_recv_pool, BUF_ALLOC_TIMEOUT);
			if (!*buf) {
				LOG_ERR("Can't allocate RX data! "
					"Skipping data!");
				break;
			}
		}

		rx_len =
			net_buf_append_bytes(*buf, bytes_read, uart_buffer,
					     BUF_ALLOC_TIMEOUT,
					     read_rx_allocator, &mdm_recv_pool);
		if (rx_len < bytes_read) {
			LOG_ERR("Data was lost! read %u of %u!", rx_len,
				bytes_read);
		}
	}
}

/* RX thread */
static void hl7800_rx(void)
{
	struct net_buf *rx_buf = NULL;
	struct net_buf *frag = NULL;
	int i;
	u16_t offset, len;
	size_t out_len;
	bool cmd_handled = false;

	static const struct cmd_handler handlers[] = {
		/* NON-SOCKET COMMAND ECHOES to clear last_socket_id */
		CMD_HANDLER("ATE1", atcmdecho_nosock),
		CMD_HANDLER("AT+CESQ", atcmdecho_nosock),
		CMD_HANDLER("AT+CGCONTRDP=", atcmdecho_nosock),
		CMD_HANDLER("AT+KSREP=", atcmdecho_nosock),
		CMD_HANDLER("AT+CEREG=", atcmdecho_nosock),
		CMD_HANDLER("AT+CEREG?", atcmdecho_nosock),
		CMD_HANDLER("AT+CGDCONT=", atcmdecho_nosock),
		CMD_HANDLER("AT+KCNXCFG=", atcmdecho_nosock),
		CMD_HANDLER("AT+KSLEEP=", atcmdecho_nosock),
		CMD_HANDLER("AT+CPSMS=", atcmdecho_nosock),
		CMD_HANDLER("AT+CEDRXS=", atcmdecho_nosock),
		CMD_HANDLER("AT+COPS?", atcmdecho_nosock),

		/* SOCKET COMMAND ECHOES for last_socket_id processing */
		CMD_HANDLER("AT+KTCPCFG=", atcmdecho),
		CMD_HANDLER("AT+KUDPCFG=", atcmdecho),
		CMD_HANDLER("AT+KTCPCNX=", atcmdecho),
		CMD_HANDLER("AT+KTCPSND=", atcmdecho),
		CMD_HANDLER("AT+KUDPSND=", atcmdecho),
		CMD_HANDLER("AT+KTCPCLOSE=", atcmdecho),
		CMD_HANDLER("AT+KUDPCLOSE=", atcmdecho),
		CMD_HANDLER("AT+KTCPDEL=", atcmdecho),

		/* MODEM Information */
		CMD_HANDLER("AT+CGMI", atcmdinfo_manufacturer),
		CMD_HANDLER("ATI0", atcmdinfo_model),
		CMD_HANDLER("ATI3", atcmdinfo_revision),
		CMD_HANDLER("+CESQ: ", atcmdinfo_rssi),
		CMD_HANDLER("AT+KGSN=3", atcmdinfo_serial_number),
		CMD_HANDLER("+CGCONTRDP: ", atcmdinfo_ipaddr),
		CMD_HANDLER("AT+CGSN", atcmdinfo_imei),
		CMD_HANDLER("+COPS: ", atcmdinfo_operator_status),

		/* UNSOLICITED modem information */
		/* mobile startup report */
		CMD_HANDLER("+KSUP: ", startup_report),
		/* network status */
		CMD_HANDLER("+CEREG: ", network_report),

		/* SOLICITED SOCKET RESPONSES */
		CMD_HANDLER("OK", sockok),
		CMD_HANDLER("ERROR", sockerror),
		CMD_HANDLER("+CME ERROR: ", sockerror),
		CMD_HANDLER("+CMS ERROR: ", sockerror),
		CMD_HANDLER("+CEER: ", sockerror),
		CMD_HANDLER("+KTCPCFG: ", sockcreate),
		CMD_HANDLER("+KUDPCFG: ", sockcreate),
		CMD_HANDLER(CONNECT_STRING, sockok),
		CMD_HANDLER("AT+KTCPRCV=", sockread),
		CMD_HANDLER("AT+KUDPRCV=", sockread),

		/* UNSOLICITED SOCKET RESPONSES */
		CMD_HANDLER("+KTCP_IND: ", sock_ind),
		CMD_HANDLER("+KUDP_IND: ", sock_ind),
		CMD_HANDLER("+KTCP_NOTIF: ", sock_notif),
		CMD_HANDLER("+KUDP_NOTIF: ", sock_notif),
		CMD_HANDLER("+KTCP_DATA: ", sockdataind),
		CMD_HANDLER("+KUDP_DATA: ", sockdataind),
	};

	while (true) {
		/* wait for incoming data */
		k_sem_take(&ictx.mdm_ctx.rx_sem, K_FOREVER);

		hl7800_read_rx(&rx_buf);

		while (rx_buf) {
			cmd_handled = false;
			net_buf_skipcrlf(&rx_buf);
			if (!rx_buf) {
				break;
			}

			frag = NULL;
			len = net_buf_findcrlf(rx_buf, &frag, &offset);
			if (!frag) {
				break;
			}

			/* look for matching data handlers */
			i = -1;
			for (i = 0; i < ARRAY_SIZE(handlers); i++) {
				if (net_buf_ncmp(rx_buf, handlers[i].cmd,
						 handlers[i].cmd_len) == 0) {
					/* found a matching handler */
					/* LOG_DBG("MATCH %s (len:%u)",
					 	handlers[i].cmd, len);*/

					/* skip cmd_len */
					rx_buf = net_buf_skip(
						rx_buf, handlers[i].cmd_len);

					/* locate next cr/lf */
					frag = NULL;
					len = net_buf_findcrlf(rx_buf, &frag,
							       &offset);
					if (!frag) {
						break;
					}

					LOG_DBG("HANDLE %s (len:%u)",
						handlers[i].cmd, len);
					/* call handler */
					if (handlers[i].func) {
						handlers[i].func(&rx_buf, len);
					}
					cmd_handled = true;

					frag = NULL;
					/* make sure buf still has data */
					if (!rx_buf) {
						break;
					}

					/*
					* We've handled the current line
					* and need to exit the "search for
					* handler loop".  Let's skip any
					* "extra" data and look for the next
					* CR/LF, leaving us ready for the
					* next handler search.  Ignore the
					* length returned.
					*/
					(void)net_buf_findcrlf(rx_buf, &frag,
							       &offset);
					break;
				}
			}

#ifdef LOG_UNHANDLED_RX_MSGS
			/* Handle unhandled commands */
			if (!cmd_handled && frag && len > 1) {
				char msg[len + 1];
				out_len = net_buf_linearize(msg, sizeof(msg),
							    rx_buf, 0, len);
				msg[out_len] = 0;
				/* NOTE: may need to increase log_strdup buffers
				*  if many unhandled commands happen in quick
				*  succession */
				LOG_WRN("UNHANDLED RX: %s", log_strdup(msg));
			}
#endif
			if (frag && rx_buf) {
				/* clear out processed line (buffers) */
				while (frag && rx_buf != frag) {
					rx_buf = net_buf_frag_del(NULL, rx_buf);
				}

				net_buf_pull(rx_buf, offset);
			}
		}

		/* give up time if we have a solid stream of data */
		k_yield();
	}
}

void mdm_tx_on_callback(struct device *port, struct gpio_callback *cb,
			u32_t pins)
{
	u32_t val;
	gpio_pin_read(ictx.gpio_port_dev[MDM_LTE_TX_ON],
		      pinconfig[MDM_LTE_TX_ON].pin, &val);
	LOG_DBG("MDM_TX_ON:%d", val);
}

void mdm_vgpio_callback(struct device *port, struct gpio_callback *cb,
			u32_t pins)
{
	u32_t val;
	gpio_pin_read(ictx.gpio_port_dev[MDM_VGPIO], pinconfig[MDM_VGPIO].pin,
		      &val);
	ictx.vgpio_state = val;
	if (!val) {
		ictx.sleepState = ASLEEP;
	} else {
		ictx.sleepState = WAKING;
	}

	LOG_DBG("VGPIO:%d", val);
}

void mdm_uart_dsr_callback(struct device *port, struct gpio_callback *cb,
			   u32_t pins)
{
	u32_t val;
	gpio_pin_read(ictx.gpio_port_dev[MDM_UART_DSR],
		      pinconfig[MDM_UART_DSR].pin, &val);
	ictx.dsr_state = val;
	if (ictx.sleepState == WAKING && !val) {
		ictx.sleepState = AWAKE;
		k_sem_give(&ictx.mdm_awake);
	}

	LOG_DBG("MDM_UART_DSR:%d", val);
}

static void prepare_io_for_reset(void)
{
	modem_assert_wake(false);
	modem_assert_pwr_on(false);
	modem_assert_fast_shutd(false);
	modem_assert_uart_dtr(false);
}

static int modem_reset(bool keepAwake)
{
	LOG_INF("Resetting Modem");

	/* make sure all IO voltage removed for proper reset */
	prepare_io_for_reset();

	/* Hard reset the modem (>20 milli seconds required) */
	LOG_DBG("MDM_RESET_PIN -> ASSERTED");
	gpio_pin_write(ictx.gpio_port_dev[MDM_RESET], pinconfig[MDM_RESET].pin,
		       MDM_RESET_ASSERTED);
	k_sleep(MDM_RESET_LOW_TIME);
	LOG_DBG("MDM_RESET_PIN -> NOT_ASSERTED");
	gpio_pin_write(ictx.gpio_port_dev[MDM_RESET], pinconfig[MDM_RESET].pin,
		       MDM_RESET_NOT_ASSERTED);

	/* Flag the modem as asleep until it is fully booted */
	ictx.sleepState = ASLEEP;
	if (keepAwake) {
		allow_sleep(false);
	}

	return 0;
}

static int hl7800_modem_reset(void)
{
	int ret = 0;

	ictx.restarting = true;
	/* bring down network interface */
	if (ictx.iface) {
		net_if_down(ictx.iface);
	}

	/* stop RSSI delay work */
	k_delayed_work_cancel(&ictx.rssi_query_work);

	modem_reset(true);

	k_sem_reset(&ictx.mdm_awake);
	LOG_DBG("Waiting for modem to boot...");
	ret = k_sem_take(&ictx.mdm_awake, MDM_BOOT_TIME);
	if (ret) {
		LOG_ERR("Err waiting for boot: %d, DSR: %u", ret,
			ictx.dsr_state);
	}

	LOG_DBG("Modem booted!");

	/* SETUP THE MODEM */
	LOG_INF("Setting up modem");

	/* turn on echo, echo is required for proper cmd processing */
	ret = send_at_cmd(NULL, "ATE1", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("ATE1 ret:%d", ret);
		goto error;
	}

	/* Turn OFF EPS network registration status reporting, for now */
	ret = send_at_cmd(NULL, "AT+CEREG=0", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CEREG ret:%d", ret);
		goto error;
	}

	/* Setup APN */
	ret = send_at_cmd(
		NULL,
		"AT+CGDCONT=1,\"IPV4V6\",\"" CONFIG_MODEM_HL7800_APN_NAME "\"",
		MDM_CMD_SEND_TIMEOUT, MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CGDCONT ret:%d", ret);
		goto error;
	}

	/* Setup GPRS connection */
	ret = send_at_cmd(
		NULL,
		"AT+KCNXCFG=1,\"GPRS\",\"" CONFIG_MODEM_HL7800_APN_NAME "\"",
		MDM_CMD_SEND_TIMEOUT, MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+KCNXCFG ret:%d", ret);
		goto error;
	}

#ifdef CONFIG_MODEM_HL7800_LOW_POWER_MODE
	/* Turn on sleep mode */
	ret = send_at_cmd(NULL, "AT+KSLEEP=0,2", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+KSLEEP ret:%d", ret);
		goto error;
	}

	/* Turn on PSM */
	ret = send_at_cmd(NULL, "AT+CPSMS=1,,,\"01111000\",\"00001010\"",
			  MDM_CMD_SEND_TIMEOUT, MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CPSMS ret:%d", ret);
		goto error;
	}

	allow_sleep(true);
#else
	/* Turn off sleep mode */
	ret = send_at_cmd(NULL, "AT+KSLEEP=2", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+KSLEEP ret:%d", ret);
		goto error;
	}

	/* Turn off PSM */
	ret = send_at_cmd(NULL, "AT+CPSMS=0", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CPSMS ret:%d", ret);
		goto error;
	}

	/* Turn off eDRX */
	ret = send_at_cmd(NULL, "AT+CEDRXS=0", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CEDRXS ret:%d", ret);
		goto error;
	}
#endif

	/* query modem info */
	LOG_INF("Querying modem information");

	/* modem manufacturer */
	ret = send_at_cmd(NULL, "AT+CGMI", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CGMI ret:%d", ret);
		goto error;
	}

	/* modem model */
	ret = send_at_cmd(NULL, "ATI0", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("ATI0 ret:%d", ret);
		goto error;
	}

	/* modem revision */
	ret = send_at_cmd(NULL, "ATI3", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("ATI3 ret:%d", ret);
		goto error;
	}

	/* query modem IMEI */
	ret = send_at_cmd(NULL, "AT+CGSN", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CGSN ret:%d", ret);
		goto error;
	}

	/* query modem serial number */
	ret = send_at_cmd(NULL, "AT+KGSN=3", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+KGSN ret:%d", ret);
		goto error;
	}

	/* Turn on mobile start-up reporting */
	ret = send_at_cmd(NULL, "AT+KSREP=1", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+KSREP ret:%d", ret);
		goto error;
	}

	/* Turn on EPS network registration status reporting */
	ret = send_at_cmd(NULL, "AT+CEREG=4", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CEREG ret:%d", ret);
		goto error;
	}

	/* query the network status in case we already registered */
	ret = send_at_cmd(NULL, "AT+CEREG?", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret < 0) {
		LOG_ERR("AT+CEREG? ret:%d", ret);
		goto error;
	}

	LOG_INF("Modem ready!");
error:
	ictx.restarting = false;
	return ret;
}

s32_t mdm_hl7800_reset(void)
{
	int ret;
	ret = hl7800_modem_reset();
	if (ret == 0) {
		/* update the iface status after reset */
		iface_status_work_cb(NULL);
	}

	return ret;
}

static int hl7800_power_off(void)
{
	int ret = 0;
	LOG_INF("Powering off modem");
#ifdef CONFIG_MODEM_HL7800_LOW_POWER_MODE
	/* Wake up the module */
	wakeup_hl7800();
#else
	/* stop RSSI delay work */
	k_delayed_work_cancel(&ictx.rssi_query_work);

#endif
	ret = send_at_cmd(NULL, "AT+CPOF", MDM_CMD_SEND_TIMEOUT,
			  MDM_DEFAULT_AT_CMD_RETRIES);
	if (ret) {
		LOG_ERR("AT+CPOF ret:%d", ret);
		return ret;
	}

	allow_sleep(true);
	LOG_INF("Modem powered off");
	return ret;
}

s32_t mdm_hl7800_power_off(void)
{
	return hl7800_power_off();
}

/*** OFFLOAD FUNCTIONS ***/

static int offload_get(sa_family_t family, enum net_sock_type type,
		       enum net_ip_protocol ip_proto,
		       struct net_context **context)
{
	int ret = 0;
	struct hl7800_socket *sock = NULL;

	/* new socket */
	sock = socket_get();
	if (!sock) {
		return -ENOMEM;
	}

	(*context)->offload_context = sock;
	/* set the context iface index to our iface */
	(*context)->iface = net_if_get_by_iface(ictx.iface);
	sock->family = family;
	sock->type = type;
	sock->ip_proto = ip_proto;
	sock->context = *context;
	sock->socket_id = MDM_MAX_SOCKETS + 1; /* socket # needs assigning */

	/* If UDP, create UDP socket now.
	*  TCP socket needs to be created later once the connection IP address is known.
	*/
	if (type == SOCK_DGRAM) {
		ret = send_at_cmd(sock, "AT+KUDPCFG=1,0", MDM_CMD_SEND_TIMEOUT,
				  0);
		if (ret < 0) {
			LOG_ERR("AT+KUDPCFG ret:%d", ret);
			socket_put(sock);
		}

		/* Now wait for +KUDP_IND or +KUDP_NOTIF to ensure
		*  the socket was created */
		ret = k_sem_take(&sock->sock_send_sem, MDM_CMD_CONN_TIMEOUT);
		if (ret == 0) {
			ret = ictx.last_error;
		} else if (ret == -EAGAIN) {
			ret = -ETIMEDOUT;
		}
		if (ret < 0) {
			LOG_ERR("+KUDP_IND/NOTIF ret:%d", ret);
			socket_put(sock);
		}
	}

	return ret;
}

static int offload_bind(struct net_context *context,
			const struct sockaddr *addr, socklen_t addrlen)
{
	struct hl7800_socket *sock = NULL;

	if (!context) {
		return -EINVAL;
	}

	sock = (struct hl7800_socket *)context->offload_context;
	if (!sock) {
		LOG_ERR("Can't locate socket for net_ctx:%p!", context);
		return -EINVAL;
	}

	/* save bind address information */
	sock->src.sa_family = addr->sa_family;
#if defined(CONFIG_NET_IPV6)
	if (addr->sa_family == AF_INET6) {
		net_ipaddr_copy(&net_sin6(&sock->src)->sin6_addr,
				&net_sin6(addr)->sin6_addr);
		net_sin6(&sock->src)->sin6_port = net_sin6(addr)->sin6_port;
	} else
#endif
#if defined(CONFIG_NET_IPV4)
		if (addr->sa_family == AF_INET) {
		net_ipaddr_copy(&net_sin(&sock->src)->sin_addr,
				&net_sin(addr)->sin_addr);
		net_sin(&sock->src)->sin_port = net_sin(addr)->sin_port;
	} else
#endif
	{
		return -EPFNOSUPPORT;
	}

	return 0;
}

static int offload_listen(struct net_context *context, int backlog)
{
	/* NOT IMPLEMENTED */
	return -ENOTSUP;
}

static int offload_connect(struct net_context *context,
			   const struct sockaddr *addr, socklen_t addrlen,
			   net_context_connect_cb_t cb, s32_t timeout,
			   void *user_data)
{
	int ret = 0;
	int dst_port = -1;
	char cmd_cfg[sizeof("AT+KTCPCFG=#,#,\"###.###.###.###\",#####")];
	char cmd_con[sizeof("AT+KTCPCNX=##")];
	struct hl7800_socket *sock;

	if (!context || !addr) {
		return -EINVAL;
	}

	sock = (struct hl7800_socket *)context->offload_context;
	if (!sock) {
		LOG_ERR("Can't locate socket for net_ctx:%p!", context);
		return -EINVAL;
	}

	if (sock->socket_id < 1) {
		LOG_ERR("Invalid socket_id(%d) for net_ctx:%p!",
			sock->socket_id, context);
		return -EINVAL;
	}

	sock->dst.sa_family = addr->sa_family;

#if defined(CONFIG_NET_IPV6)
	if (addr->sa_family == AF_INET6) {
		net_ipaddr_copy(&net_sin6(&sock->dst)->sin6_addr,
				&net_sin6(addr)->sin6_addr);
		dst_port = ntohs(net_sin6(addr)->sin6_port);
		net_sin6(&sock->dst)->sin6_port = dst_port;
	} else
#endif
#if defined(CONFIG_NET_IPV4)
		if (addr->sa_family == AF_INET) {
		net_ipaddr_copy(&net_sin(&sock->dst)->sin_addr,
				&net_sin(addr)->sin_addr);
		dst_port = ntohs(net_sin(addr)->sin_port);
		net_sin(&sock->dst)->sin_port = dst_port;
	} else
#endif
	{
		return -EINVAL;
	}

	if (dst_port < 0) {
		LOG_ERR("Invalid port: %d", dst_port);
		return -EINVAL;
	}

	if (sock->type == SOCK_STREAM) {
		/* Configure/create TCP connection */
		if (!sock->created) {
			snprintk(cmd_cfg, sizeof(cmd_cfg),
				 "AT+KTCPCFG=%d,%d,\"%s\",%u", 1, 0,
				 hl7800_sprint_ip_addr(addr), dst_port);
			ret = send_at_cmd(sock, cmd_cfg, MDM_CMD_SEND_TIMEOUT,
					  0);
			if (ret < 0) {
				LOG_ERR("AT+KTCPCFG ret:%d", ret);
				return -EIO;
			}
		}

		/* Connect to TCP */
		snprintk(cmd_con, sizeof(cmd_con), "AT+KTCPCNX=%d",
			 sock->socket_id);
		ret = send_at_cmd(sock, cmd_con, MDM_CMD_SEND_TIMEOUT, 0);
		if (ret < 0) {
			LOG_ERR("AT+KTCPCNX ret:%d", ret);
			return -EIO;
		}
		/* Now wait for +KTCP_IND or +KTCP_NOTIF to ensure
		*  the connection succeded or failed */
		ret = k_sem_take(&sock->sock_send_sem, MDM_CMD_CONN_TIMEOUT);
		if (ret == 0) {
			ret = ictx.last_error;
		} else if (ret == -EAGAIN) {
			ret = -ETIMEDOUT;
		}
		if (ret < 0) {
			LOG_ERR("+KTCP_IND/NOTIF ret:%d", ret);
		} else {
			net_context_set_state(context, NET_CONTEXT_CONNECTED);
		}
	}

	if (cb) {
		cb(context, ret, user_data);
	}

	return ret;
}

static int offload_accept(struct net_context *context, net_tcp_accept_cb_t cb,
			  s32_t timeout, void *user_data)
{
	/* NOT IMPLEMENTED */
	return -ENOTSUP;
}

static int offload_sendto(struct net_pkt *pkt, const struct sockaddr *dst_addr,
			  socklen_t addrlen, net_context_send_cb_t cb,
			  s32_t timeout, void *user_data)
{
	struct net_context *context = net_pkt_context(pkt);
	struct hl7800_socket *sock;
	int ret, dst_port = 0;

	if (!context) {
		return -EINVAL;
	}

	sock = (struct hl7800_socket *)context->offload_context;
	if (!sock) {
		LOG_ERR("Can't locate socket for net_ctx:%p!", context);
		return -EINVAL;
	}

#if defined(CONFIG_NET_IPV6)
	if (dst_addr->sa_family == AF_INET6) {
		net_ipaddr_copy(&net_sin6(&sock->dst)->sin6_addr,
				&net_sin6(dst_addr)->sin6_addr);
		dst_port = ntohs(net_sin6(dst_addr)->sin6_port);
		net_sin6(&sock->dst)->sin6_port = dst_port;
	} else
#endif
#if defined(CONFIG_NET_IPV4)
		if (dst_addr->sa_family == AF_INET) {
		net_ipaddr_copy(&net_sin(&sock->dst)->sin_addr,
				&net_sin(dst_addr)->sin_addr);
		dst_port = ntohs(net_sin(dst_addr)->sin_port);
		net_sin(&sock->dst)->sin_port = dst_port;
	} else
#endif
	{
		return -EINVAL;
	}

	ret = send_data(sock, pkt);
	if (ret < 0) {
		LOG_ERR("send_data error: %d", ret);
	}

	net_pkt_unref(pkt);
	if (cb) {
		cb(context, ret, user_data);
	}

	return ret;
}

static int offload_send(struct net_pkt *pkt, net_context_send_cb_t cb,
			s32_t timeout, void *user_data)
{
	struct net_context *context = net_pkt_context(pkt);
	socklen_t addrlen;

	addrlen = 0;
#if defined(CONFIG_NET_IPV6)
	if (net_pkt_family(pkt) == AF_INET6) {
		addrlen = sizeof(struct sockaddr_in6);
	} else
#endif /* CONFIG_NET_IPV6 */
#if defined(CONFIG_NET_IPV4)
		if (net_pkt_family(pkt) == AF_INET) {
		addrlen = sizeof(struct sockaddr_in);
	} else
#endif /* CONFIG_NET_IPV4 */
	{
		return -EPFNOSUPPORT;
	}

	return offload_sendto(pkt, &context->remote, addrlen, cb, timeout,
			      user_data);
}

static int offload_recv(struct net_context *context, net_context_recv_cb_t cb,
			s32_t timeout, void *user_data)
{
	struct hl7800_socket *sock;

	if (!context) {
		return -EINVAL;
	}

	sock = (struct hl7800_socket *)context->offload_context;
	if (!sock) {
		LOG_ERR("Can't locate socket for net_ctx:%p!", context);
		return -EINVAL;
	}

	sock->recv_cb = cb;
	sock->recv_user_data = user_data;

	return 0;
}

static int offload_put(struct net_context *context)
{
	struct hl7800_socket *sock;
	char cmd1[sizeof("AT+KTCPCLOSE=##")];
	char cmd2[sizeof("AT+KTCPDEL=##")];
	int ret;

	if (!context) {
		return -EINVAL;
	}

	sock = (struct hl7800_socket *)context->offload_context;
	if (!sock) {
		/* socket was already closed?  Exit quietly here. */
		return 0;
	}

	/* close connection */
	if (sock->type == SOCK_STREAM) {
		snprintk(cmd1, sizeof(cmd1), "AT+KTCPCLOSE=%d",
			 sock->socket_id);
	} else {
		snprintk(cmd1, sizeof(cmd1), "AT+KUDPCLOSE=%d",
			 sock->socket_id);
	}

	ret = send_at_cmd(sock, cmd1, MDM_CMD_SEND_TIMEOUT, 0);
	if (ret < 0) {
		LOG_ERR("TCP/UDP CLOSE ret:%d", ret);
	}

	if (sock->type == SOCK_STREAM) {
		/* delete TCP session */
		snprintk(cmd2, sizeof(cmd2), "AT+KTCPDEL=%d", sock->socket_id);
		ret = send_at_cmd(sock, cmd2, MDM_CMD_SEND_TIMEOUT, 0);
		if (ret < 0) {
			LOG_ERR("AT+KTCPDEL ret:%d", ret);
		}
	}

	/* clear last_socket_id */
	ictx.last_socket_id = 0;

	sock->context->connect_cb = NULL;
	sock->context->recv_cb = NULL;
	sock->context->send_cb = NULL;
	socket_put(sock);
	net_context_unref(context);

	return 0;
}

static struct net_offload offload_funcs = {
	.get = offload_get,
	.bind = offload_bind,
	.listen = offload_listen, /* TODO */
	.connect = offload_connect,
	.accept = offload_accept, /* TODO */
	.send = offload_send,
	.sendto = offload_sendto,
	.recv = offload_recv,
	.put = offload_put,
};

static inline u8_t *hl7800_get_mac(struct device *dev)
{
	struct hl7800_iface_ctx *ctx = dev->driver_data;

	/* TODO: move mac_addr to Kconfig option */
	ctx->mac_addr[0] = 0x00;
	ctx->mac_addr[1] = 0x10;

	UNALIGNED_PUT(sys_cpu_to_be32(sys_rand32_get()),
		      (u32_t *)(ctx->mac_addr + 2));

	return ctx->mac_addr;
}

static int hl7800_init(struct device *dev)
{
	int i, ret = 0;

	ARG_UNUSED(dev);

	/* check for valid pinconfig */
	__ASSERT(sizeof(pinconfig) == MAX_MDM_CONTROL_PINS,
		 "Incorrect modem pinconfig!");

	(void)memset(&ictx, 0, sizeof(ictx));
	for (i = 0; i < MDM_MAX_SOCKETS; i++) {
		k_work_init(&ictx.sockets[i].recv_cb_work,
			    sockreadrecv_cb_work);
		k_sem_init(&ictx.sockets[i].sock_send_sem, 0, 1);
	}
	k_sem_init(&ictx.response_sem, 0, 1);
	k_sem_init(&ictx.mdm_awake, 0, 1);

	/* initialize the work queue */
	k_work_q_start(&hl7800_workq, hl7800_workq_stack,
		       K_THREAD_STACK_SIZEOF(hl7800_workq_stack),
		       WORKQ_PRIORITY);

	k_work_init(&ictx.iface_status_work, iface_status_work_cb);
	k_delayed_work_init(&ictx.dns_work, dns_work_cb);

	ictx.last_socket_id = 0;

	/* setup port devices and pin directions */
	for (i = 0; i < MAX_MDM_CONTROL_PINS; i++) {
		ictx.gpio_port_dev[i] =
			device_get_binding(pinconfig[i].dev_name);
		if (!ictx.gpio_port_dev[i]) {
			LOG_ERR("gpio port (%s) not found!",
				pinconfig[i].dev_name);
			return -ENODEV;
		}

		ret = gpio_pin_configure(ictx.gpio_port_dev[i],
					 pinconfig[i].pin, pinconfig[i].config);
		if (ret) {
			LOG_ERR("Error configuring io %s %d err: %d!",
				pinconfig[i].dev_name, pinconfig[i].pin, ret);
			return ret;
		}
	}

	/* setup input pin callbacks */
	/* LTE TX ON callback */
	gpio_init_callback(&ictx.mdm_tx_on_cb, mdm_tx_on_callback,
			   BIT(pinconfig[MDM_LTE_TX_ON].pin));
	ret = gpio_add_callback(ictx.gpio_port_dev[MDM_LTE_TX_ON],
				&ictx.mdm_tx_on_cb);
	if (ret) {
		LOG_ERR("Cannot setup tx_on callback! (%d)", ret);
		return ret;
	}
	ret = gpio_pin_enable_callback(ictx.gpio_port_dev[MDM_LTE_TX_ON],
				       pinconfig[MDM_LTE_TX_ON].pin);
	if (ret) {
		LOG_ERR("Error enabling tx_on callback! (%d)", ret);
		return ret;
	}

	/* VGPIO */
	gpio_init_callback(&ictx.mdm_vgpio_cb, mdm_vgpio_callback,
			   BIT(pinconfig[MDM_VGPIO].pin));
	ret = gpio_add_callback(ictx.gpio_port_dev[MDM_VGPIO],
				&ictx.mdm_vgpio_cb);
	if (ret) {
		LOG_ERR("Cannot setup vgpio callback! (%d)", ret);
		return ret;
	}
	ret = gpio_pin_enable_callback(ictx.gpio_port_dev[MDM_VGPIO],
				       pinconfig[MDM_VGPIO].pin);
	if (ret) {
		LOG_ERR("Error enabling vgpio callback! (%d)", ret);
		return ret;
	}

	/* UART DSR */
	gpio_init_callback(&ictx.mdm_uart_dsr_cb, mdm_uart_dsr_callback,
			   BIT(pinconfig[MDM_UART_DSR].pin));
	ret = gpio_add_callback(ictx.gpio_port_dev[MDM_UART_DSR],
				&ictx.mdm_uart_dsr_cb);
	if (ret) {
		LOG_ERR("Cannot setup uart dsr callback! (%d)", ret);
		return ret;
	}
	ret = gpio_pin_enable_callback(ictx.gpio_port_dev[MDM_UART_DSR],
				       pinconfig[MDM_UART_DSR].pin);
	if (ret) {
		LOG_ERR("Error enabling uart dsr callback! (%d)", ret);
		return ret;
	}

	/* Set modem data storage */
	ictx.mdm_ctx.data_manufacturer = ictx.mdm_manufacturer;
	ictx.mdm_ctx.data_model = ictx.mdm_model;
	ictx.mdm_ctx.data_revision = ictx.mdm_revision;
	ictx.mdm_ctx.data_imei = ictx.mdm_imei;

	ret = mdm_receiver_register(&ictx.mdm_ctx, MDM_UART_DEV_NAME,
				    mdm_recv_buf, sizeof(mdm_recv_buf));
	if (ret < 0) {
		LOG_ERR("Error registering modem receiver (%d)!", ret);
		return ret;
	}

	/* start RX thread */
	k_thread_create(&hl7800_rx_thread, hl7800_rx_stack,
			K_THREAD_STACK_SIZEOF(hl7800_rx_stack),
			(k_thread_entry_t)hl7800_rx, NULL, NULL, NULL,
			RX_THREAD_PRIORITY, 0, K_NO_WAIT);

	/* init RSSI query */
	k_delayed_work_init(&ictx.rssi_query_work, hl7800_rssi_query_work);

	hl7800_modem_reset();

	return ret;
}

static void offload_iface_init(struct net_if *iface)
{
	struct device *dev = net_if_get_device(iface);
	struct hl7800_iface_ctx *ctx = dev->driver_data;

	iface->if_dev->offload = &offload_funcs;
	net_if_set_link_addr(iface, hl7800_get_mac(dev), sizeof(ctx->mac_addr),
			     NET_LINK_ETHERNET);
	ctx->iface = iface;
#if defined(CONFIG_NET_IPV4)
	net_if_set_mtu(ctx->iface, 1500 + NET_IPV4TCPH_LEN);
#endif
	/* TODO: set proper IPv6 MTU */
	/* Update the iface */
	iface_status_work_cb(NULL);
	ictx.initialized = true;
}

static struct net_if_api api_funcs = {
	.init = offload_iface_init,
};

NET_DEVICE_OFFLOAD_INIT(modem_hl7800, "MODEM_HL7800", hl7800_init, &ictx, NULL,
			CONFIG_MODEM_HL7800_INIT_PRIORITY, &api_funcs,
			MDM_MAX_DATA_LENGTH);
