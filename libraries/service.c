/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * service.c
 * - service related routines
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/syslog.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <netinet/in.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPrivate.h>

#include "globals.h"
#include "interfaces.h"
#include "config_method.h"
#include "ip6config_utils.h"


__private_extern__ CFDictionaryRef
my_SCDynamicStoreCopyValue(SCDynamicStoreRef session, CFStringRef key)
{
    CFDictionaryRef 	dict;

    dict = SCDynamicStoreCopyValue(session, key);
    if (dict) {
        if (isA_CFDictionary(dict) == NULL) {
            my_CFRelease(&dict);
        }
    }
    return (dict);
}

/*
static boolean_t
host_route(int cmd, struct in6_addr * i6addr)
{
    int		len;
    boolean_t	ret = TRUE;
    int 	rtm_seq = 0;
    int 	sockfd = -1;
    struct {
	struct rt_msghdr	hdr;
	struct sockaddr_in6	dst;
	struct sockaddr_in6	gway;
    } rtmsg;

    if ((sockfd = socket(PF_ROUTE, SOCK_RAW, AF_INET6)) < 0) {
	my_log(LOG_INFO, "host_route: open routing socket failed, %s",
		    strerror(errno));
	ret = FALSE;
	goto done;
    }

    memset(&rtmsg, 0, sizeof(rtmsg));
    rtmsg.hdr.rtm_type = cmd;
    rtmsg.hdr.rtm_flags = RTF_UP | RTF_STATIC | RTF_HOST;
    rtmsg.hdr.rtm_version = RTM_VERSION;
    rtmsg.hdr.rtm_seq = ++rtm_seq;
    rtmsg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY;
    rtmsg.dst.sin6_len = sizeof(rtmsg.dst);
    rtmsg.dst.sin6_family = AF_INET6;
    memcpy(&rtmsg.dst.sin6_addr, i6addr, sizeof(struct in6_addr));
    rtmsg.gway.sin6_len = sizeof(rtmsg.gway);
    rtmsg.gway.sin6_family = AF_INET6;
    memcpy(&rtmsg.gway.sin6_addr, &in6addr_loopback, sizeof(struct in6_addr));

    len = sizeof(rtmsg);
    rtmsg.hdr.rtm_msglen = len;
    if (write(sockfd, &rtmsg, len) < 0) {
		my_log(LOG_DEBUG, "host_route: write routing socket failed, %s",
			   strerror(errno));
		ret = FALSE;
    }
 done:
    close(sockfd);
    return (ret);
}
*/

__private_extern__ int
service_set_address(Service_t * service_p,
		    struct in6_addr * addr,
		    int prefixLen)
{
    interface_t *	if_p = service_interface(service_p);
    int			ret = 0;
    struct in6_addr	prefixmask;
    struct in6_addr	netaddr;
    int 		s = inet6_dgram_socket();
    char		str[64];
    
    memset(&prefixmask, 0, sizeof(struct in6_addr));
    prefixLen2mask(&prefixmask, prefixLen);
    memset(&netaddr, 0, sizeof(struct in6_addr));
    network_addr(addr, &prefixmask, &netaddr);

    inet_ntop(AF_INET6, (const void *)addr, str, sizeof(str));
    
    if (G_verbose) {
        char	prefx[64], net[64];
        inet_ntop(AF_INET6, (const void *)&prefixmask, prefx, sizeof(prefx));
        inet_ntop(AF_INET6, (const void *)&netaddr, net, sizeof(net));
        my_log(LOG_DEBUG,
                "service_set_address(%s): %s prefixmask: %s netaddr: %s",
                if_name(if_p), str, prefx, net);
    }

    if (s < 0) {
	ret = errno; /* ??? */
	my_log(LOG_ERR,
		"service_set_address(%s): socket() failed, %s (%d)",
		if_name(if_p), strerror(errno), errno);
    }
    else {
	inet6_addrinfo_t * info_p = &service_p->info;

	if (inet6_aifaddr(s, if_name(if_p), addr, NULL, &prefixmask) < 0) {
            ret = errno;
            my_log(LOG_DEBUG,
		    "service_set_address(%s): %s inet_aifaddr() failed, %s (%d)",
		    if_name(if_p), str, strerror(errno), errno);
	}
        if_setflags(if_p, if_flags(if_p) | IFF_UP);
        ifflags_set(s, if_name(if_p), IFF_UP);
        bzero(info_p, sizeof(*info_p));
        memcpy(&info_p->addr, addr, sizeof(struct in6_addr));
        memcpy(&info_p->prefixmask, &prefixmask, sizeof(struct in6_addr));
        memcpy(&info_p->netaddr, &netaddr, sizeof(struct in6_addr));
        info_p->prefixlen = prefixLen;
        close(s);
/*
        **** JV ****
        (void)host_route(RTM_DELETE, addr);
        (void)host_route(RTM_ADD, addr);
*/
    }

    return (ret);
}

