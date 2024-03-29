/*
** Copyright Glaber 2018-2023
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "glb_state.h"
#include "glb_state_hosts.h"
#include "zbxalgo.h"

extern int CONFIG_UNREACHABLE_DELAY;
extern int CONFIG_UNREACHABLE_RETRIES;
extern int CONFIG_UNREACHABLE_TIMEOUT;

typedef struct {
    elems_hash_t *hosts;
    elems_hash_t *ip_to_host_index;
    mem_funcs_t memf;
    strpool_t strpool;
} conf_t;

static conf_t *conf;

typedef enum {
    IFACE_TYPE_UNKNOWN = 0,
    IFACE_TYPE_ID, 
    IFACE_TYPE_NAME
} iface_type_t;

typedef union {
   u_int64_t id;
   char description[32];
} interface_t;

typedef struct {
    iface_type_t type;
    char name[16];
    interface_t iface;
    const char *error;
    int lastchange;
    int lastupdate;
    int first_fail;
    int disabled_till;
    unsigned char avail_state;
    unsigned char fail_count;
} host_iface_t;

GLB_VECTOR_DECL(host_ifaces, host_iface_t);
GLB_VECTOR_IMPL(host_ifaces, host_iface_t);

typedef struct {
    int last_heartbeat;
    int heartbeat_frequency;
    int last_avail_state_change;
    unsigned char host_avail_state;

    glb_vector_host_ifaces_t interfaces;
} host_state_t;

ELEMS_CREATE(host_create_cb) {
    elem->data = memf->malloc_func(NULL, sizeof(host_state_t));
    
    host_state_t* host_state = elem->data;
    bzero(host_state, sizeof(host_state_t));
    DEBUG_HOST(elem->id, "Host is created in the state");
    glb_vector_host_ifaces_create(&host_state->interfaces, &conf->memf);

}

void interface_clear_cb(void *iface_in, mem_funcs_t *memf){
    host_iface_t *iface = iface_in;

    strpool_free(&conf->strpool, iface->error);
};

ELEMS_FREE(host_free_cb) {
    host_state_t* host_state = elem->data;
    
    glb_vector_host_ifaces_destroy(&host_state->interfaces, &conf->memf, interface_clear_cb);
   
    memf->free_func(host_state);
    elem->data = NULL;
    DEBUG_HOST(elem->id,"Host state has been deleted");
}

ELEMS_CREATE(ip_to_host_create_cb) {
    elem->data = 0;
}

ELEMS_FREE(ip_to_host_free_cb) {
    strpool_free(&conf->strpool, (char *)elem->id);
}

int glb_state_hosts_init(mem_funcs_t *memf)
{
    if (NULL == (conf = memf->malloc_func(NULL, sizeof(conf_t)))) {
        LOG_WRN("Cannot allocate memory for cache struct");
        exit(-1);
    };
    
    conf->hosts = elems_hash_init(memf, host_create_cb, host_free_cb );
    conf->ip_to_host_index = elems_hash_init(memf, ip_to_host_create_cb, ip_to_host_free_cb );
    conf->memf = *memf;
    strpool_init(&conf->strpool,memf);
    
    return SUCCEED;
}

int glb_state_hosts_destroy() {
    elems_hash_destroy(conf->hosts);
    elems_hash_destroy(conf->ip_to_host_index);
    strpool_destroy(&conf->strpool);
    conf->memf.free_func(conf);
}

ELEMS_CALLBACK(process_heartbeat_cb) {
    host_state_t *host = elem->data;
    
    host->last_heartbeat = time(NULL);
    host->heartbeat_frequency = *(int*)data;
    DEBUG_HOST(elem->id, "Processed host heartbeat, frequency is %d", host->heartbeat_frequency);
    
    if (0 != host->heartbeat_frequency &&  HOST_AVAIL_STATE_DOWN == host->host_avail_state) {
        host->last_avail_state_change = time(NULL);
        host->host_avail_state = HOST_AVAIL_STATE_ALIVE;
        DEBUG_HOST(elem->id, "Arrived heartbeat, setting host avail state to ALIVE");
    }
    
    return SUCCEED;
}

void reset_iface_cb (void *element, void* data) {
    host_iface_t *iface = element;

    iface->disabled_till = 0;
    iface->avail_state = INTERFACE_AVAILABLE_UNKNOWN;
    iface->error = strpool_replace(&conf->strpool, iface->error, "Interface avail state was reset after host update");
    iface->fail_count = 0;
    iface->lastupdate = 0; //time(NULL);
}

ELEMS_CALLBACK(reset_host_cb) {
    host_state_t *host = elem->data;
    host->last_heartbeat = 0;
    host->heartbeat_frequency = 0;
    host->host_avail_state = HOST_AVAIL_STATE_UNKNOWN;
    host->last_avail_state_change = time(NULL);
       
    glb_vector_host_ifaces_iterate(&host->interfaces, reset_iface_cb, NULL);
    DEBUG_HOST(elem->id, "Reset host interfaces, total %d ifaces", host->interfaces.values_num);
}

void glb_state_host_process_heartbeat(u_int64_t hostid, int freq) {
    elems_hash_process(conf->hosts, hostid, process_heartbeat_cb, &freq, 0);
}

void glb_state_host_reset_heartbeat(u_int64_t hostid) {
    glb_state_host_process_heartbeat(hostid, 0);
}

void glb_state_hosts_reset(u_int64_t hostid) {
    int freq = 0;
    elems_hash_process(conf->hosts, hostid, reset_host_cb, &freq, 0);
}

ELEMS_CALLBACK(get_avail_state_cb) {
    host_state_t *host = elem->data;
    DEBUG_HOST(elem->id, "Requested host avail_state, state is %d", host->host_avail_state);
    return host->host_avail_state;
}

int glb_state_hosts_get_alive_status(u_int64_t hostid) {
    return elems_hash_process(conf->hosts, hostid, get_avail_state_cb, NULL, ELEM_FLAG_DO_NOT_CREATE);
}

void glb_state_hosts_delete(u_int64_t hostid) {
    elems_hash_delete(conf->hosts, hostid);
}

typedef struct {
    u_int64_t ifnterfaceid;
    int avail_state;
    const char *error;
    const char *ifname;
} if_info_t;


int interface_compare_func(const void *d1, const void *d2) {
    const host_iface_t *iface1 = d1, *iface2 = d2;
    
    if (iface1->type != iface2->type)
        return FAIL;

    switch (iface1->type) {
        case IFACE_TYPE_ID:
            if (iface1->iface.id == iface2->iface.id) 
                return SUCCEED;
            return FAIL;
        case IFACE_TYPE_NAME:
            return strcmp(iface1->name, iface2->name);
        default: 
            HALT_HERE("Unknown interface type %d, this is programming bug", iface1->type);
    }
}

#define MAX_IFACE_TIMEOUT_COUNT 3

ELEMS_CALLBACK(set_interface_cb) {
    host_state_t *host = elem->data;
    host_iface_t  *set_iface = data, *interface = NULL;
    int idx, new = 0;
    int now = time(NULL);

    DEBUG_HOST(elem->id, "Updating host's inteface");
    if (FAIL == (idx = glb_vector_host_ifaces_search(&host->interfaces, *set_iface, interface_compare_func ))) {
     
        if (host->interfaces.values_alloc == host->interfaces.values_num)
            glb_vector_host_ifaces_reserve(&host->interfaces, host->interfaces.values_num + 2, &conf->memf);
 
        idx = glb_vector_host_ifaces_append(&host->interfaces, *set_iface, &conf->memf);
        new = 1;
        DEBUG_HOST(elem->id, "Added interface id %"PRIu64", name %s", set_iface->iface.id, set_iface->name);
    }
    
    interface = glb_vector_host_ifaces_get_element(&host->interfaces, idx);

    if (new) {
        interface->error = strpool_add(&conf->strpool, set_iface->error);
        interface->lastchange = now;
        interface->avail_state = INTERFACE_AVAILABLE_UNKNOWN;
    }
    else{ 
        if (NULL == interface->error || NULL == set_iface->error || 0 == strcmp(interface->error,set_iface->error))
            interface->error = strpool_replace(&conf->strpool, interface->error, set_iface->error);
    }

    if (set_iface->lastupdate != 0) 
        interface->lastupdate = set_iface->lastupdate;

    if (set_iface->lastchange != 0)
        interface->lastchange = set_iface->lastchange;

    if (set_iface->avail_state != interface->avail_state) {
        DEBUG_HOST(elem->id, "Interface changed state %d -> %d", interface->avail_state, set_iface->avail_state );

        if ( INTERFACE_AVAILABLE_FALSE == set_iface->avail_state) {

            if (0 == interface->first_fail)
                interface->first_fail == time(NULL);
            
            interface->fail_count++;
            
            if (set_iface->fail_count > 0)
               interface->fail_count = set_iface->fail_count;

            if (interface->fail_count < CONFIG_UNREACHABLE_RETRIES || 
                now - interface->first_fail < CONFIG_UNREACHABLE_TIMEOUT ) {
                return SUCCEED;
            }
        } 

        interface->fail_count = 0;
        interface->first_fail = 0;
        interface->disabled_till = 0;
        interface->avail_state = set_iface->avail_state;
        interface->lastchange = now;

        DEBUG_HOST(elem->id, "Interface  state is %d, lastchange is %d", interface->avail_state, interface->lastchange );
    }

    return SUCCEED;
}
     

static void set_iface_state(u_int64_t hostid,u_int64_t ifaceid, const char *ifname, int state, const char *error) {
    host_iface_t if_info = {.avail_state = state, .error = error, .fail_count = 0,
                            .lastchange = 0, .lastupdate = time(NULL) };
    if (0 == ifaceid) {
        if_info.type = IFACE_TYPE_NAME;
   
        if (NULL == ifname || 0 == ifname[0] )
        return;
        
        zbx_strlcpy(if_info.name, ifname, MIN(strlen(ifname), 15) + 1);
    } else {
        if_info.type = IFACE_TYPE_ID;
        if_info.iface.id = ifaceid;
    }

    elems_hash_process(conf->hosts, hostid, set_interface_cb, &if_info, 0);
}

void glb_state_host_iface_register_response_arrive(u_int64_t hostid,u_int64_t ifaceid, const char *ifname) {
    DEBUG_HOST(hostid, "Set host interface AVAIL due to response arrive ,id %"PRIu64", ifname %s", ifaceid, ifname);
    set_iface_state(hostid, ifaceid, ifname, INTERFACE_AVAILABLE_TRUE, NULL);
}

void glb_state_host_iface_register_timeout(u_int64_t hostid,u_int64_t ifaceid, const char *ifname, const char *error) {
    DEBUG_HOST(hostid, "Register interface timeout, id %"PRIu64", ifname %s", ifaceid, ifname);
    set_iface_state(hostid, ifaceid, ifname, INTERFACE_AVAILABLE_FALSE, error);
}

void glb_state_host_iface_create(u_int64_t hostid,u_int64_t ifaceid, const char *ifname) {
    DEBUG_HOST(hostid, "Created interface, id %"PRIu64", ifname %s", ifaceid, ifname);
    set_iface_state(hostid, ifaceid, ifname, INTERFACE_AVAILABLE_UNKNOWN, NULL);
}

void glb_state_host_iface_register_passive_arrive(u_int64_t hostid, const char *ifname) {
    DEBUG_HOST(hostid, "Registering data arrive on passive iface %s", ifname);
    set_iface_state(hostid, 0, ifname, INTERFACE_AVAILABLE_TRUE, NULL);
}


ELEMS_CALLBACK(update_host_avail_state_cb) {
    host_state_t *host = elem->data;
    int now = *(int *)data;

    unsigned char old_state = host->host_avail_state;
    
    if (0 == host->heartbeat_frequency) {
        host->host_avail_state = HOST_AVAIL_STATE_UNKNOWN;
        return SUCCEED;
    }
    
    if ((host->heartbeat_frequency + host->last_heartbeat < now) && 
                    HOST_AVAIL_STATE_DOWN != host->host_avail_state ) {
        DEBUG_HOST(elem->id, "Heartbeat timer expired, setting host avail to DONW");
        host->host_avail_state = HOST_AVAIL_STATE_DOWN;
        host->last_avail_state_change = now;
    }
}

void update_hosts_avail_state_cb(void) {

    int current_time = time(NULL);
    elems_hash_iterate(conf->hosts, update_host_avail_state_cb, &current_time , 0);

}

ELEMS_CALLBACK(is_interface_pollable_cb) {
    host_state_t *host = elem->data;
    host_iface_t  *set_iface = data, *interface = NULL;
    int idx;

    if (FAIL == (idx = glb_vector_host_ifaces_search(&host->interfaces, *set_iface, interface_compare_func)))
        return SUCCEED;
  
    interface = glb_vector_host_ifaces_get_element(&host->interfaces, idx);

    if (INTERFACE_AVAILABLE_FALSE != interface->avail_state) {
        DEBUG_HOST(elem->id, "Requested host iface id %"PRIu64", name %s avail status, status is AVAILABLE", 
                        set_iface->iface.id, set_iface->name);
        return SUCCEED;
    }
    
    if ( interface->disabled_till > time(NULL)) {
        set_iface->disabled_till = interface->disabled_till;
        DEBUG_HOST(elem->id, "Requested host iface avail status iface id %"PRIu64", name %s, status is UNAVAILABLE, disabled for %ld more seconds", 
                                    set_iface->iface.id, set_iface->name, set_iface->disabled_till - time(NULL));
        return FAIL;
    }

    //interface is AVAIL_FALSE, but disable time expired, allow to poll once and set disable till again
    DEBUG_HOST(elem->id, "Requested host iface id %"PRIu64", name %s, avail status, status is UNAVAILABLE, allowing single poll and marking disabled for %d more seconds", 
                                   set_iface->iface.id, set_iface->name, CONFIG_UNREACHABLE_DELAY);

    interface->disabled_till = time(NULL) + CONFIG_UNREACHABLE_DELAY;
    
    return SUCCEED;
}

static int  is_interface_pollable(u_int64_t hostid, host_iface_t *if_info, int *disabled_till) {
    
    if_info->disabled_till = -1;

    if (SUCCEED == elems_hash_process(conf->hosts, hostid, is_interface_pollable_cb, if_info, ELEM_FLAG_DO_NOT_CREATE)) 
        return SUCCEED;
    
    if (-1 == if_info->disabled_till) 
        return SUCCEED;

    if (NULL != disabled_till)
        *disabled_till = if_info->disabled_till;

    return FAIL;

}

int glb_state_host_is_interface_pollable(u_int64_t hostid, u_int64_t ifaceid, const char *ifname, int *disabled_till) {
    host_iface_t if_info = {0};

    if (0 == ifaceid) {
        if ( NULL == ifname) //this is possible for templated items, it's ok to skip them
            return INTERFACE_AVAILABLE_UNKNOWN;

        if_info.type = IFACE_TYPE_NAME;
        zbx_strlcpy(if_info.name, ifname, strlen(ifname) + 1);
    } else {
        if_info.type = IFACE_TYPE_ID;
        if_info.iface.id = ifaceid;
    }

    return is_interface_pollable(hostid, &if_info, disabled_till);
}

ELEMS_CALLBACK(get_interface_avail_cb) {
    host_state_t *host = elem->data;
    host_iface_t  *set_iface = data, *interface = NULL;
    int idx;

    if (FAIL == (idx = glb_vector_host_ifaces_search(&host->interfaces, *set_iface, interface_compare_func)))
        return SUCCEED;

    interface = glb_vector_host_ifaces_get_element(&host->interfaces, idx);

    if (INTERFACE_AVAILABLE_FALSE != interface->avail_state)
        return SUCCEED;
    
    set_iface->disabled_till = interface->disabled_till;
    
    return FAIL;
}

static int  get_interface_avail(u_int64_t hostid, host_iface_t *if_info, int *disabled_till) {
    
    if_info->disabled_till = -1;

    if (SUCCEED == elems_hash_process(conf->hosts, hostid, get_interface_avail_cb, if_info, ELEM_FLAG_DO_NOT_CREATE)) 
        return SUCCEED;
    
    if (-1 == if_info->disabled_till) 
        return SUCCEED;
    
    if (NULL != disabled_till)
        *disabled_till = if_info->disabled_till;

    return FAIL;
}

int glb_state_host_iface_get_avail(u_int64_t hostid, u_int64_t interfaceid, const char *ifname, int *disabled_till) {
    
    host_iface_t if_info;

    if ( 0 == interfaceid ) {
      if ( NULL == ifname) //this is possible for templated items, it's ok to skip them
            return INTERFACE_AVAILABLE_UNKNOWN;

        if_info.type = IFACE_TYPE_NAME;
        zbx_strlcpy(if_info.name, ifname, strlen(ifname) + 1);
    } else {
        if_info.type = IFACE_TYPE_ID;
        if_info.iface.id = interfaceid;
    }

    return get_interface_avail(hostid, &if_info, disabled_till);
}

typedef struct {
    struct zbx_json *j;
    u_int64_t hostid;
    int change_timestamp;
} json_gen_t;


void generate_iface_state_json_cb(void *if_info, void *data) {
    host_iface_t *iface = if_info;
    json_gen_t *req = data;
    struct zbx_json *j = req->j;

    DEBUG_HOST(req->hostid, "Processing iface %s %"PRIu64, iface->name, iface->iface.id);
    if (req->change_timestamp > 0 && req->change_timestamp > iface->lastchange) {
        DEBUG_HOST(req->hostid, "Iface %s %"PRIu64" is not changed ( req ts: %d, if lastchange %d), not adding to state data", 
            iface->name, iface->iface.id, req->change_timestamp, iface->lastchange);
        return;
    }
    
    zbx_json_addobject(j, NULL);
    zbx_json_adduint64(j,"hostid", req->hostid);

    if (IFACE_TYPE_ID == iface->type )
    zbx_json_adduint64(j, ZBX_PROTO_TAG_INTERFACE_ID, iface->iface.id);
    
    zbx_json_addstring(j, "interface_name", iface->name, ZBX_JSON_TYPE_STRING);

	zbx_json_adduint64(j, ZBX_PROTO_TAG_AVAILABLE, iface->avail_state);
    zbx_json_adduint64(j, "lastchange", iface->lastchange);
    zbx_json_adduint64(j, "lastupdate", iface->lastupdate);
 	zbx_json_addstring(j, ZBX_PROTO_TAG_ERROR, iface->error, ZBX_JSON_TYPE_STRING);

	zbx_json_close(j);


}

ELEMS_CALLBACK(get_ifaces_avail_state_json_cb) {
    host_state_t *host = elem->data;
    json_gen_t *req = data;
    
    req->hostid = elem->id;

    glb_vector_host_ifaces_iterate(&host->interfaces, generate_iface_state_json_cb, req);
     return SUCCEED;
}

int glb_state_host_get_interfaces_avail_json(u_int64_t hostid, struct zbx_json *j) {
    json_gen_t req = {.j = j, .change_timestamp = 0 };
    return elems_hash_process(conf->hosts, hostid, get_ifaces_avail_state_json_cb, &req, ELEM_FLAG_DO_NOT_CREATE);
}
    
int glb_state_hosts_get_interfaces_avail_json(zbx_vector_uint64_t *hostids, struct zbx_json *j) {
    int i;
    for (i = 0; i< hostids->values_num; i++) {
     
        int res = glb_state_host_get_interfaces_avail_json(hostids->values[i], j);
    }
}

int glb_state_hosts_get_changed_ifaces_json(int timestamp, struct zbx_json *j) {
    json_gen_t req = {.j = j, .change_timestamp = timestamp};
    elems_hash_iterate(conf->hosts, get_ifaces_avail_state_json_cb, &req, 0);
}

int DC_config_get_hostid_by_interfaceid(u_int64_t interfaceid, u_int64_t *hostid);

int parse_interface_record(const char *p) {
    struct zbx_json_parse	jp;
    host_iface_t iface = {0};
    u_int64_t interfaceid, hostid;
    zbx_json_type_t type;
    int tmp;
    char buffer[MAX_STRING_LEN];

    if (SUCCEED != zbx_json_brackets_open(p, &jp))
        return FAIL;
    
    if (FAIL == zbx_json_value_by_name(&jp, "interface_name", iface.name, 16, &type))
        return FAIL;    
                                                        
    if (SUCCEED == glb_json_get_uint64_value_by_name(&jp, "interfaceid", &iface.iface.id)) {
        if (FAIL ==  glb_json_get_uint64_value_by_name(&jp, "hostid", &hostid) ||
            FAIL == DC_config_get_hostid_by_interfaceid(iface.iface.id, &hostid))
            return FAIL;
        iface.type = IFACE_TYPE_ID;
    } else {
        if(FAIL ==  glb_json_get_uint64_value_by_name(&jp, "hostid", &hostid) )
            return FAIL;
        iface.type = IFACE_TYPE_NAME;
    }

    if (FAIL == glb_json_get_int_value_by_name(&jp, "available", &tmp))
        return FAIL;
    
    iface.avail_state = tmp;
    
    glb_json_get_int_value_by_name(&jp, "lastupdate", &iface.lastupdate);
    glb_json_get_int_value_by_name(&jp, "lastchange", &iface.lastchange);

    if (SUCCEED == zbx_json_value_by_name(&jp, "error", buffer, MAX_STRING_LEN, &type))
        iface.error = buffer;
    
    iface.fail_count = MAX_IFACE_TIMEOUT_COUNT + 1;
    DEBUG_HOST(hostid, "Arrived host info via json, setting interface: %s", jp.start);
    return elems_hash_process(conf->hosts, hostid, set_interface_cb, &iface, 0);
}

//acccept array of states and updates/creates the intrefaces
int glb_state_host_set_interfaces_from_json(struct zbx_json_parse *jp) {
    const char *p = NULL;

    while (NULL != (p = zbx_json_next(jp, p))) 
        parse_interface_record(p);

    return SUCCEED;
}

typedef struct {
    int iface_type;
    const char *ifname;
    int result;
} get_if_info_t;

int DC_config_get_type_by_interfaceid(u_int64_t interfaceid, unsigned char *type);

void check_iface_matches_by_type_or_name_cb(void *ifdata, void *data) {
    host_iface_t *iface = ifdata;
    get_if_info_t *ifinfo = data;
    
    unsigned char iftype;
    
    switch (iface->type)
    {
        case IFACE_TYPE_ID:
            if (SUCCEED == DC_config_get_type_by_interfaceid(iface->iface.id, &iftype) &&
                                iftype == ifinfo->iface_type ){
                ifinfo->result = iface->avail_state;
                return;
            }
            break;

        case IFACE_TYPE_NAME:
            if (0 == strcmp(ifinfo->ifname, iface->name)) {
                ifinfo->result = iface->avail_state;
                return;
            }
    }
}

ELEMS_CALLBACK(get_iface_avail_by_type_or_name_cb) {
    get_if_info_t *ifinfo = data;
    host_state_t *host = elem->data;

    glb_vector_host_ifaces_iterate(&host->interfaces, check_iface_matches_by_type_or_name_cb, ifinfo); 
}

int glb_state_host_get_interface_avail_by_type(u_int64_t hostid, int iface_type, const char *if_name) {
    get_if_info_t if_info = {.iface_type = iface_type, .ifname = if_name, .result = INTERFACE_AVAILABLE_UNKNOWN };
 
    if (FAIL == elems_hash_process(conf->hosts, hostid, get_iface_avail_by_type_or_name_cb, &if_info, ELEM_FLAG_DO_NOT_CREATE))
        return FAIL;
    
    return if_info.result;
}


void reset_interface_cb(void *if_data, void*data) {
    host_iface_t *iface = if_data;
    
    iface->avail_state = INTERFACE_AVAILABLE_UNKNOWN;
    iface->error = strpool_replace(&conf->strpool, iface->error, "Interface state has been reset");
    iface->lastupdate = iface->lastchange = time(NULL);
}

ELEMS_CALLBACK(host_reset_cb) {
    host_state_t *host = elem->data;
    host->heartbeat_frequency = 0;
    host->last_heartbeat = 0;
    
    glb_vector_host_ifaces_iterate(&host->interfaces, reset_interface_cb, NULL);
}

void glb_state_host_reset(u_int64_t hostid) {
    elems_hash_process(conf->hosts, hostid, host_reset_cb, NULL, ELEM_FLAG_DO_NOT_CREATE);
}

typedef struct {
    const char *ip;
    u_int64_t hostid;
} ip_to_host_info_t;


ELEMS_CALLBACK(find_ip_cb) {
  
    ip_to_host_info_t* ip_to_host = data;
    ip_to_host->hostid = (u_int64_t)elem->data;
    ip_to_host->ip = (char *)elem->id;
  
    return SUCCEED;
}

u_int64_t   glb_state_host_find_by_ip(const char *addr) {
    if (NULL == addr)
        return 0;

    ip_to_host_info_t ip_to_host = {.ip = strpool_add(&conf->strpool, addr), .hostid = 0};
  
    if (SUCCEED == elems_hash_process(conf->ip_to_host_index, (u_int64_t)ip_to_host.ip, find_ip_cb, &ip_to_host, ELEM_FLAG_DO_NOT_CREATE )) {
        strpool_free(&conf->strpool, ip_to_host.ip);
        return ip_to_host.hostid;
    }
    
    strpool_free(&conf->strpool, ip_to_host.ip);
    return 0;
}

void glb_state_hosts_release_ip(const char *addr) {
    const char *id = strpool_add(&conf->strpool, addr);
    elems_hash_delete(conf->ip_to_host_index, (u_int64_t)id );
    strpool_free(&conf->strpool, id);
}

ELEMS_CALLBACK(register_ip_cb) {
    ip_to_host_info_t* ip_to_host = data;
    elem->data = (void *)ip_to_host->hostid;
    strpool_copy((char * )elem->id);
}

int glb_state_host_register_ip(const char *addr, u_int64_t hostid) {
    if (NULL == addr || 0 == hostid)
        return FAIL;

    ip_to_host_info_t ip_to_host = {.ip = strpool_add(&conf->strpool, addr), .hostid = hostid};
  
    elems_hash_process(conf->ip_to_host_index, (u_int64_t)ip_to_host.ip, register_ip_cb, &ip_to_host, 0);
    strpool_free(&conf->strpool, ip_to_host.ip);
  
    return SUCCEED;
}

ELEMS_CALLBACK(get_host_active_status_json_cb) 
{
    json_gen_t *req = data;
    host_state_t *host = elem->data;
       
    if (req->change_timestamp > host->last_avail_state_change)
        return SUCCEED;

	zbx_json_addobject(req->j, NULL);
	zbx_json_adduint64(req->j, ZBX_PROTO_TAG_HOSTID, elem->id);
    zbx_json_addint64(req->j, "heartbeat_frequency", host->heartbeat_frequency);
    zbx_json_addint64(req->j, "last_heartbeat", host->last_heartbeat);
    zbx_json_addint64(req->j, "last_avail_state_change", host->last_avail_state_change);
    zbx_json_addint64(req->j, ZBX_PROTO_TAG_ACTIVE_STATUS, host->host_avail_state);
    zbx_json_close(req->j);

    return SUCCEED;
}

void glb_state_hosts_get_changed_avail_states_json(int timestamp, struct zbx_json *j) {
    json_gen_t req = {.j = j, .change_timestamp = timestamp, .hostid = 0};
       
    elems_hash_iterate(conf->hosts, get_host_active_status_json_cb, &req, 0);
}

ELEMS_CALLBACK(set_host_avail_state_cb) {
    host_state_t *host = elem->data;
    host->host_avail_state = *(unsigned char*) data;
}

void parse_host_avail_record(const char *p) {
  
    u_int64_t hostid, avail_state;
    struct zbx_json_parse jp;

    if (SUCCEED == zbx_json_brackets_open(p, &jp) &&
        SUCCEED == glb_json_get_uint64_value_by_name(&jp, "hostid", &hostid) &&
        SUCCEED == glb_json_get_uint64_value_by_name(&jp, "active_status", &avail_state)) {
            unsigned char state= avail_state;
            elems_hash_process(conf->hosts, hostid, set_host_avail_state_cb, &state, 0);
        }
}

int glb_state_hosts_set_avail_states_from_json(struct zbx_json_parse *jp) {
    const char *p = NULL;

    while (NULL != (p = zbx_json_next(jp, p))) 
        parse_host_avail_record(p);

    return SUCCEED;
}   