#include "net_schedule.h"
#include "net_driver.h"
#include "net_ipset.h"
#include "net_raw_driver_i.h"
#include "net_raw_device_i.h"
#include "net_raw_endpoint.h"
#include "net_raw_dgram.h"
#include "net_raw_device_raw_capture_i.h"

static int net_raw_driver_init(net_driver_t driver);
static void net_raw_driver_fini(net_driver_t driver);
static void net_raw_driver_tcp_timer_cb(EV_P_ ev_timer *watcher, int revents);

net_raw_driver_t
net_raw_driver_create(net_schedule_t schedule, void * ev_loop, net_raw_driver_match_mode_t mode) {
    net_driver_t base_driver;

    base_driver = net_driver_create(
        schedule,
        "raw",
        /*driver*/
        sizeof(struct net_raw_driver),
        net_raw_driver_init,
        net_raw_driver_fini,
        /*timer*/
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        /*endpoint*/
        sizeof(struct net_raw_endpoint),
        net_raw_endpoint_init,
        net_raw_endpoint_fini,
        net_raw_endpoint_connect,
        net_raw_endpoint_close,
        net_raw_endpoint_on_output,
        /*dgram*/
        sizeof(struct net_raw_dgram),
        net_raw_dgram_init,
        net_raw_dgram_fini,
        net_raw_dgram_send);

    if (base_driver == NULL) return NULL;

    net_raw_driver_t driver = net_driver_data(base_driver);
    driver->m_ev_loop = ev_loop;
    driver->m_mode = mode;
    ev_timer_start(driver->m_ev_loop, &driver->m_tcp_timer);

    lwip_init();
    
    return net_driver_data(base_driver);
}

net_raw_driver_t net_raw_driver_cast(net_driver_t driver) {
    return strcmp(net_driver_name(driver), "raw") == 0 ? net_driver_data(driver) : NULL;
}

static int net_raw_driver_init(net_driver_t base_driver) {
    net_schedule_t schedule = net_driver_schedule(base_driver);
    net_raw_driver_t driver = net_driver_data(base_driver);

    driver->m_alloc = net_schedule_allocrator(schedule);
    driver->m_em = net_schedule_em(schedule);
    driver->m_ev_loop = NULL;
    driver->m_mode = net_raw_driver_match_white;
    driver->m_ipset = NULL;
    TAILQ_INIT(&driver->m_devices);
    TAILQ_INIT(&driver->m_free_device_raw_captures);    
    driver->m_sock_process_fun = NULL;
    driver->m_sock_process_ctx = NULL;
    driver->m_data_monitor_fun = NULL;
    driver->m_data_monitor_ctx = NULL;
    driver->m_debug = 0;

    double tcp_timer_interval = ((double)TCP_TMR_INTERVAL / 1000.0);
    ev_timer_init(&driver->m_tcp_timer, net_raw_driver_tcp_timer_cb, tcp_timer_interval, tcp_timer_interval);

    mem_buffer_init(&driver->m_data_buffer, driver->m_alloc);
    
    return 0;
}

static void net_raw_driver_fini(net_driver_t base_driver) {
    net_raw_driver_t driver = net_driver_data(base_driver);

    ev_timer_stop(driver->m_ev_loop, &driver->m_tcp_timer);
    
    while(!TAILQ_EMPTY(&driver->m_devices)) {
        net_raw_device_free(TAILQ_FIRST(&driver->m_devices));
    }

    if (driver->m_ipset) {
        net_ipset_free(driver->m_ipset);
    }

    while(!TAILQ_EMPTY(&driver->m_free_device_raw_captures)) {
        net_raw_device_raw_capture_real_free(TAILQ_FIRST(&driver->m_free_device_raw_captures));
    }

    mem_buffer_clear(&driver->m_data_buffer);
}

void net_raw_driver_free(net_raw_driver_t driver) {
    net_driver_free(net_driver_from_data(driver));
}

net_raw_driver_match_mode_t net_raw_driver_match_mode(net_raw_driver_t driver) {
    return driver->m_mode;
}

net_ipset_t net_raw_driver_ipset(net_raw_driver_t driver) {
    return driver->m_ipset;
}

net_ipset_t net_raw_driver_ipset_check_create(net_raw_driver_t driver) {
    if (driver->m_ipset == NULL) {
        driver->m_ipset = net_ipset_create(net_raw_driver_schedule(driver));
        if (driver->m_ipset == NULL) {
            CPE_ERROR(driver->m_em, "raw: driver create ipset fail!");
            return NULL;
        }
    }

    return driver->m_ipset;
}

uint8_t net_raw_driver_debug(net_raw_driver_t driver) {
    return driver->m_debug;
}

void net_raw_driver_set_debug(net_raw_driver_t driver, uint8_t debug) {
    driver->m_debug = debug;
}

void net_raw_driver_set_sock_create_processor(
    net_raw_driver_t driver,
    net_raw_driver_sock_create_process_fun_t process_fun,
    void * process_ctx)
{
    driver->m_sock_process_fun = process_fun;
    driver->m_sock_process_ctx = process_ctx;
}
    
void net_raw_driver_set_data_monitor(
    net_raw_driver_t driver,
    net_data_monitor_fun_t monitor_fun, void * monitor_ctx)
{
    driver->m_data_monitor_fun = monitor_fun;
    driver->m_data_monitor_ctx = monitor_ctx;
}

net_schedule_t net_raw_driver_schedule(net_raw_driver_t driver) {
    return net_driver_schedule(net_driver_from_data(driver));
}

mem_buffer_t net_raw_driver_tmp_buffer(net_raw_driver_t driver) {
    return net_schedule_tmp_buffer(net_driver_schedule(net_driver_from_data(driver)));
}

static void net_raw_driver_tcp_timer_cb(EV_P_ ev_timer *watcher, int revents) {
    tcp_tmr();
    return;
}