__private_extern__ int
service_remove_address(Service_t * service_p)
{
    interface_t *	if_p = service_interface(service_p);
    inet6_addrinfo_t *	info_p = &service_p->info;
    int			ret = 0;

    if (!IN6_IS_ADDR_UNSPECIFIED(&info_p->addr)) {
        inet6_addrinfo_t	saved_info;

        /* copy IP info then clear it so that it won't be elected */
        saved_info = service_p->info;
        bzero(info_p, sizeof(*info_p));

        /* if no service on this interface refers to this IP, remove the IP */
        if (IFState_service_with_ip(service_ifstate(service_p),
                                    &saved_info.addr) == NULL) {
            int s;

            /*
            * JV: Is this comment still valid?
            * This can only happen if there's a manual service
            * and an rtadv service with the same IP.  Duplicate
            * manual services are prevented when created.
            */
            s = inet6_dgram_socket();
            if (s < 0) {
                ret = errno;
                my_log(LOG_DEBUG,
                        "service_remove_address(%s) socket() failed, %s (%d)",
                        if_name(if_p), strerror(errno), errno);
            }
            else {
                my_log(LOG_DEBUG, "service_remove_address(%s): " IP6_FORMAT,
                        if_name(if_p), IP6_LIST(&saved_info.addr));

                if (inet6_difaddr(s, if_name(if_p), &saved_info.addr) < 0) {
                    ret = errno;
                    my_log(LOG_DEBUG, "service_remove_address(%s) "
                            IP6_FORMAT " failed, %s (%d)", if_name(if_p),
                            IP6_LIST(&saved_info.addr), strerror(errno), errno);
                }
                close(s);
            }
        }
        /* if no service refers to this IP, remove the host route for the IP */
/*
        if (IFStateList_service_with_ip(&G_ifstate_list,
                                        &saved_info.addr, NULL) == NULL) {
            (void)host_route(RTM_DELETE, &saved_info.addr);
        }
*/
    }

    return (ret);
}

__private_extern__ void
service_publish_clear(Service_t * service_p)
{
    if (G_scd_session != NULL && service_p->serviceID) {
	CFMutableArrayRef	array;
	CFStringRef		key;

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (array == NULL) {
	    return;
	}

	/* ipv6 */
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							  kSCDynamicStoreDomainState,
							  service_p->serviceID,
							  kSCEntNetIPv6);
	if (key) {
	    CFArrayAppendValue(array, key);
	}
	my_CFRelease(&key);

	SCDynamicStoreSetMultiple(G_scd_session, NULL, array, NULL);
	my_CFRelease(&array);
    }
    return;
}

__private_extern__ void
service_publish_failure(Service_t * service_p, ip6config_status_t status,
			char * msg)
{
    service_publish_clear(service_p);
    my_log(LOG_DEBUG, "%s %s: status = '%s'",
            ip6config_method_string(service_p->method),
            if_name(service_interface(service_p)),
            ip6config_status_string(status));
    return;
}

