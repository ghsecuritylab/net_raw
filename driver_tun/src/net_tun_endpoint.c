#include "assert.h"
#include "cpe/pal/pal_socket.h"
#include "cpe/pal/pal_string.h"
#include "cpe/pal/pal_strings.h"
#include "cpe/utils/string_utils.h"
#include "cpe/utils_sock/sock_utils.h"
#include "net_endpoint.h"
#include "net_address.h"
#include "net_driver.h"
#include "net_tun_endpoint.h"
#include "net_tun_utils.h"

static err_t net_tun_endpoint_recv_func(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t net_tun_endpoint_sent_func(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void net_tun_endpoint_err_func(void *arg, err_t err);
static err_t net_tun_endpoint_connected_func(void *arg, struct tcp_pcb *tpcb, err_t err);
static int net_tun_endpoint_do_write(struct net_tun_endpoint * endpoint);

void net_tun_endpoint_set_pcb(struct net_tun_endpoint * endpoint, struct tcp_pcb * pcb) {
    if (endpoint->m_pcb) {
        tcp_err(endpoint->m_pcb, NULL);
        tcp_recv(endpoint->m_pcb, NULL);
        tcp_sent(endpoint->m_pcb, NULL);
        tcp_close(endpoint->m_pcb);
        endpoint->m_pcb = NULL;
    }

    endpoint->m_pcb = pcb;

    if (endpoint->m_pcb) {
        tcp_nagle_disable(endpoint->m_pcb);
        tcp_arg(endpoint->m_pcb, endpoint);
        tcp_err(endpoint->m_pcb, net_tun_endpoint_err_func);
        tcp_recv(endpoint->m_pcb, net_tun_endpoint_recv_func);
        tcp_sent(endpoint->m_pcb, net_tun_endpoint_sent_func);
    }
}

static err_t net_tun_endpoint_recv_func(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    net_tun_endpoint_t endpoint = arg;
    net_endpoint_t base_endpoint = net_endpoint_from_data(endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));
    net_schedule_t schedule = net_endpoint_schedule(base_endpoint);

    assert(err == ERR_OK); /* checked in lwIP source. Otherwise, I've no idea what should
                              be done with the pbuf in case of an error.*/

    if (net_endpoint_state(base_endpoint) == net_endpoint_state_deleting) {
        return ERR_RST;
    }
    
    if (!p) {
        if (net_endpoint_driver_debug(base_endpoint) >= 2) {
            CPE_INFO(
                driver->m_em, "tun: %s: client closed",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint));
        }

        net_endpoint_set_error(base_endpoint, net_endpoint_error_source_network, net_endpoint_network_errno_remote_closed, NULL);
        if (net_endpoint_set_state(base_endpoint, net_endpoint_state_disable) != 0) {
            net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
            return ERR_CLSD;
        }
        else {
            return endpoint->m_pcb == NULL ? ERR_ABRT : ERR_OK;
        }
    }

    assert(p->tot_len > 0);
    uint32_t total_len = p->tot_len;
    
    uint32_t capacity = total_len;

    void * data = net_endpoint_buf_alloc(base_endpoint, &capacity);
    if (data == NULL) {
        CPE_ERROR(
            driver->m_em, "tun: %s: no buffer for data, size=%d",
            net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint), capacity);
        return ERR_MEM;
    }

    pbuf_copy_partial(p, data, total_len, 0);
    pbuf_free(p);

    tcp_recved(endpoint->m_pcb, total_len);
    
    if (net_endpoint_driver_debug(base_endpoint) || net_schedule_debug(schedule) >= 2) {
        CPE_INFO(
            driver->m_em, "tun: %s: recv %d bytes",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint), total_len);
    }

    if (net_endpoint_buf_supply(base_endpoint, net_ep_buf_read, total_len) != 0) {
        if (net_endpoint_is_active(base_endpoint)) {
            if (!net_endpoint_have_error(base_endpoint)) {
                net_endpoint_set_error(base_endpoint, net_endpoint_error_source_network, net_endpoint_network_errno_logic, NULL);
            }
            if (net_endpoint_set_state(base_endpoint, net_endpoint_state_logic_error) != 0) {
                if (net_endpoint_driver_debug(base_endpoint) || net_schedule_debug(schedule) >= 2) {
                    CPE_INFO(
                        driver->m_em, "tun: %s: free for process fail!",
                        net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
                }
                net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
            }
        }
    }

    return endpoint->m_pcb == NULL ? ERR_ABRT : ERR_OK;
}

