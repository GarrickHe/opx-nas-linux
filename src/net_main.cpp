/*
 * Copyright (c) 2016 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*!
 * \file   net_main.c
 * \brief  Thread for all notification from kernel
 * \date   11-2013
 */


#include "ds_api_linux_interface.h"
#include "ds_api_linux_neigh.h"

#include "nas_os_if_priv.h"
#include "os_if_utils.h"

#include "event_log.h"
#include "ds_api_linux_route.h"

#include "std_utils.h"

#include "db_linux_event_register.h"

#include "cps_api_interface_types.h"
#include "cps_api_object_category.h"
#include "cps_api_operation.h"

#include "std_socket_tools.h"
#include "netlink_tools.h"
#include "std_thread_tools.h"
#include "std_ip_utils.h"
#include "nas_nlmsg.h"

#include "dell-base-l2-mac.h"
#include "cps_api_route.h"
#include "nas_nlmsg_object_utils.h"
#include "netlink_stats.h"

#include <limits.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

#include <netinet/in.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <linux/if.h>
#include <map>

/*
 * Global variables
 */

typedef bool (*fn_nl_msg_handle)(int sock, int type, struct nlmsghdr * nh, void *context);

static auto nlm_sockets = new std::map<int,nas_nl_sock_TYPES>;

static INTERFACE *g_if_db;
INTERFACE *os_get_if_db_hdlr() {
    return g_if_db;
}

static if_bridge *g_if_bridge_db;
if_bridge *os_get_bridge_db_hdlr() {
    return g_if_bridge_db;
}

static if_bond *g_if_bond_db;
if_bond *os_get_bond_db_hdlr() {
    return g_if_bond_db;
}