static boolean_t
cache_key_different(SCDynamicStoreRef session, CFStringRef key,
		    CFDictionaryRef value)
{
    CFDictionaryRef	cache_value;
    boolean_t		ret = TRUE;

    cache_value = my_SCDynamicStoreCopyValue(session, key);
    if (cache_value) {
        if (CFEqual(value, cache_value)) {
            ret = FALSE;
        }
        my_CFRelease(&cache_value);
    }
    return (ret);
}

static __inline__ void
update_key(SCDynamicStoreRef session,
	   CFStringRef key, CFDictionaryRef dict,
	   CFMutableDictionaryRef keys_to_set,
	   CFMutableArrayRef keys_to_remove)
{
    if (dict) {
        if (cache_key_different(session, key, dict)) {
            CFDictionarySetValue(keys_to_set, key, dict);
        }
    }
    else {
        CFArrayAppendValue(keys_to_remove, key);
    }
    return;
}

static void
publish_keys(CFStringRef ipv6_key, CFDictionaryRef ipv6_dict)
{
    CFMutableDictionaryRef	keys_to_set = NULL;
    CFMutableArrayRef		keys_to_remove = NULL;

    if (ipv6_dict) {
        SCLog(G_verbose, LOG_INFO, CFSTR("%@ = %@"), ipv6_key, ipv6_dict);
    }

    keys_to_set = CFDictionaryCreateMutable(NULL, 0,
					    &kCFTypeDictionaryKeyCallBacks,
					    &kCFTypeDictionaryValueCallBacks);
    keys_to_remove = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

    if (keys_to_set == NULL || keys_to_remove == NULL) {
        goto done;
    }
    
    update_key(G_scd_session, ipv6_key, ipv6_dict, keys_to_set, keys_to_remove);
    
    if (CFArrayGetCount(keys_to_remove) > 0
            || CFDictionaryGetCount(keys_to_set) > 0) {
        SCDynamicStoreSetMultiple(G_scd_session, keys_to_set,
                                keys_to_remove, NULL);
    }
 done:
    my_CFRelease(&keys_to_remove);
    my_CFRelease(&keys_to_set);
    return;
}

static void
publish_service(CFStringRef serviceID, CFDictionaryRef ipv6_dict)
{
    CFStringRef	ipv6_key = NULL;

    /* create the cache keys */
    ipv6_key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							   kSCDynamicStoreDomainState,
							   serviceID,
							   kSCEntNetIPv6);

    if (ipv6_key == NULL) {
        goto done;
    }

    publish_keys(ipv6_key, ipv6_dict);

 done:
    my_CFRelease(&ipv6_key);
    return;
}