static err_t net_tun_endpoint_sent_func(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    net_tun_endpoint_t endpoint = arg;
    net_endpoint_t base_endpoint = net_endpoint_from_data(endpoint);

    if (net_tun_endpoint_do_write(endpoint) != 0) {
        net_endpoint_set_error(
            base_endpoint, net_endpoint_error_source_network,
            net_endpoint_network_errno_network_error, "tun write error");
        if (net_endpoint_set_state(base_endpoint, net_endpoint_state_network_error) != 0) {
            net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
            return ERR_CLSD; 
        }
    }

    return endpoint->m_pcb == NULL ? ERR_ABRT : ERR_OK;
}

static void net_tun_endpoint_err_func(void *arg, err_t err) {
    net_tun_endpoint_t endpoint = arg;
    net_endpoint_t base_endpoint = net_endpoint_from_data(endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));

    if (err != ERR_ABRT) {
        tcp_err(endpoint->m_pcb, NULL);
        tcp_recv(endpoint->m_pcb, NULL);
        tcp_sent(endpoint->m_pcb, NULL);
        endpoint->m_pcb = NULL;
    }
    else {
        endpoint->m_pcb = NULL;
    }

    if (err == ERR_RST) {
        if (net_endpoint_driver_debug(base_endpoint)) {
            CPE_INFO(
                driver->m_em, "tun: %s: remote disconnected!",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint));
        }

        net_endpoint_set_error(base_endpoint, net_endpoint_error_source_network, net_endpoint_network_errno_remote_closed, NULL);
        if (net_endpoint_set_state(base_endpoint, net_endpoint_state_disable) != 0) {
            net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
        }
    }
    else {
        if (net_endpoint_driver_debug(base_endpoint)) {
            CPE_INFO(
                driver->m_em, "tun: %s: error %d (%s)",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint),
                (int)err, lwip_strerr(err));
        }
        
        net_endpoint_set_error(base_endpoint, net_endpoint_error_source_network, net_endpoint_network_errno_network_error, lwip_strerr(err));
        if (net_endpoint_set_state(base_endpoint, net_endpoint_state_network_error) != 0) {
            net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
        }
    }
}

static err_t net_tun_endpoint_connected_func(void *arg, struct tcp_pcb *tpcb, err_t err) {
    net_tun_endpoint_t endpoint = arg;
    net_endpoint_t base_endpoint = net_endpoint_from_data(endpoint);
    net_schedule_t schedule = net_endpoint_schedule(base_endpoint);
    error_monitor_t em = net_schedule_em(schedule);

    if (err != ERR_OK) {
        CPE_ERROR(
            em, "ev: %s: connect error, errno=%d (%s)",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint), err, lwip_strerr(err));
        net_endpoint_set_error(
            base_endpoint, net_endpoint_error_source_network,
            net_endpoint_network_errno_network_error, lwip_strerr(err));
        if (net_endpoint_set_state(base_endpoint, net_endpoint_state_network_error) != 0) {
            net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
            return ERR_CLSD;
        }
        return endpoint->m_pcb == NULL ? ERR_ABRT : ERR_OK;
    }

    if (net_endpoint_driver_debug(base_endpoint) || net_schedule_debug(schedule) >= 2) {
        CPE_INFO(
            em, "ev: %s: connect success",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
    }

    if (net_endpoint_set_state(base_endpoint, net_endpoint_state_established) != 0) {
        net_endpoint_set_state(base_endpoint, net_endpoint_state_deleting);
        return ERR_CLSD;
    }

    return endpoint->m_pcb == NULL ? ERR_ABRT : ERR_OK;
}