extern "C" {

/*
 * Pthread variables
 */
static uint64_t                     _local_event_count = 0;
static std_thread_create_param_t      _net_main_thr;
static cps_api_event_service_handle_t         _handle;

const static int MAX_CPS_MSG_SIZE=10000;

/*
 * Functions
 */

#define KN_DEBUG(x,...) EV_LOG_TRACE (ev_log_t_NETLINK,0,"NL-DBG",x, ##__VA_ARGS__)

cps_api_return_code_t net_publish_event(cps_api_object_t msg) {
    cps_api_return_code_t rc = cps_api_ret_code_OK;
    ++_local_event_count;
    rc = cps_api_event_publish(_handle,msg);
    cps_api_object_delete(msg);
    return rc;
}

void cps_api_event_count_clear(void) {
    _local_event_count = 0;
}

uint64_t cps_api_event_count_get(void) {
    return _local_event_count;
}

void rta_add_mac( struct nlattr* rtatp, cps_api_object_t obj, uint32_t attr) {
    cps_api_object_attr_add(obj,attr,nla_data(rtatp),nla_len(rtatp));
}

void rta_add_mask(int family, uint_t prefix_len, cps_api_object_t obj, uint32_t attr) {
    hal_ip_addr_t mask;
    std_ip_get_mask_from_prefix_len(family,prefix_len,&mask);
    cps_api_object_attr_add(obj,attr,&mask,sizeof(mask));
}

void rta_add_e_ip( struct nlattr* rtatp, cps_api_object_t obj,
        cps_api_attr_id_t *attr, size_t attr_id_len) {
    hal_ip_addr_t ip;
    size_t len = nla_len(rtatp);
    if(len == HAL_INET4_LEN) {
        struct in_addr *inp = (struct in_addr *) nla_data(rtatp);
        std_ip_from_inet(&ip,inp);
    } else if (len == HAL_INET6_LEN) {
        struct in6_addr *inp6 = (struct in6_addr *) nla_data(rtatp);
        std_ip_from_inet6(&ip,inp6);
    }else{
        EV_LOGGING(NETLINK,ERR,"ADD-IP","Invalied IP length %d",len);
        return;
    }
    cps_api_object_e_add(obj,attr,attr_id_len, cps_api_object_ATTR_T_BIN,
            &ip,sizeof(ip));
}

unsigned int rta_add_name( struct nlattr* rtatp,cps_api_object_t obj, uint32_t attr_id) {
    char buff[PATH_MAX];
    memset(buff,0,sizeof(buff));
    size_t len = (size_t)nla_len(rtatp)  < (sizeof(buff)-1) ? nla_len(rtatp) : sizeof(buff)-1;
    memcpy(buff,nla_data(rtatp),len);
    len = strlen(buff)+1;
    cps_api_object_attr_add(obj,attr_id,buff,len);
    return len;
}

static bool get_netlink_data(int sock, int rt_msg_type, struct nlmsghdr *hdr, void *data) {
    static char buff[MAX_CPS_MSG_SIZE];

    cps_api_object_t obj = cps_api_object_init(buff,sizeof(buff));

    if (rt_msg_type < RTM_BASE)
        return false;

    /*!
     * Range upto SET_LINK
     */
    if (rt_msg_type <= RTM_SETLINK) {
        nas_nl_stats_update_tot_msg (sock, rt_msg_type);
        if (os_interface_to_object(rt_msg_type,hdr,obj)) {
            nas_nl_stats_update_pub_msg (sock, rt_msg_type);
            if (net_publish_event(obj) != cps_api_ret_code_OK) {
                nas_nl_stats_update_pub_msg_failed (sock, rt_msg_type);
            }
        } else {
            nas_nl_stats_update_invalid_msg (sock, rt_msg_type);
        }
        return true;
    }

    /*!
     * Range upto GET_ADDRRESS
     */
    if (rt_msg_type <= RTM_GETADDR) {
        nas_nl_stats_update_tot_msg (sock, rt_msg_type);
        if (nl_get_ip_info(rt_msg_type,hdr,obj)) {
            nas_nl_stats_update_pub_msg (sock, rt_msg_type);
            if (net_publish_event(obj) != cps_api_ret_code_OK) {
                nas_nl_stats_update_pub_msg_failed (sock, rt_msg_type);
            }
        } else {
            nas_nl_stats_update_invalid_msg (sock, rt_msg_type);
        }
        return true;
    }

    /*!
     * Range upto GET_ROUTE
     */
    if (rt_msg_type <= RTM_GETROUTE) {
        nas_nl_stats_update_tot_msg (sock, rt_msg_type);
        if (nl_to_route_info(rt_msg_type,hdr, obj)) {
            nas_nl_stats_update_pub_msg (sock, rt_msg_type);
            if (net_publish_event(obj) != cps_api_ret_code_OK) {
                nas_nl_stats_update_pub_msg_failed (sock, rt_msg_type);
            }
        } else {
            nas_nl_stats_update_invalid_msg (sock, rt_msg_type);
        }
        return true;
    }

    /*!
     * Range upto GET_NEIGHBOR
     */
    if (rt_msg_type <= RTM_GETNEIGH) {
        nas_nl_stats_update_tot_msg (sock, rt_msg_type);
        if (nl_to_neigh_info(rt_msg_type, hdr,obj)) {
            cps_api_key_init(cps_api_object_key(obj),cps_api_qualifier_TARGET,
                             cps_api_obj_cat_ROUTE,cps_api_route_obj_NEIBH,0);
            nas_nl_stats_update_pub_msg (sock, rt_msg_type);
            if (net_publish_event(obj) != cps_api_ret_code_OK) {
                nas_nl_stats_update_pub_msg_failed (sock, rt_msg_type);
            }
        } else {
            nas_nl_stats_update_invalid_msg (sock, rt_msg_type);
        }
        return true;
    }
    /*!
     * Range upto GET_NETCONF
     */
    if (rt_msg_type <= RTM_GETNETCONF) {
        nas_nl_stats_update_tot_msg (sock, rt_msg_type);
        if (nl_get_ip_netconf_info(rt_msg_type,hdr, obj)) {
            nas_nl_stats_update_pub_msg (sock, rt_msg_type);
            if (net_publish_event(obj) != cps_api_ret_code_OK) {
                nas_nl_stats_update_pub_msg_failed (sock, rt_msg_type);
            }
        } else {
            nas_nl_stats_update_invalid_msg (sock, rt_msg_type);
        }
        return true;
    }
    return false;
}

static void publish_existing()
{
    struct ifaddrs *if_addr, *ifa;
    int    family, s;
    char   name[NI_MAXHOST];



    if(getifaddrs(&if_addr) == -1) {
        return;
    }

    for (ifa = if_addr; ifa; ifa = ifa->ifa_next)
    {
        if(ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET || family == AF_INET6)
        {
            KN_DEBUG("%s - family: %d%s, flags 0x%x",
                ifa->ifa_name, family,
                (family == AF_INET)?"(AF_INET)":
                (family == AF_INET6)?"(AF_INET6)":"", ifa->ifa_flags);

            s = getnameinfo(ifa->ifa_addr,
                            (family == AF_INET)? sizeof(struct sockaddr_in):
                                                 sizeof(struct sockaddr_in6),
                            name, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

            if (!s)
                KN_DEBUG("  Address %s", name);
            else
                KN_DEBUG("  get name failed");

            s = getnameinfo(ifa->ifa_netmask,
                            (family == AF_INET)? sizeof(struct sockaddr_in):
                                                 sizeof(struct sockaddr_in6),
                            name, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (!s)
                KN_DEBUG("  Mask %s strlen %d", name, (int)strlen(name));
            else
                KN_DEBUG("  get name failed");


            cps_api_object_t obj = cps_api_object_create();
            cps_api_object_attr_add(obj,cps_api_if_ADDR_A_NAME,
                    ifa->ifa_name,strlen(ifa->ifa_name)+1);

            if (family == AF_INET) {
                hal_ip_addr_t ip;
                std_ip_from_inet(&ip,&(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr));
                cps_api_object_attr_add(obj,cps_api_if_ADDR_A_IF_ADDR,
                                    &ip,sizeof(ip));

                std_ip_from_inet(&ip,&(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr));
                cps_api_object_attr_add(obj,cps_api_if_ADDR_A_IF_MASK,
                                    &ip,sizeof(ip));
            }
            else {
                hal_ip_addr_t ip;
                std_ip_from_inet6(&ip,&(((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr));
                cps_api_object_attr_add(obj,cps_api_if_ADDR_A_IF_ADDR,
                                    &ip,sizeof(ip));
                std_ip_from_inet6(&ip,&(((struct sockaddr_in6 *)ifa->ifa_netmask)->sin6_addr));
                cps_api_object_attr_add(obj,cps_api_if_ADDR_A_IF_MASK,
                                    &ip,sizeof(ip));
            }
            cps_api_key_init(cps_api_object_key(obj),cps_api_qualifier_TARGET,
                    cps_api_obj_cat_INTERFACE,cps_api_int_obj_INTERFACE_ADDR,0);
            net_publish_event(obj);

        }
    }

    freeifaddrs(if_addr);
    return;
}

static char   buf[NL_SCRATCH_BUFFER_LEN];

static inline void add_fd_set(int fd, fd_set &fdset, int &max_fd) {
    FD_SET(fd, &fdset);
    if (fd>max_fd) max_fd = fd;
}

struct nl_event_desc {
    fn_nl_msg_handle process;
    bool (*trigger)(int sock, int id);
} ;

static bool trigger_route(int sock, int reqid);
static bool trigger_neighbour(int sock, int reqid);
static bool trigger_netconf(int sock, int reqid);

static auto nlm_handlers = new std::map<nas_nl_sock_TYPES,nl_event_desc >{
    { nas_nl_sock_T_ROUTE , { get_netlink_data, &trigger_route} } ,
    { nas_nl_sock_T_INT ,{ get_netlink_data, nl_interface_get_request} },
    { nas_nl_sock_T_NEI ,{ get_netlink_data,&trigger_neighbour } },
    { nas_nl_sock_T_NETCONF ,{ get_netlink_data, &trigger_netconf } },
};

static bool trigger_route(int sock, int reqid) {
    if (nl_request_existing_routes(sock,AF_INET,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_ROUTE).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }

    if (nl_request_existing_routes(sock,AF_INET6,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_ROUTE).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }
    return true;
}
static bool trigger_neighbour(int sock, int reqid) {
    if (nl_neigh_get_all_request(sock,AF_INET,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_NEI).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }

    if (nl_neigh_get_all_request(sock,AF_INET6,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_NEI).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }
    return true;
}

static bool trigger_netconf(int sock, int reqid) {
    if (nl_netconf_get_all_request(sock,AF_INET,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_NETCONF).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }
    if (nl_netconf_get_all_request(sock,AF_INET6,++reqid)) {
        netlink_tools_process_socket(sock,nlm_handlers->at(nas_nl_sock_T_NETCONF).process,
                NULL,buf,sizeof(buf),&reqid,NULL);
    }

    return true;
}


void os_debug_nl_stats_reset () {
    for ( auto it = nlm_sockets->begin(); it != nlm_sockets->end() ; ++it) {
        nas_nl_stats_reset(it->first);
    }
}

void os_debug_nl_stats_print () {
    printf("\r\n NETLINK STATS INFORMATION scratch buf-size: %d\r\n",
           NL_SCRATCH_BUFFER_LEN);

    for ( auto it = nlm_sockets->begin(); it != nlm_sockets->end() ; ++it) {
        printf("\r\nSocket type: %-10s sock-fd: %-10d socket-rx-buf-size: %-10d\r\n",
               ((it->second == nas_nl_sock_T_ROUTE) ? "Route" :
                (it->second == nas_nl_sock_T_INT) ? "Intf" :
                (it->second == nas_nl_sock_T_NEI) ? "Nbr" : "NetConf"),
               it->first,
               ((it->second == nas_nl_sock_T_ROUTE) ? NL_ROUTE_SOCKET_BUFFER_LEN :
                (it->second == nas_nl_sock_T_INT) ? NL_INTF_SOCKET_BUFFER_LEN :
                (it->second == nas_nl_sock_T_NEI) ? NL_NEIGH_SOCKET_BUFFER_LEN :
                (it->second == nas_nl_sock_T_NETCONF) ? NL_NETCONF_SOCKET_BUFFER_LEN: 0));
        printf("\r=========================================================================\r\n");

        nas_nl_stats_print (it->first);
    }
}

void os_send_refresh(nas_nl_sock_TYPES type) {
    int RANDOM_REQ_ID = 0xee00;

    for ( auto it = nlm_sockets->begin(); it != nlm_sockets->end() ; ++it) {
        if (it->second == type && nlm_handlers->at(it->second).trigger!=NULL) {
            nlm_handlers->at(it->second).trigger(it->first,RANDOM_REQ_ID);
        }
    }
}

int net_main() {
    int max_fd = -1;
    struct sockaddr_nl sa;
    fd_set read_fds, sel_fds;

    FD_ZERO(&read_fds);
    memset(&sa, 0, sizeof(sa));

    size_t ix = nas_nl_sock_T_ROUTE;
    for ( ; ix < (size_t)nas_nl_sock_T_MAX; ++ix ) {
        int sock = nas_nl_sock_create((nas_nl_sock_TYPES)(ix),true);
        if(sock==-1) {
            EV_LOG(ERR,NETLINK,0,"INIT","Failed to initialize sockets...%d",errno);
            exit(-1);
        }
        EV_LOG(INFO,NETLINK,2, "NET-NOTIFY","Socket: ix %d, sock id %d", ix, sock);
        nlm_sockets->insert({sock, (nas_nl_sock_TYPES)(ix)});
        add_fd_set(sock,read_fds,max_fd);
        nas_nl_stats_init (sock);
    }

    //Publish existing..
    publish_existing();

    nas_nl_sock_TYPES _refresh_list[] = {
            nas_nl_sock_T_INT,
            nas_nl_sock_T_NEI,
            nas_nl_sock_T_ROUTE,
            nas_nl_sock_T_NETCONF
    };

    g_if_db = new (std::nothrow) (INTERFACE);
    g_if_bridge_db = new (std::nothrow) (if_bridge);
    g_if_bond_db = new (std::nothrow) (if_bond);

    if(g_if_db == nullptr || g_if_bridge_db == nullptr || g_if_bridge_db == nullptr)
        EV_LOG(ERR,NETLINK,0,"INIT","Allocation failed for class objects...");

    ix = 0;
    size_t refresh_mx = sizeof(_refresh_list)/sizeof(*_refresh_list);
    for ( ; ix < refresh_mx ; ++ix ) {
        os_send_refresh(_refresh_list[ix]);
    }

    while (1) {
        memcpy ((char *) &sel_fds, (char *) &read_fds, sizeof(fd_set));

        if(select((max_fd+1), &sel_fds, NULL, NULL, NULL) <= 0)
            continue;

        for ( auto it = nlm_sockets->begin(); it != nlm_sockets->end() ; ++it) {
            if (FD_ISSET(it->first,&sel_fds)) {
                netlink_tools_receive_event(it->first,nlm_handlers->at(it->second).process,
                            NULL,buf,sizeof(buf),NULL);
            }
        }
    }

    /* deinit the netlink stats on exit */
    for ( auto it = nlm_sockets->begin(); it != nlm_sockets->end() ; ++it) {
        nas_nl_stats_deinit (it->first);
    }

    return 0;
}

t_std_error cps_api_net_notify_init(void) {

    EV_LOG_TRACE(ev_log_t_NULL, 3, "NET-NOTIFY","Initializing Net Notify Thread");

    if (cps_api_event_client_connect(&_handle)!=STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_NULL, 3, "NET-NOTIFY","Failed to initialize");
        return STD_ERR(INTERFACE,FAIL,0);
    }

    std_thread_init_struct(&_net_main_thr);
    _net_main_thr.name = "db-api-linux-events";
    _net_main_thr.thread_function = (std_thread_function_t)net_main;
    t_std_error rc = std_thread_create(&_net_main_thr);
    if (rc!=STD_ERR_OK) {
        EV_LOG(ERR,INTERFACE,3,"db-api-linux-event-init-fail","Failed to "
                "initialize event service due");
    }
    cps_api_operation_handle_t handle;

    if (cps_api_operation_subsystem_init(&handle,1)!=STD_ERR_OK) {
        return STD_ERR(INTERFACE,FAIL,0);
    }

    if (os_interface_object_reg(handle)!=STD_ERR_OK) {
        return STD_ERR(INTERFACE,FAIL,0);
    }

    return rc;
}

/*
 * Enable NFLOG for AF_BRIDGE
 * For bridging NFLOG is used to copy certain packet types (like ARP)
 * user socket for injecting to ingress pipeline.
 */
bool os_nflog_enable ()
{
    std::stringstream str_stream;

    str_stream << "/proc/sys/net/netfilter/nf_log/7";
    std::string path = str_stream.str();
    std::ofstream nflog_conf (path.c_str());
    if(!nflog_conf.good()) {
        EV_LOGGING(NAS_OS,ERR,"NAS-UPD-NFLOG", "NFLOG file :%s enable failed!!!", path.c_str());
        return false;
    }

    nflog_conf << "nfnetlink_log";
    EV_LOGGING(NAS_OS,INFO,"NAS-UPD-NFLOG", "NFLOG file :%s enabled", path.c_str());
    nflog_conf.close();
    return true;
}

}