__private_extern__ void
service_publish_success(Service_t * service_p)
{
    CFMutableArrayRef		array = NULL;
    inet6_addrinfo_t *		info_p;
    CFMutableDictionaryRef	ipv6_dict = NULL;
    CFStringRef			str;
    CFNumberRef			prefixlen, flags;

    if (service_p->serviceID == NULL) {
        return;
    }

    info_p = &service_p->info;

    if (G_scd_session == NULL) {
        /* configd is not running */
        return;
    }

    ipv6_dict = CFDictionaryCreateMutable(NULL, 0,
					  &kCFTypeDictionaryKeyCallBacks,
					  &kCFTypeDictionaryValueCallBacks);

    /* set the ip6 address array */
    array = CFArrayCreateMutable(NULL, 1,  &kCFTypeArrayCallBacks);

    str = CFStringCreateWithFormat(NULL, NULL, CFSTR(IP6_FORMAT), IP6_LIST(&info_p->addr));

    if (array && str) {
        CFArrayAppendValue(array, str);
        CFDictionarySetValue(ipv6_dict, kSCPropNetIPv6Addresses, array);
    }
    my_CFRelease(&str);
    my_CFRelease(&array);

    /* set the ip6 prefixlen array */
    array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    prefixlen = CFNumberCreate(NULL, kCFNumberIntType, &info_p->prefixlen);
    if (array && prefixlen) {
        CFArrayAppendValue(array, prefixlen);
        CFDictionarySetValue(ipv6_dict, kSCPropNetIPv6PrefixLength, array);
    }
    my_CFRelease(&prefixlen);
    my_CFRelease(&array);

    /* set the ip6 flags array */
    array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    flags = CFNumberCreate(NULL, kCFNumberIntType, &info_p->addr_flags);
    if (array && flags) {
        CFArrayAppendValue(array, flags);
        CFDictionarySetValue(ipv6_dict, kSCPropNetIPv6Flags, array);
    }
    my_CFRelease(&flags);
    my_CFRelease(&array);
    
    /* set the router */
    if (!IN6_IS_ADDR_UNSPECIFIED(&info_p->router)) {
        str = CFStringCreateWithFormat(NULL, NULL, CFSTR(IP6_FORMAT), IP6_LIST(&info_p->router));
        if (str) {
            CFDictionarySetValue(ipv6_dict, kSCPropNetIPv6Router, str);
        }
        my_CFRelease(&str);
    }

    CFDictionarySetValue(ipv6_dict, CFSTR("InterfaceName"), service_ifstate(service_p)->ifname);

    publish_service(service_p->serviceID, ipv6_dict);

    my_CFRelease(&ipv6_dict);

    return;
}

__private_extern__ ip6config_status_t
service_set_service(IFState_t * ifstate, ServiceConfig_t * config)
{
    CFStringRef		serviceID = config->serviceID;
    Service_t *		service_p;
    IFState_t *		this_ifstate = NULL;
    
    service_p = IFState_service_with_ID(ifstate, serviceID);
    if (service_p) {
        boolean_t		needs_stop = FALSE;
        ip6config_status_t	status;
        
        if (service_p->method == config->method) {
            status = config_method_change(service_p, config->method, 
                                        config->method_data,
                                        &needs_stop);
            if (status == ip6config_status_success_e && needs_stop == FALSE) {
                return (ip6config_status_success_e);
            }
        }
        IFState_service_free(ifstate, serviceID);
    }
    else {
        this_ifstate = IFStateList_service_with_ID(&G_ifstate_list, 
                                                serviceID,
                                                &service_p);
        if (this_ifstate) {
            /* service is on other interface, stop it now */
            IFState_service_free(this_ifstate, serviceID);
        }
    }
    return (IFState_service_add(ifstate, serviceID, config->method,
                                config->method_data));
}

static ServiceConfig_t *
service_config_list_lookup_service(ServiceConfig_t * config_list, int count,
				  CFStringRef serviceID)
{
    ServiceConfig_t *	config;
    int 		i;

    for (config = config_list, i = 0; i < count; i++, config++) {
        if (CFEqual(serviceID, config->serviceID)) {
            return (config);
        }
    }
    return (NULL);
}

__private_extern__ void
service_free_inactive_services(char * ifname, ServiceConfig_t * config_list, int count)
{
    int			j;
    IFState_t *		ifstate;
    CFMutableArrayRef	list = NULL;
    int			service_count;

    ifstate = IFStateList_ifstate_with_name(&G_ifstate_list, ifname, NULL);
    if (ifstate == NULL) {
        goto done;
    }

    list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (list == NULL) {
        goto done;
    }

    service_count = dynarray_count(&ifstate->services);
    for (j = 0; j < service_count; j++) {
        Service_t * service_p = dynarray_element(&ifstate->services, j);
        CFStringRef serviceID = service_p->serviceID;

        if (service_config_list_lookup_service(config_list, count,
                serviceID) == NULL) {
            CFArrayAppendValue(list, serviceID);
        }
    }

    service_count = CFArrayGetCount(list);
    for (j = 0; j < service_count; j++) {
        CFStringRef serviceID = CFArrayGetValueAtIndex(list, j);

        IFState_service_free(ifstate, serviceID);
    }

    done:
        my_CFRelease(&list);
    return;
}