int net_tun_endpoint_init(net_endpoint_t base_endpoint) {
    net_tun_endpoint_t endpoint = net_endpoint_data(base_endpoint);
    endpoint->m_pcb = NULL;
    return 0;
}

void net_tun_endpoint_fini(net_endpoint_t base_endpoint) {
    net_tun_endpoint_t endpoint = net_endpoint_data(base_endpoint);
    //net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));

    if (endpoint->m_pcb) {
        net_tun_endpoint_set_pcb(endpoint, NULL);
        assert(endpoint->m_pcb == NULL);
    }
}

int net_tun_endpoint_update(net_endpoint_t base_endpoint) {
    if (net_endpoint_state(base_endpoint) != net_endpoint_state_established) return 0;

    net_tun_endpoint_t endpoint = net_endpoint_data(base_endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));

    if (endpoint->m_pcb == NULL) {
        CPE_ERROR(
            driver->m_em, "tun: %s: on output: not connected!",
            net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint));
        return -1;
    }

    return net_tun_endpoint_do_write(endpoint);
}

static int net_tun_endpoint_do_write(struct net_tun_endpoint * endpoint) {
    net_endpoint_t base_endpoint = net_endpoint_from_data(endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));

    assert(endpoint->m_pcb);
    while(net_endpoint_state(base_endpoint) == net_endpoint_state_established
          && !net_endpoint_buf_is_empty(base_endpoint, net_ep_buf_write))
    {
        uint32_t data_size;
        void * data = net_endpoint_buf_peak(base_endpoint, net_ep_buf_write, &data_size);

        assert(data_size > 0);
        assert(data);

        if (data_size > tcp_sndbuf(endpoint->m_pcb)) {
            data_size = tcp_sndbuf(endpoint->m_pcb);
        }
        
        if (data_size == 0) {
            break;
        }

        err_t err = tcp_write(endpoint->m_pcb, data, data_size, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            if (err == ERR_MEM) {
                break;
            }

            CPE_ERROR(
                driver->m_em, "tun: %s: write: tcp_write fail %d (%s)!",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint), err, lwip_strerr(err));
            
            return -1;
        }

        if (net_endpoint_driver_debug(base_endpoint)) {
            CPE_INFO(
                driver->m_em, "tun: %s: send %d bytes data!",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint),
                data_size);
        }
        
        net_endpoint_buf_consume(base_endpoint, net_ep_buf_write, data_size);
    }

    if (net_endpoint_state(base_endpoint) == net_endpoint_state_established) {
        err_t err = tcp_output(endpoint->m_pcb);
        if (err != ERR_OK) {
            CPE_ERROR(
                driver->m_em, "tun: %s: write: tcp_output fail %d (%s)!",
                net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint), err, lwip_strerr(err));
            return -1;
        }
    }

    return 0;
}

