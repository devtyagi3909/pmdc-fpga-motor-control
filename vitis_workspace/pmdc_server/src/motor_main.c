/*
 * motor_main.c — entry point for PMDC Motor TCP Streaming Server
 *
 * Based on the standard Xilinx lwIP raw-API echo server template.
 * Static IP (DHCP disabled): 192.168.1.10 / 255.255.255.0, gw 192.168.1.1
 *
 * Set your PC's Ethernet adapter to a static IP on the same subnet,
 * e.g. 192.168.1.5 / 255.255.255.0, connect via Ethernet cable
 * (direct or through a switch) to the Zybo's Ethernet port.
 */

#include <stdio.h>

#include "xparameters.h"
#include "netif/xadapter.h"
#include "platform.h"
#include "platform_config.h"

#if defined (__arm__) || defined(__aarch64__)
#include "xil_printf.h"
#endif

#include "lwip/tcp.h"
#include "xil_cache.h"

#if LWIP_IPV6==1
#include "lwip/ip.h"
#else
#if LWIP_DHCP==1
#include "lwip/dhcp.h"
#endif
#endif

/* Defined in motor_echo.c */
void print_app_header();
int  start_application();
int  send_samples(void);
void tcp_fasttmr(void);
void tcp_slowtmr(void);

void lwip_init();

#if LWIP_IPV6==0
#if LWIP_DHCP==1
extern volatile int dhcp_timoutcntr;
err_t dhcp_start(struct netif *netif);
#endif
#endif

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
static struct netif server_netif;
struct netif *echo_netif;

#if LWIP_IPV6==0
void print_ip(char *msg, ip_addr_t *ip)
{
    print(msg);
    xil_printf("%d.%d.%d.%d\n\r", ip4_addr1(ip), ip4_addr2(ip),
            ip4_addr3(ip), ip4_addr4(ip));
}

void print_ip_settings(ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw)
{
    print_ip("Board IP:   ", ip);
    print_ip("Netmask :   ", mask);
    print_ip("Gateway :   ", gw);
}
#endif

int main()
{
#if LWIP_IPV6==0
    ip_addr_t ipaddr, netmask, gw;
#endif

    /* Unique MAC address for this board */
    unsigned char mac_ethernet_address[] =
    { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

    echo_netif = &server_netif;

    init_platform();

#if LWIP_IPV6==0
#if LWIP_DHCP==1
    ipaddr.addr  = 0;
    gw.addr      = 0;
    netmask.addr = 0;
#else
    /* Static IP — must be DHCP-disabled in lwIP BSP settings */
    IP4_ADDR(&ipaddr,  192, 168,   1, 10);
    IP4_ADDR(&netmask, 255, 255, 255,  0);
    IP4_ADDR(&gw,      192, 168,   1,  1);
#endif
#endif

    print_app_header();

    lwip_init();

#if (LWIP_IPV6 == 0)
    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw,
                    mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding N/W interface\n\r");
        return -1;
    }
#else
    if (!xemac_add(echo_netif, NULL, NULL, NULL,
                    mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding N/W interface\n\r");
        return -1;
    }
    echo_netif->ip6_autoconfig_enabled = 1;
    netif_create_ip6_linklocal_address(echo_netif, 1);
    netif_ip6_addr_set_state(echo_netif, 0, IP6_ADDR_VALID);
#endif

    netif_set_default(echo_netif);
    platform_enable_interrupts();
    netif_set_up(echo_netif);

#if (LWIP_IPV6 == 0)
#if (LWIP_DHCP==1)
    dhcp_start(echo_netif);
    dhcp_timoutcntr = 24;

    while (((echo_netif->ip_addr.addr) == 0) && (dhcp_timoutcntr > 0))
        xemacif_input(echo_netif);

    if (dhcp_timoutcntr <= 0) {
        if ((echo_netif->ip_addr.addr) == 0) {
            xil_printf("DHCP Timeout\r\n");
            xil_printf("Configuring default IP of 192.168.1.10\r\n");
            IP4_ADDR(&(echo_netif->ip_addr),  192, 168,   1, 10);
            IP4_ADDR(&(echo_netif->netmask), 255, 255, 255,  0);
            IP4_ADDR(&(echo_netif->gw),      192, 168,   1,  1);
        }
    }

    ipaddr.addr  = echo_netif->ip_addr.addr;
    gw.addr      = echo_netif->gw.addr;
    netmask.addr = echo_netif->netmask.addr;
#endif

    print_ip_settings(&ipaddr, &netmask, &gw);
#endif

    /* Init motor + DMA + TCP server (motor_echo.c) */
    start_application();

    /* Main loop: stream motor data + service TCP/IP stack */
    while (1) {
        send_samples();
        xemacif_input(echo_netif);
    }

    cleanup_platform();
    return 0;
}