static CFArrayRef
interface_services_copy(CFArrayRef all, CFStringRef ifn_cf)
{
    int 		i, n;
    CFMutableArrayRef	list = NULL;

    if (all == NULL) {
        return (NULL);
    }

    list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (list == NULL) {
        return (NULL);
    }

    n = CFArrayGetCount(all);
    for (i = 0; i < n; i++) {
        CFDictionaryRef	item = CFArrayGetValueAtIndex(all, i);
        CFStringRef	name;

        name = CFDictionaryGetValue(item, kSCPropNetInterfaceDeviceName);
        if (CFEqual(name, ifn_cf)) {
            CFArrayAppendValue(list, item);
        }
    }

    if (CFArrayGetCount(list) == 0) {
        my_CFRelease(&list);
    }

    return (list);
}

__private_extern__ void
service_config_list_free(ServiceConfig_t * * list_p_p, int count)
{
    int 		i;
    ServiceConfig_t * 	list_p = *list_p_p;

    for (i = 0; i < count; i++) {
        if (list_p[i].serviceID)
            my_CFRelease(&list_p[i].serviceID);
        if (list_p[i].method_data) {
            if (list_p[i].method_data->stf_data.ip4_addrs_list)
                free(list_p[i].method_data->stf_data.ip4_addrs_list);
            if (list_p[i].method_data->stf_data.relay_address.addr_type == relay_address_type_dns_e
                && list_p[i].method_data->stf_data.relay_address.relay_address_u.dns_relay_addr)
                free(list_p[i].method_data->stf_data.relay_address.relay_address_u.dns_relay_addr);
            free(list_p[i].method_data);
        }
    }
    free(list_p);
    *list_p_p = NULL;
    return;
}

__private_extern__ ServiceConfig_t *
service_config_list_lookup_method(ServiceConfig_t * config_list, int count,
	ip6config_method_t method,
	ip6config_method_data_t * method_data)
{
    ServiceConfig_t *	config;
    int 		i;

    switch (method) {
	case ip6config_method_automatic_e:
	case ip6config_method_rtadv_e:
        case ip6config_method_6to4_e: {
	    for (config = config_list, i = 0; i < count; i++, config++) {
                if (ip6config_method_is_dynamic(config->method))
                    return (config);
            }
	    break;
	}
	case ip6config_method_manual_e: {
	    for (config = config_list, i = 0; i < count; i++, config++) {
                if (ip6config_method_is_manual(config->method)
                    && IN6_ARE_ADDR_EQUAL(&method_data->ip6[0].addr,
                        &config->method_data->ip6[0].addr)) {
                    return (config);
                }
            }
	    break;
	}
	default: {
	    break;
	}
    }
    return (NULL);
}

