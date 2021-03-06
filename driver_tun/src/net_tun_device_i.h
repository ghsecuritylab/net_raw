#ifndef NET_TUN_DEVICE_I_H_INCLEDED
#define NET_TUN_DEVICE_I_H_INCLEDED
#include "net_tun_device.h"
#include "net_tun_driver_i.h"

#if NET_TUN_USE_DEV_NE
@interface NetTunDeviceBridger : NSObject {
    @public net_tun_device_t m_device;
}
@end
#endif

struct net_tun_device {
    net_tun_driver_t m_driver;
    TAILQ_ENTRY(net_tun_device) m_next_for_driver;
    struct netif m_netif;
    struct tcp_pcb * m_listener_ip4;
    struct tcp_pcb * m_listener_ip6;
    net_address_t m_netif_ipv4_address;
    net_address_t m_netif_ipv6_address;
    uint16_t m_mtu;
    uint8_t * m_output_buf;
    uint16_t m_output_capacity;
    uint8_t m_quitting;
    net_address_t m_ipv4_address;
    net_address_t m_ipv4_mask;
    net_address_t m_ipv6_address;
    char m_dev_name[16];
    /*使用tun设备接口 */
#if NET_TUN_USE_DEV_TUN
    uint8_t m_dev_fd_close;
    int m_dev_fd;
    net_watcher_t m_watcher;
#endif

    /*使用NetworkExtention设备接口 */
#if NET_TUN_USE_DEV_NE
    __unsafe_unretained NetTunDeviceBridger * m_bridger;
    __unsafe_unretained NEPacketTunnelFlow * m_tunnelFlow;
    __unsafe_unretained NSMutableArray<NSData *> * m_packets;
    __unsafe_unretained NSMutableArray<NSNumber *> * m_versions;
#endif
};

#if NET_TUN_USE_DEV_TUN
int net_tun_device_init_dev_by_fd(
    net_tun_driver_t driver, net_tun_device_t device
    , int dev_fd
    , uint16_t dev_mtu
    , net_address_t dev_ipv4_address
    , net_address_t dev_ipv4_mask
    , net_address_t dev_ipv6_address);

int net_tun_device_init_dev_by_name(net_tun_driver_t driver, net_tun_device_t device, const char * name);
#endif

#if NET_TUN_USE_DEV_NE
int net_tun_device_init_dev(
    net_tun_driver_t driver, net_tun_device_t device,
    NEPacketTunnelFlow * tunnelFlow,
    NEPacketTunnelNetworkSettings * settings);
#endif

void net_tun_device_fini_dev(net_tun_driver_t driver, net_tun_device_t device);

int net_tun_device_packet_input(net_tun_driver_t driver, net_tun_device_t device, uint8_t const * data, uint16_t bytes);
int net_tun_device_packet_output(net_tun_device_t device, uint8_t *data, int data_len);

#endif