int net_tun_endpoint_connect(net_endpoint_t base_endpoint) {
    net_tun_endpoint_t endpoint = net_endpoint_data(base_endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));
    net_schedule_t schedule = net_endpoint_schedule(base_endpoint);
    error_monitor_t em = net_schedule_em(schedule);

    if (endpoint->m_pcb != NULL) {
        CPE_ERROR(
            em, "tun: %s: already connected!",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
        return -1;
    }

    net_address_t remote_addr = net_endpoint_remote_address(base_endpoint);
    if (remote_addr == NULL) {
        CPE_ERROR(
            em, "tun: %s: connect with no remote address!",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
        return -1;
    }
    
    struct tcp_pcb * pcb = NULL;
    pcb = tcp_new();
    if (pcb == NULL) {
        CPE_ERROR(
            em, "tun: %s: connect: create pcb fail!",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
        return -1;
    }

    net_address_t local_address = net_endpoint_address(base_endpoint);
    if (local_address) {
        ip_addr_t local_lwip_addr;

        switch(net_address_type(local_address)) {
        case net_address_ipv4:
            local_lwip_addr.type = IPADDR_TYPE_V4;
            net_address_to_lwip_ipv4(&local_lwip_addr.u_addr.ip4, local_address);
            break;
        case net_address_ipv6:
            local_lwip_addr.type = IPADDR_TYPE_V6;
            net_address_to_lwip_ipv6(&local_lwip_addr.u_addr.ip6, local_address);
            break;
        case net_address_domain:
        case net_address_local:
            CPE_ERROR(
                em, "tun: %s: connect not support %s address!",
                net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint),
                net_address_type_str(net_address_type(local_address)));
            tcp_abort(pcb);
            return -1;
        }
            
        err_t err = tcp_bind(pcb, &local_lwip_addr, net_address_port(local_address));
        if (err != ERR_OK) {
            char ip_buf[64];
            cpe_str_dup(ip_buf, sizeof(ip_buf), net_address_dump(net_schedule_tmp_buffer(schedule), local_address));
                
            CPE_ERROR(
                em, "tun: %s: bind %s fail, error=%d (%s)",
                net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint), ip_buf, err, lwip_strerr(err));
            tcp_abort(pcb);
            return -1;
        }
    }

    ip_addr_t remote_lwip_addr;
    switch(net_address_type(remote_addr)) {
    case net_address_ipv4:
        remote_lwip_addr.type = IPADDR_TYPE_V4;
        net_address_to_lwip_ipv4(&remote_lwip_addr.u_addr.ip4, remote_addr);
        break;
    case net_address_ipv6:
        remote_lwip_addr.type = IPADDR_TYPE_V6;
        net_address_to_lwip_ipv6(&remote_lwip_addr.u_addr.ip6, remote_addr);
        break;
    case net_address_domain:
    case net_address_local:
        CPE_ERROR(
            em, "tun: %s: connect not support %s!",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint),
            net_address_type_str(net_address_type(remote_addr)));
        tcp_abort(pcb);
        return -1;
    }

    err_t err = tcp_connect(pcb, &remote_lwip_addr, net_address_port(remote_addr), net_tun_endpoint_connected_func);
    if (err != ERR_OK) {
        CPE_ERROR(
            em, "tun: %s: connect error, error=%d (%s)",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint), err, lwip_strerr(err));
        tcp_abort(pcb);
        return -1;
    }

    if (local_address) {
        net_address_set_port(local_address, pcb->local_port);
    }
    else {
        local_address = net_address_from_lwip(driver, &pcb->local_ip, pcb->local_port);
        if (local_address == NULL) {
            CPE_ERROR(
                em, "tun: %s: connect success, create local address fail",
                net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
            tcp_abort(pcb);
            return -1;
        }

        net_endpoint_set_address(base_endpoint, local_address, 1);
    }

    if (net_schedule_debug(schedule) >= 2) {
        CPE_INFO(
            em, "tun: %s: connect start",
            net_endpoint_dump(net_schedule_tmp_buffer(schedule), base_endpoint));
    }

    net_tun_endpoint_set_pcb(endpoint, pcb);
    
    return net_endpoint_set_state(base_endpoint, net_endpoint_state_connecting);
}

void net_tun_endpoint_close(net_endpoint_t base_endpoint) {
    net_tun_endpoint_t endpoint = net_endpoint_data(base_endpoint);
    net_tun_driver_t driver = net_driver_data(net_endpoint_driver(base_endpoint));

    if (endpoint->m_pcb == NULL) return;

    tcp_err(endpoint->m_pcb, NULL);
    tcp_recv(endpoint->m_pcb, NULL);
    tcp_sent(endpoint->m_pcb, NULL);
    tcp_poll(endpoint->m_pcb, NULL, 0);
    
    err_t err = tcp_close(endpoint->m_pcb);
    if (err != ERR_OK) {
        CPE_ERROR(
            driver->m_em, "tun: %s: tcp close failed, error=%d (%s)",
            net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint),
            err, lwip_strerr(err));
        net_tun_endpoint_set_pcb(endpoint, NULL);
        return;
    }
    endpoint->m_pcb = NULL;
    
    if (net_endpoint_driver_debug(base_endpoint) >= 2) {
        CPE_INFO(
            driver->m_em, "tun: %s: tcp closed",
            net_endpoint_dump(net_tun_driver_tmp_buffer(driver), base_endpoint));
    }
}