__private_extern__ ServiceConfig_t *
service_config_list_init(SCDynamicStoreRef session, 
                        CFArrayRef all_ipv6, 
                        char * ifname, int * count_p)
{
    ServiceConfig_t *	config_list = NULL;
    int			count = 0;
    int			i;
    CFArrayRef 		if_service_list;
    int			if_service_count;
    CFStringRef		ifn_cf = NULL;

    ifn_cf = CFStringCreateWithCString(NULL, ifname,
		kCFStringEncodingMacRoman);
    if (ifn_cf == NULL) {
        goto done;
    }
    
    if_service_list = interface_services_copy(all_ipv6, ifn_cf);
    if (if_service_list == NULL) {
        goto done;
    }

    if_service_count = CFArrayGetCount(if_service_list);
    config_list = (ServiceConfig_t *) calloc(if_service_count, sizeof(*config_list));
    if (config_list == NULL) {
        goto done;
    }

    for (i = 0; i < if_service_count; i++) {
        CFDictionaryRef			ipv6_dict;
        ip6config_method_t		method;
        ip6config_method_data_t	*	method_data;
        CFStringRef			serviceID;

        ipv6_dict = CFArrayGetValueAtIndex(if_service_list, i);
        serviceID = CFDictionaryGetValue(ipv6_dict, PROP_SERVICEID);
        method_data = ip6config_method_data_from_dict(ipv6_dict, &method);
        if (method_data == NULL) {
            continue;
        }
        
        if (method == ip6config_method_6to4_e) {
            if (ip6config_get_6to4_address_data(session, method_data) != 0) {
                /* this is OK, continue setting up service */
                my_log(LOG_DEBUG, 
                        "service_config_list_init: ip6config_get_6to4_address_data returned error");
            }
        }

        /* check if it's already in the list */
        if (service_config_list_lookup_method(config_list, count, method, method_data)) {
            boolean_t	is_manual = ip6config_method_is_manual(method);

            if (is_manual) {
                my_log(LOG_INFO, "%s: %s " IP6_FORMAT " duplicate service",
                        ifname, ip6config_method_string(method),
                        IP6_LIST(&method_data->ip6[0].addr));
            }
            else {
                my_log(LOG_INFO, "%s: %s ignored",
                        ifname, ip6config_method_string(method));
            }
            free(method_data);
            continue;
        }
        /* add the new entry */
        config_list[count].serviceID = CFRetain(serviceID);
        config_list[count].method = method;
        config_list[count].method_data = method_data;
        count++;
    }
 done:
    if (config_list && count == 0) {
        service_config_list_free(&config_list, count);
    }
    my_CFRelease(&ifn_cf);
    my_CFRelease(&if_service_list);
    *count_p = count;
    return (config_list);
}

__private_extern__ void
Service_free(void * arg)
{
    Service_t *	service_p = (Service_t *)arg;

    SCLog(G_verbose, LOG_INFO, CFSTR("Service_free(%@)"),
	  service_p->serviceID);

    config_method_stop(service_p);
    service_publish_clear(service_p);
    if (service_p->user_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), service_p->user_rls,
                            kCFRunLoopDefaultMode);
        my_CFRelease(&service_p->user_rls);
    }
    if (service_p->user_notification != NULL) {
        CFUserNotificationCancel(service_p->user_notification);
        my_CFRelease(&service_p->user_notification);
    }
    my_CFRelease(&service_p->serviceID);
    free(service_p);
    return;
}

__private_extern__ Service_t *
Service_init(IFState_t * ifstate, CFStringRef serviceID,
	     ip6config_method_t method,
	     void * method_data,
	     ip6config_status_t * status_p)
{
    Service_t *		service_p = NULL;
    ip6config_status_t	status = ip6config_status_success_e;

    service_p = (Service_t *)malloc(sizeof(*service_p));
    if (service_p == NULL) {
        status = ip6config_status_allocation_failed_e;
        goto failed;
    }

    bzero(service_p, sizeof(*service_p));
    service_p->method = method;
    service_p->ifstate = ifstate;

    if (serviceID) {
        service_p->serviceID = (void *)CFRetain(serviceID);
    }
    else {
        service_p->serviceID = (void *)CFStringCreateWithFormat(NULL, NULL,
                                                            CFSTR("%s-%s"),
                                                            ip6config_method_string(method),
                                                            if_name(service_interface(service_p)));
    }

    status = config_method_start(service_p, method, method_data);
    if (status != ip6config_status_success_e) {
        goto failed;
    }

    *status_p = status;
    return (service_p);

 failed:
    if (service_p) {
        my_CFRelease(&service_p->serviceID);
        free(service_p);
    }
    *status_p = status;
    return (NULL);
}

/*
 * Functions used to notify user when there are conflicts in configuration.
 */

#ifndef NO_CFUserNotification
static void
user_confirm(CFUserNotificationRef userNotification, CFOptionFlags responseFlags)
{
    int i;
    int ifstate_count;

    /* clean-up the notification */
    ifstate_count = dynarray_count(&G_ifstate_list);
    for (i = 0; i < ifstate_count; i++) {
        IFState_t *	ifstate = dynarray_element(&G_ifstate_list, i);
        int		j;
        int		service_count;

        service_count = dynarray_count(&ifstate->services);
        for (j = 0; j < service_count; j++) {
            Service_t * service_p = dynarray_element(&ifstate->services, j);
            if (service_p->user_notification == userNotification) {
                CFRunLoopRemoveSource(CFRunLoopGetCurrent(),
                                    service_p->user_rls, kCFRunLoopDefaultMode);
                my_CFRelease(&service_p->user_rls);
                my_CFRelease(&service_p->user_notification);
                return;
            }
        }
    }
    return;
}

static void
service_notify_user(Service_t * service_p, CFTypeRef alert_string)
{
    CFMutableDictionaryRef	dict = NULL;
    SInt32			error = 0;
    CFUserNotificationRef 	notify = NULL;
    CFRunLoopSourceRef		rls = NULL;
    CFURLRef			url = NULL;

    dict = CFDictionaryCreateMutable(NULL, 0,
				     &kCFTypeDictionaryKeyCallBacks,
				     &kCFTypeDictionaryValueCallBacks);
    if (dict == NULL) {
        goto done;
    }

    url = CFBundleCopyBundleURL(G_bundle);
    if (url == NULL) {
        goto done;
    }

    CFDictionarySetValue(dict, kCFUserNotificationAlertHeaderKey,
			 CFSTR("IP6 Configuration"));
    CFDictionarySetValue(dict, kCFUserNotificationAlertMessageKey,
			 alert_string);
    CFDictionarySetValue(dict, kCFUserNotificationLocalizationURLKey,
			 url);

    if (service_p->user_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), service_p->user_rls,
                            kCFRunLoopDefaultMode);
        my_CFRelease(&service_p->user_rls);
    }
    if (service_p->user_notification != NULL) {
        CFUserNotificationCancel(service_p->user_notification);
        my_CFRelease(&service_p->user_notification);
    }
    notify = CFUserNotificationCreate(NULL, 0, 0, &error, dict);
    if (notify == NULL) {
        my_log(LOG_ERR, "CFUserNotificationCreate() failed, %d",
                error);
        goto done;
    }
    rls = CFUserNotificationCreateRunLoopSource(NULL, notify,
                                                user_confirm, 0);
    if (rls == NULL) {
        my_log(LOG_ERR, "CFUserNotificationCreateRunLoopSource() failed");
        my_CFRelease(&notify);
    }
    else {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), rls,
                        kCFRunLoopDefaultMode);
        service_p->user_rls = rls;
        service_p->user_notification = notify;
    }
 done:
    my_CFRelease(&dict);
    my_CFRelease(&url);
    return;
}

#else

static void
service_notify_user(Service_t * service_p, CFTypeRef alertMessage)
{
}

#endif /* NO_CFUserNotification */

__private_extern__ void
service_report_conflict(Service_t * service_p, struct in6_addr * ip6)
{
    CFMutableArrayRef	array;
    CFStringRef         str;

    array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (array == NULL) {
        goto done;
    }

    /* add "The IPv6 address " */
    CFArrayAppendValue(array, CFSTR("The IPv6 address "));

    /* add conflicting IP6 address */
    str = CFStringCreateWithFormat(NULL, NULL, CFSTR(IP6_FORMAT), IP6_LIST(ip6));
    if (str == NULL) {
        goto done;
    }
    CFArrayAppendValue(array, str);
    CFRelease(str);

    /* add " is in use by another computer" */
    CFArrayAppendValue(array, CFSTR(" is in use by another computer"));

    /* post notification */
    service_notify_user(service_p, array);

 done:
    my_CFRelease(&array);
    return;
}

