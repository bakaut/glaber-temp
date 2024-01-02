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

#include "zbxcommon.h"
#include "zbxalgo.h"
#include "../zbxcacheconfig/dbsync.h"
#include "../zbxcacheconfig/dbconfig.h"
#include "../../zabbix_server/glb_poller/poller_ipc.h"

//typedef struct {
//    u_int64_t orig_itemid;
//    u_int64_t conf_itemid;
//} ref_item_t;


typedef struct
{
	zbx_uint64_t		itemid;
	zbx_uint64_t		hostid;

	zbx_uint64_t		interfaceid;
	zbx_uint64_t		valuemapid;

	u_int16_t 		name;
	u_int16_t  		description;
	u_int16_t		key;
	
    u_int16_t		delay;
	u_int16_t		delay_ex;
	//u_int16_t		history_period;

	zbx_uint64_t		revision;

	unsigned char		poll_type;
    void                *poll_type_data;

	unsigned char		value_type;
    //u_int16_t           trend_period;
    unsigned char       has_trends_or_history;
	
	unsigned char		flags;
	unsigned char		admin_status;

	unsigned char		update_triggers;
	zbx_uint64_t		templateid;
	
	u_int16_t 			params;
	//future improvement: this should be "inventory name"
	//however it's quite pointless to do item->inventory link 
	//it's much more feasible to make cutstom attributes for the the host that
	//might refer to the lastvalues or event functions 
	unsigned char		inventory_link;

	//related objects
    //it's OK to convert this to just a text (json?)
	ZBX_DC_PREPROCITEM	*preproc_item;



    //it's feasible to keep dependancy relation out of the item_config object

	//ZBX_DC_MASTERITEM	*master_item; //not sure if needed at all, the reason we want to keep master->dep list
                                      //is to have it in the preprocessing


	ZBX_DC_TRIGGER		**triggers; //this is realation again?
	zbx_vector_ptr_t	tags;
	u_int64_t			master_itemid;
	
    //theese are related to queue mgmt, not the item config
	//probably, should be removed to queues and left in the dbcache
	int			data_expected_from;
	int			queue_next_check;
	unsigned char		location;
	unsigned char		queue_priority;
	unsigned char		poller_type;

    const char *serial_buffer;
}
conf_item_t;

typedef struct {
    zbx_hashset_t ref_items;
    elems_hash_t items;
    mem_funcs_t *memf;
} config_t;

static config_t *conf = {0};

void agent_clean_func(void *data) {

} 

void snmp_clean_func(void *data) {

}

void clean_item(conf_item_t *item) {
    switch (item->poll_type) {
        case ITEM_TYPE_AGENT:
            agent_clean_func(item->poll_type_data);
            break;
        case ITEM_TYPE_SNMP:
            snmp_clean_func(item->poll_type_data);
            break;
    }
}

ELEMS_CREATE(item_create_cb) {
    elem->data = memf->malloc_func(NULL, sizeof(conf_item_t));
    conf_item_t *item = elem->data;

    bzero(elem->data, sizeof(conf_item_t));
    item->poll_type = ITEM_POLL_TYPE_NONE;
}

ELEMS_FREE(item_free_cb) {
    conf_item_t *item = elem->data;
    clean_item(item);
}

void conf_items_init(mem_funcs_t *memf) {
    
    conf = memf->malloc_func(NULL, sizeof(config_t));
    conf->memf = memf;

    zbx_hashset_create_ext(&conf->ref_items, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC, NULL, 
                memf->malloc_func, memf->realloc_func, memf->free_func);

    elems_hash_init(memf, item_create_cb, item_free_cb);

}


ELEMS_CALLBACK(item_update_cb) {

}

void glb_conf_items_sybc(zbx_dbsync_t *sync, zbx_uint64_t revision, int flags, zbx_synced_new_config_t synced,
						 zbx_vector_uint64_t *deleted_itemids)
{
	char **row;
	zbx_uint64_t rowid;
	unsigned char tag;

	ZBX_DC_HOST *host;

	ZBX_DC_ITEM *item;
	ZBX_DC_NUMITEM *numitem;
	ZBX_DC_SNMPITEM *snmpitem;
	ZBX_DC_IPMIITEM *ipmiitem;
	ZBX_DC_TRAPITEM *trapitem;
	ZBX_DC_DEPENDENTITEM *depitem;
	ZBX_DC_LOGITEM *logitem;
	ZBX_DC_DBITEM *dbitem;
	ZBX_DC_SSHITEM *sshitem;
	ZBX_DC_TELNETITEM *telnetitem;
	ZBX_DC_SIMPLEITEM *simpleitem;
	ZBX_DC_JMXITEM *jmxitem;
	ZBX_DC_CALCITEM *calcitem;
	ZBX_DC_INTERFACE_ITEM *interface_snmpitem;
	ZBX_DC_HTTPITEM *httpitem;
	ZBX_DC_SCRIPTITEM *scriptitem;
	ZBX_DC_ITEM_HK *item_hk, item_hk_local;
	ZBX_DC_INTERFACE *interface;

	time_t now;
	unsigned char status, type, value_type, old_poller_type;
	int found, update_index, ret, i;
	zbx_uint64_t itemid, hostid, interfaceid;
	zbx_vector_ptr_t dep_items;

	zbx_vector_ptr_create(&dep_items);
	poller_item_notify_init();
	
	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	now = time(NULL);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(itemid, row[0]);
        elems_hash_process(&conf->items, itemid, item_update_cb, row, 0);
    }
    
}

//     	ZBX_STR2UINT64(hostid, row[1]);
// 		ZBX_STR2UCHAR(status, row[2]);
// 		ZBX_STR2UCHAR(type, row[3]);

// 		if (NULL == (host = (ZBX_DC_HOST *)zbx_hashset_search(&config->hosts, &hostid)))
// 			continue;

// 		item = (ZBX_DC_ITEM *)DCfind_id(&config->items, itemid, sizeof(ZBX_DC_ITEM), &found);

// 		/* template item */
// 		ZBX_DBROW2UINT64(item->templateid, row[48]);

// 		if (0 != found && ITEM_TYPE_TRAP_REGEXP == item->type)
// 			dc_interface_snmpitems_remove(item);

// 		/* see whether we should and can update items_hk index at this point */

// 		update_index = 0;

// 		if (0 == found || item->hostid != hostid || 0 != strcmp(item->key, row[5]))
// 		{
// 			if (1 == found)
// 			{
// 				item_hk_local.hostid = item->hostid;
// 				item_hk_local.key = item->key;

// 				if (NULL == (item_hk = (ZBX_DC_ITEM_HK *)zbx_hashset_search(&config->items_hk,
// 																			&item_hk_local)))
// 				{
// 					/* item keys should be unique for items within a host, otherwise items with  */
// 					/* same key share index and removal of last added item already cleared index */
// 					THIS_SHOULD_NEVER_HAPPEN;
// 				}
// 				else if (item == item_hk->item_ptr)
// 				{
// 					dc_strpool_release(item_hk->key);
// 					zbx_hashset_remove_direct(&config->items_hk, item_hk);
// 				}
// 			}

// 			item_hk_local.hostid = hostid;
// 			item_hk_local.key = row[5];
// 			item_hk = (ZBX_DC_ITEM_HK *)zbx_hashset_search(&config->items_hk, &item_hk_local);

// 			if (NULL != item_hk)
// 				item_hk->item_ptr = item;
// 			else
// 				update_index = 1;
// 		}

// 		/* store new information in item structure */

// 		item->hostid = hostid;
// 		item->flags = (unsigned char)atoi(row[18]);
// 		ZBX_DBROW2UINT64(interfaceid, row[19]);

// 		dc_strpool_replace(found, &item->history_period, row[22]);
// 		dc_strpool_replace(found, &item->name, row[27]);
// 		dc_strpool_replace(found, &item->description, row[49]);

// 		ZBX_STR2UCHAR(item->inventory_link, row[24]);
// 		ZBX_DBROW2UINT64(item->valuemapid, row[25]);

// 		if (0 != (ZBX_FLAG_DISCOVERY_RULE & item->flags))
// 			value_type = ITEM_VALUE_TYPE_TEXT;
// 		else
// 			ZBX_STR2UCHAR(value_type, row[4]);

// 		if (SUCCEED == dc_strpool_replace(found, &item->key, row[5]))
// 			flags |= ZBX_ITEM_KEY_CHANGED;

// 		if (0 == found)
// 		{
// 			item->triggers = NULL;
// 			item->update_triggers = 0;
// 			item->data_expected_from = now;
// 			item->location = ZBX_LOC_NOWHERE;
// 			item->poller_type = ZBX_NO_POLLER;
// 			item->queue_priority = ZBX_QUEUE_PRIORITY_NORMAL;
// 			item->delay_ex = NULL;

// 			if (ZBX_SYNCED_NEW_CONFIG_YES == synced && 0 == host->proxy_hostid)
// 				flags |= ZBX_ITEM_NEW;

// 			zbx_vector_ptr_create_ext(&item->tags, __config_shmem_malloc_func, __config_shmem_realloc_func,
// 									  __config_shmem_free_func);

// 			zbx_vector_dc_item_ptr_append(&host->items, item);

// 			item->preproc_item = NULL;
// 			item->master_item = NULL;
// 		}
// 		else
// 		{
// 			if (item->type != type)
// 				flags |= ZBX_ITEM_TYPE_CHANGED;

// 			if (ITEM_ADMIN_STATUS_ENABLED == status && ITEM_ADMIN_STATUS_ENABLED != item->status)
// 				item->data_expected_from = now;

// 			if (ITEM_ADMIN_STATUS_ENABLED == item->status)
// 			{
// 				ZBX_DC_INTERFACE *interface_old;

// 				interface_old = (ZBX_DC_INTERFACE *)zbx_hashset_search(&config->interfaces,
// 																	   &item->interfaceid);
// 				dc_interface_update_agent_stats(interface_old, item->type, -1);
// 			}
// 		}

// 		item->revision = revision;
// 		dc_host_update_revision(host, revision);

// 		if (ITEM_ADMIN_STATUS_ENABLED == status)
// 		{
// 			interface = (ZBX_DC_INTERFACE *)zbx_hashset_search(&config->interfaces, &interfaceid);
// 			dc_interface_update_agent_stats(interface, type, 1);
// 		}

// 		item->type = type;
// 		item->status = status;
// 		item->value_type = value_type;
// 		item->interfaceid = interfaceid;

// 		/* update items_hk index using new data, if not done already */

// 		if (1 == update_index)
// 		{
// 			item_hk_local.hostid = item->hostid;
// 			item_hk_local.key = dc_strpool_acquire(item->key);
// 			item_hk_local.item_ptr = item;
// 			zbx_hashset_insert(&config->items_hk, &item_hk_local, sizeof(ZBX_DC_ITEM_HK));
// 		}

// 		/* process item intervals and update item nextcheck */

// 		if (SUCCEED == dc_strpool_replace(found, &item->delay, row[8]))
// 		{
// 			flags |= ZBX_ITEM_DELAY_CHANGED;

// 			/* reset expanded delay if raw value was changed */
// 			if (NULL != item->delay_ex)
// 			{
// 				dc_strpool_release(item->delay_ex);
// 				item->delay_ex = NULL;
// 			}
// 		}

// 		/* numeric items */

// 		if (ITEM_VALUE_TYPE_FLOAT == item->value_type || ITEM_VALUE_TYPE_UINT64 == item->value_type)
// 		{
// 			numitem = (ZBX_DC_NUMITEM *)DCfind_id(&config->numitems, itemid, sizeof(ZBX_DC_NUMITEM),
// 												  &found);

// 			dc_strpool_replace(found, &numitem->trends_period, row[23]);
// 			dc_strpool_replace(found, &numitem->units, row[26]);
// 		}
// 		else if (NULL != (numitem = (ZBX_DC_NUMITEM *)zbx_hashset_search(&config->numitems, &itemid)))
// 		{
// 			/* remove parameters for non-numeric item */

// 			dc_strpool_release(numitem->units);
// 			dc_strpool_release(numitem->trends_period);

// 			zbx_hashset_remove_direct(&config->numitems, numitem);
// 		}

// 		/* SNMP items */
// ergqergqergf
// 		if (ITEM_TYPE_SNMP == item->type)
// 		{
// 			snmpitem = (ZBX_DC_SNMPITEM *)DCfind_id(&config->snmpitems, itemid, sizeof(ZBX_DC_SNMPITEM),
// 													&found);

// 			if (SUCCEED == dc_strpool_replace(found, &snmpitem->snmp_oid, row[6]))
// 			{
// 				if (NULL != strchr(snmpitem->snmp_oid, '{'))
// 					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_MACRO;
// 				else if (NULL != strchr(snmpitem->snmp_oid, '['))
// 					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_DYNAMIC;
// 				else
// 					snmpitem->snmp_oid_type = ZBX_SNMP_OID_TYPE_NORMAL;
// 			}
// 		}
// 		else if (NULL != (snmpitem = (ZBX_DC_SNMPITEM *)zbx_hashset_search(&config->snmpitems, &itemid)))
// 		{
// 			/* remove SNMP parameters for non-SNMP item */

// 			dc_strpool_release(snmpitem->snmp_oid);
// 			zbx_hashset_remove_direct(&config->snmpitems, snmpitem);
// 		}

// 		/* IPMI items */

// 		if (ITEM_TYPE_IPMI == item->type)
// 		{
// 			ipmiitem = (ZBX_DC_IPMIITEM *)DCfind_id(&config->ipmiitems, itemid, sizeof(ZBX_DC_IPMIITEM),
// 													&found);

// 			dc_strpool_replace(found, &ipmiitem->ipmi_sensor, row[7]);
// 		}
// 		else if (NULL != (ipmiitem = (ZBX_DC_IPMIITEM *)zbx_hashset_search(&config->ipmiitems, &itemid)))
// 		{
// 			/* remove IPMI parameters for non-IPMI item */
// 			dc_strpool_release(ipmiitem->ipmi_sensor);
// 			zbx_hashset_remove_direct(&config->ipmiitems, ipmiitem);
// 		}

// 		/* trapper items */

// 		if (ITEM_TYPE_TRAPPER == item->type && '\0' != *row[9])
// 		{
// 			trapitem = (ZBX_DC_TRAPITEM *)DCfind_id(&config->trapitems, itemid, sizeof(ZBX_DC_TRAPITEM),
// 													&found);
// 			dc_strpool_replace(found, &trapitem->trapper_hosts, row[9]);
// 		}
// 		else if (NULL != (trapitem = (ZBX_DC_TRAPITEM *)zbx_hashset_search(&config->trapitems, &itemid)))
// 		{
// 			/* remove trapper_hosts parameter */
// 			dc_strpool_release(trapitem->trapper_hosts);
// 			zbx_hashset_remove_direct(&config->trapitems, trapitem);
// 		}

// 		/* dependent items */

// 		if (ITEM_TYPE_DEPENDENT == item->type && SUCCEED != zbx_db_is_null(row[29]))
// 		{
// 			depitem = (ZBX_DC_DEPENDENTITEM *)DCfind_id(&config->dependentitems, itemid,
// 														sizeof(ZBX_DC_DEPENDENTITEM), &found);

// 			if (1 == found)
// 				depitem->last_master_itemid = depitem->master_itemid;
// 			else
// 				depitem->last_master_itemid = 0;

// 			depitem->flags = item->flags;
// 			ZBX_STR2UINT64(depitem->master_itemid, row[29]);

// 			if (depitem->last_master_itemid != depitem->master_itemid)
// 				zbx_vector_ptr_append(&dep_items, depitem);
// 		}
// 		else if (NULL != (depitem = (ZBX_DC_DEPENDENTITEM *)zbx_hashset_search(&config->dependentitems,
// 																			   &itemid)))
// 		{
// 			dc_masteritem_remove_depitem(depitem->master_itemid, itemid);
// 			zbx_hashset_remove_direct(&config->dependentitems, depitem);
// 		}

// 		/* log items */

// 		if (ITEM_VALUE_TYPE_LOG == item->value_type && '\0' != *row[10])
// 		{
// 			logitem = (ZBX_DC_LOGITEM *)DCfind_id(&config->logitems, itemid, sizeof(ZBX_DC_LOGITEM),
// 												  &found);

// 			dc_strpool_replace(found, &logitem->logtimefmt, row[10]);
// 		}
// 		else if (NULL != (logitem = (ZBX_DC_LOGITEM *)zbx_hashset_search(&config->logitems, &itemid)))
// 		{
// 			/* remove logtimefmt parameter */
// 			dc_strpool_release(logitem->logtimefmt);
// 			zbx_hashset_remove_direct(&config->logitems, logitem);
// 		}

// 		/* db items */

// 		if (ITEM_TYPE_DB_MONITOR == item->type && '\0' != *row[11])
// 		{
// 			dbitem = (ZBX_DC_DBITEM *)DCfind_id(&config->dbitems, itemid, sizeof(ZBX_DC_DBITEM), &found);

// 			dc_strpool_replace(found, &dbitem->params, row[11]);
// 			dc_strpool_replace(found, &dbitem->username, row[14]);
// 			dc_strpool_replace(found, &dbitem->password, row[15]);
// 		}
// 		else if (NULL != (dbitem = (ZBX_DC_DBITEM *)zbx_hashset_search(&config->dbitems, &itemid)))
// 		{
// 			/* remove db item parameters */
// 			dc_strpool_release(dbitem->params);
// 			dc_strpool_release(dbitem->username);
// 			dc_strpool_release(dbitem->password);

// 			zbx_hashset_remove_direct(&config->dbitems, dbitem);
// 		}

// 		/* SSH items */

// 		if (ITEM_TYPE_SSH == item->type)
// 		{
// 			sshitem = (ZBX_DC_SSHITEM *)DCfind_id(&config->sshitems, itemid, sizeof(ZBX_DC_SSHITEM),
// 												  &found);

// 			sshitem->authtype = (unsigned short)atoi(row[13]);
// 			dc_strpool_replace(found, &sshitem->username, row[14]);
// 			dc_strpool_replace(found, &sshitem->password, row[15]);
// 			dc_strpool_replace(found, &sshitem->publickey, row[16]);
// 			dc_strpool_replace(found, &sshitem->privatekey, row[17]);
// 			dc_strpool_replace(found, &sshitem->params, row[11]);
// 		}
// 		else if (NULL != (sshitem = (ZBX_DC_SSHITEM *)zbx_hashset_search(&config->sshitems, &itemid)))
// 		{
// 			/* remove SSH item parameters */

// 			dc_strpool_release(sshitem->username);
// 			dc_strpool_release(sshitem->password);
// 			dc_strpool_release(sshitem->publickey);
// 			dc_strpool_release(sshitem->privatekey);
// 			dc_strpool_release(sshitem->params);

// 			zbx_hashset_remove_direct(&config->sshitems, sshitem);
// 		}

// 		/* TELNET items */

// 		if (ITEM_TYPE_TELNET == item->type)
// 		{
// 			telnetitem = (ZBX_DC_TELNETITEM *)DCfind_id(&config->telnetitems, itemid,
// 														sizeof(ZBX_DC_TELNETITEM), &found);

// 			dc_strpool_replace(found, &telnetitem->username, row[14]);
// 			dc_strpool_replace(found, &telnetitem->password, row[15]);
// 			dc_strpool_replace(found, &telnetitem->params, row[11]);
// 		}
// 		else if (NULL != (telnetitem = (ZBX_DC_TELNETITEM *)zbx_hashset_search(&config->telnetitems, &itemid)))
// 		{
// 			/* remove TELNET item parameters */

// 			dc_strpool_release(telnetitem->username);
// 			dc_strpool_release(telnetitem->password);
// 			dc_strpool_release(telnetitem->params);

// 			zbx_hashset_remove_direct(&config->telnetitems, telnetitem);
// 		}

// 		/* simple items */

// 		if (ITEM_TYPE_SIMPLE == item->type)
// 		{
// 			simpleitem = (ZBX_DC_SIMPLEITEM *)DCfind_id(&config->simpleitems, itemid,
// 														sizeof(ZBX_DC_SIMPLEITEM), &found);

// 			dc_strpool_replace(found, &simpleitem->username, row[14]);
// 			dc_strpool_replace(found, &simpleitem->password, row[15]);
// 		}
// 		else if (NULL != (simpleitem = (ZBX_DC_SIMPLEITEM *)zbx_hashset_search(&config->simpleitems, &itemid)))
// 		{
// 			/* remove simple item parameters */

// 			dc_strpool_release(simpleitem->username);
// 			dc_strpool_release(simpleitem->password);

// 			zbx_hashset_remove_direct(&config->simpleitems, simpleitem);
// 		}

// 		/* JMX items */

// 		if (ITEM_TYPE_JMX == item->type)
// 		{
// 			jmxitem = (ZBX_DC_JMXITEM *)DCfind_id(&config->jmxitems, itemid, sizeof(ZBX_DC_JMXITEM),
// 												  &found);

// 			dc_strpool_replace(found, &jmxitem->username, row[14]);
// 			dc_strpool_replace(found, &jmxitem->password, row[15]);
// 			dc_strpool_replace(found, &jmxitem->jmx_endpoint, row[28]);
// 		}
// 		else if (NULL != (jmxitem = (ZBX_DC_JMXITEM *)zbx_hashset_search(&config->jmxitems, &itemid)))
// 		{
// 			/* remove JMX item parameters */

// 			dc_strpool_release(jmxitem->username);
// 			dc_strpool_release(jmxitem->password);
// 			dc_strpool_release(jmxitem->jmx_endpoint);

// 			zbx_hashset_remove_direct(&config->jmxitems, jmxitem);
// 		}

// 		/* SNMP trap items for current server/proxy */

// 		if (ITEM_TYPE_TRAP_REGEXP == item->type && 0 == host->proxy_hostid)
// 		{
// 			interface_snmpitem = (ZBX_DC_INTERFACE_ITEM *)DCfind_id(&config->interface_snmpitems,
// 																	item->interfaceid, sizeof(ZBX_DC_INTERFACE_ITEM), &found);

// 			if (0 == found)
// 			{
// 				zbx_vector_uint64_create_ext(&interface_snmpitem->itemids,
// 											 __config_shmem_malloc_func,
// 											 __config_shmem_realloc_func,
// 											 __config_shmem_free_func);
// 			}

// 			zbx_vector_uint64_append(&interface_snmpitem->itemids, itemid);
// 		}

// 		/* calculated items */

// 		if (ITEM_TYPE_CALCULATED == item->type)
// 		{
// 			calcitem = (ZBX_DC_CALCITEM *)DCfind_id(&config->calcitems, itemid, sizeof(ZBX_DC_CALCITEM),
// 													&found);

// 			dc_strpool_replace(found, &calcitem->params, row[11]);

// 			if (1 == found && NULL != calcitem->formula_bin)
// 				__config_shmem_free_func((void *)calcitem->formula_bin);

// 			calcitem->formula_bin = config_decode_serialized_expression(row[49]);
// 		}
// 		else if (NULL != (calcitem = (ZBX_DC_CALCITEM *)zbx_hashset_search(&config->calcitems, &itemid)))
// 		{
// 			/* remove calculated item parameters */

// 			if (NULL != calcitem->formula_bin)
// 				__config_shmem_free_func((void *)calcitem->formula_bin);
// 			dc_strpool_release(calcitem->params);
// 			zbx_hashset_remove_direct(&config->calcitems, calcitem);
// 		}

// 		/* HTTP agent items */

// 		if (ITEM_TYPE_HTTPAGENT == item->type)
// 		{
// 			httpitem = (ZBX_DC_HTTPITEM *)DCfind_id(&config->httpitems, itemid, sizeof(ZBX_DC_HTTPITEM),
// 													&found);

// 			dc_strpool_replace(found, &httpitem->timeout, row[30]);
// 			dc_strpool_replace(found, &httpitem->url, row[31]);
// 			dc_strpool_replace(found, &httpitem->query_fields, row[32]);
// 			dc_strpool_replace(found, &httpitem->posts, row[33]);
// 			dc_strpool_replace(found, &httpitem->status_codes, row[34]);
// 			httpitem->follow_redirects = (unsigned char)atoi(row[35]);
// 			httpitem->post_type = (unsigned char)atoi(row[36]);
// 			dc_strpool_replace(found, &httpitem->http_proxy, row[37]);
// 			dc_strpool_replace(found, &httpitem->headers, row[38]);
// 			httpitem->retrieve_mode = (unsigned char)atoi(row[39]);
// 			httpitem->request_method = (unsigned char)atoi(row[40]);
// 			httpitem->output_format = (unsigned char)atoi(row[41]);
// 			dc_strpool_replace(found, &httpitem->ssl_cert_file, row[42]);
// 			dc_strpool_replace(found, &httpitem->ssl_key_file, row[43]);
// 			dc_strpool_replace(found, &httpitem->ssl_key_password, row[44]);
// 			httpitem->verify_peer = (unsigned char)atoi(row[45]);
// 			httpitem->verify_host = (unsigned char)atoi(row[46]);
// 			httpitem->allow_traps = (unsigned char)atoi(row[47]);

// 			httpitem->authtype = (unsigned char)atoi(row[13]);
// 			dc_strpool_replace(found, &httpitem->username, row[14]);
// 			dc_strpool_replace(found, &httpitem->password, row[15]);
// 			dc_strpool_replace(found, &httpitem->trapper_hosts, row[9]);
// 		}
// 		else if (NULL != (httpitem = (ZBX_DC_HTTPITEM *)zbx_hashset_search(&config->httpitems, &itemid)))
// 		{
// 			dc_strpool_release(httpitem->timeout);
// 			dc_strpool_release(httpitem->url);
// 			dc_strpool_release(httpitem->query_fields);
// 			dc_strpool_release(httpitem->posts);
// 			dc_strpool_release(httpitem->status_codes);
// 			dc_strpool_release(httpitem->http_proxy);
// 			dc_strpool_release(httpitem->headers);
// 			dc_strpool_release(httpitem->ssl_cert_file);
// 			dc_strpool_release(httpitem->ssl_key_file);
// 			dc_strpool_release(httpitem->ssl_key_password);
// 			dc_strpool_release(httpitem->username);
// 			dc_strpool_release(httpitem->password);
// 			dc_strpool_release(httpitem->trapper_hosts);

// 			zbx_hashset_remove_direct(&config->httpitems, httpitem);
// 		}

// 		/* Script items */

// 		if (ITEM_TYPE_SCRIPT == item->type)
// 		{
// 			scriptitem = (ZBX_DC_SCRIPTITEM *)DCfind_id(&config->scriptitems, itemid,
// 														sizeof(ZBX_DC_SCRIPTITEM), &found);

// 			dc_strpool_replace(found, &scriptitem->timeout, row[30]);
// 			dc_strpool_replace(found, &scriptitem->script, row[11]);

// 			if (0 == found)
// 			{
// 				zbx_vector_ptr_create_ext(&scriptitem->params, __config_shmem_malloc_func,
// 										  __config_shmem_realloc_func, __config_shmem_free_func);
// 			}
// 		}
// 		else if (NULL != (scriptitem = (ZBX_DC_SCRIPTITEM *)zbx_hashset_search(&config->scriptitems, &itemid)))
// 		{
// 			dc_strpool_release(scriptitem->timeout);
// 			dc_strpool_release(scriptitem->script);

// 			zbx_vector_ptr_destroy(&scriptitem->params);
// 			zbx_hashset_remove_direct(&config->scriptitems, scriptitem);
// 		}

// 		if (ITEM_TYPE_WORKER_SERVER == item->type)
// 		{
// 			dc_strpool_replace(found, &item->params, row[11]);
// 		}

// 		/* it is crucial to update type specific (config->snmpitems, config->ipmiitems, etc.) hashsets before */
// 		/* attempting to requeue an item because type specific properties are used to arrange items in queues */

// 		old_poller_type = item->poller_type;

// 		if (ITEM_ADMIN_STATUS_ENABLED == item->status && HOST_STATUS_MONITORED == host->status)
// 		{
// 			DCitem_poller_type_update(item, host, flags);

// 			if (SUCCEED == zbx_is_counted_in_item_queue(item->type, item->key))
// 			{
// 				char *error = NULL;
// 				int nextcheck;

// 				if (FAIL == (nextcheck = DCitem_nextcheck_update(item, interface, flags, now, &error)))
// 				{
// 					zbx_timespec_t ts = {now, 0};

// 					/* Usual way for an item to become not supported is to receive an error     */
// 					/* instead of value. Item state and error will be updated by history syncer */
// 					/* during history sync following a regular procedure with item update in    */
// 					/* database and config cache, logging etc. There is no need to set          */
// 					/* ITEM_STATE_NOTSUPPORTED here.                                            */

// 					if (0 == host->proxy_hostid)
// 						glb_state_item_set_error(item->itemid, error);
			
// 					zbx_free(error);
// 				} else 
// 					glb_state_item_update_nextcheck(itemid, nextcheck);
// 			}
// 		}
// 		else
// 		{
// 			item->queue_priority = ZBX_QUEUE_PRIORITY_NORMAL;
// 			item->queue_next_check = now;
// 			item->poller_type = ZBX_NO_POLLER;
// 		}

// 		/* items that do not support notify-updates are passed to old queuing */
// 		int snmp_version;

// 		DEBUG_ITEM(item->itemid, "About to be checked how to poll");
// 		if (FAIL == glb_might_be_async_polled(item, host, &snmp_version) ||
// 			FAIL == poller_item_add_notify(type, item->key, itemid, hostid, snmp_version))
// 		{

// 			DEBUG_ITEM(item->itemid, "Cannot be async polled, adding to zbx queue %s", __func__);
// 			DCupdate_item_queue(item, old_poller_type);
// 		}
// 	}

// 	/* update dependent item vectors within master items */

// 	for (i = 0; i < dep_items.values_num; i++)
// 	{
// 		zbx_uint64_pair_t pair;

// 		depitem = (ZBX_DC_DEPENDENTITEM *)dep_items.values[i];
// 		dc_masteritem_remove_depitem(depitem->last_master_itemid, depitem->itemid);

// 		if (NULL == (item = (ZBX_DC_ITEM *)zbx_hashset_search(&config->items, &depitem->master_itemid)))
// 			continue;

// 		pair.first = depitem->itemid;
// 		pair.second = depitem->flags;

// 		if (NULL == item->master_item)
// 		{
// 			item->master_item = (ZBX_DC_MASTERITEM *)__config_shmem_malloc_func(NULL,
// 																				sizeof(ZBX_DC_MASTERITEM));

// 			zbx_vector_uint64_pair_create_ext(&item->master_item->dep_itemids, __config_shmem_malloc_func,
// 											  __config_shmem_realloc_func, __config_shmem_free_func);
// 		}

// 		zbx_vector_uint64_pair_append(&item->master_item->dep_itemids, pair);
// 	}

// 	zbx_vector_ptr_destroy(&dep_items);

// 	if (NULL != deleted_itemids)
// 		zbx_vector_uint64_reserve(deleted_itemids, sync->remove_num);

// 	/* remove deleted items from cache */
// 	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
// 	{
// 		if (NULL != deleted_itemids)
// 			zbx_vector_uint64_append(deleted_itemids, rowid);

// 		if (NULL == (item = (ZBX_DC_ITEM *)zbx_hashset_search(&config->items, &rowid)))
// 			continue;

// 		if (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_search(&config->hosts, &item->hostid)))
// 		{
// 			dc_host_update_revision(host, revision);

// 			if (FAIL != (i = zbx_vector_dc_item_ptr_search(&host->items, item,
// 														   ZBX_DEFAULT_PTR_COMPARE_FUNC)))
// 			{
// 				zbx_vector_dc_item_ptr_remove(&host->items, i);
// 			}
			
// 			int snmp_version;

// 			if (SUCCEED == glb_might_be_async_polled(item, host, &snmp_version))
// 			{
// 				DEBUG_ITEM(item->itemid, "Sending poller notify about item removal");
// 				poller_item_add_notify(item->type, item->key, item->itemid, item->hostid, snmp_version);
// 			}
// 		}

// 		if (ITEM_ADMIN_STATUS_ENABLED == item->status)
// 		{
// 			interface = (ZBX_DC_INTERFACE *)zbx_hashset_search(&config->interfaces, &item->interfaceid);
// 			dc_interface_update_agent_stats(interface, item->type, -1);
// 		}

// 		itemid = item->itemid;

// 		if (ITEM_TYPE_TRAP_REGEXP == item->type)
// 			dc_interface_snmpitems_remove(item);

// 		/* numeric items */

// 		if (ITEM_VALUE_TYPE_FLOAT == item->value_type || ITEM_VALUE_TYPE_UINT64 == item->value_type)
// 		{
// 			numitem = (ZBX_DC_NUMITEM *)zbx_hashset_search(&config->numitems, &itemid);

// 			dc_strpool_release(numitem->units);
// 			dc_strpool_release(numitem->trends_period);

// 			zbx_hashset_remove_direct(&config->numitems, numitem);
// 		}

// 		/* SNMP items */

// 		if (ITEM_TYPE_SNMP == item->type)
// 		{
// 			snmpitem = (ZBX_DC_SNMPITEM *)zbx_hashset_search(&config->snmpitems, &itemid);
// 			dc_strpool_release(snmpitem->snmp_oid);
// 			zbx_hashset_remove_direct(&config->snmpitems, snmpitem);
// 		}

// 		/* IPMI items */

// 		if (ITEM_TYPE_IPMI == item->type)
// 		{
// 			ipmiitem = (ZBX_DC_IPMIITEM *)zbx_hashset_search(&config->ipmiitems, &itemid);
// 			dc_strpool_release(ipmiitem->ipmi_sensor);
// 			zbx_hashset_remove_direct(&config->ipmiitems, ipmiitem);
// 		}

// 		/* trapper items */

// 		if (ITEM_TYPE_TRAPPER == item->type &&
// 			NULL != (trapitem = (ZBX_DC_TRAPITEM *)zbx_hashset_search(&config->trapitems, &itemid)))
// 		{
// 			dc_strpool_release(trapitem->trapper_hosts);
// 			zbx_hashset_remove_direct(&config->trapitems, trapitem);
// 		}

// 		/* dependent items */

// 		if (NULL != (depitem = (ZBX_DC_DEPENDENTITEM *)zbx_hashset_search(&config->dependentitems, &itemid)))
// 		{
// 			dc_masteritem_remove_depitem(depitem->master_itemid, itemid);
// 			zbx_hashset_remove_direct(&config->dependentitems, depitem);
// 		}

// 		/* log items */

// 		if (ITEM_VALUE_TYPE_LOG == item->value_type &&
// 			NULL != (logitem = (ZBX_DC_LOGITEM *)zbx_hashset_search(&config->logitems, &itemid)))
// 		{
// 			dc_strpool_release(logitem->logtimefmt);
// 			zbx_hashset_remove_direct(&config->logitems, logitem);
// 		}

// 		/* db items */

// 		if (ITEM_TYPE_DB_MONITOR == item->type &&
// 			NULL != (dbitem = (ZBX_DC_DBITEM *)zbx_hashset_search(&config->dbitems, &itemid)))
// 		{
// 			dc_strpool_release(dbitem->params);
// 			dc_strpool_release(dbitem->username);
// 			dc_strpool_release(dbitem->password);

// 			zbx_hashset_remove_direct(&config->dbitems, dbitem);
// 		}

// 		/* SSH items */

// 		if (ITEM_TYPE_SSH == item->type)
// 		{
// 			sshitem = (ZBX_DC_SSHITEM *)zbx_hashset_search(&config->sshitems, &itemid);

// 			dc_strpool_release(sshitem->username);
// 			dc_strpool_release(sshitem->password);
// 			dc_strpool_release(sshitem->publickey);
// 			dc_strpool_release(sshitem->privatekey);
// 			dc_strpool_release(sshitem->params);

// 			zbx_hashset_remove_direct(&config->sshitems, sshitem);
// 		}

// 		/* TELNET items */

// 		if (ITEM_TYPE_TELNET == item->type)
// 		{
// 			telnetitem = (ZBX_DC_TELNETITEM *)zbx_hashset_search(&config->telnetitems, &itemid);

// 			dc_strpool_release(telnetitem->username);
// 			dc_strpool_release(telnetitem->password);
// 			dc_strpool_release(telnetitem->params);

// 			zbx_hashset_remove_direct(&config->telnetitems, telnetitem);
// 		}

// 		/* simple items */

// 		if (ITEM_TYPE_SIMPLE == item->type)
// 		{
// 			simpleitem = (ZBX_DC_SIMPLEITEM *)zbx_hashset_search(&config->simpleitems, &itemid);

// 			dc_strpool_release(simpleitem->username);
// 			dc_strpool_release(simpleitem->password);

// 			zbx_hashset_remove_direct(&config->simpleitems, simpleitem);
// 		}

// 		/* JMX items */

// 		if (ITEM_TYPE_JMX == item->type)
// 		{
// 			jmxitem = (ZBX_DC_JMXITEM *)zbx_hashset_search(&config->jmxitems, &itemid);

// 			dc_strpool_release(jmxitem->username);
// 			dc_strpool_release(jmxitem->password);
// 			dc_strpool_release(jmxitem->jmx_endpoint);

// 			zbx_hashset_remove_direct(&config->jmxitems, jmxitem);
// 		}

// 		/* calculated items */

// 		if (ITEM_TYPE_CALCULATED == item->type)
// 		{
// 			calcitem = (ZBX_DC_CALCITEM *)zbx_hashset_search(&config->calcitems, &itemid);
// 			dc_strpool_release(calcitem->params);

// 			if (NULL != calcitem->formula_bin)
// 				__config_shmem_free_func((void *)calcitem->formula_bin);

// 			zbx_hashset_remove_direct(&config->calcitems, calcitem);
// 		}

// 		/* HTTP agent items */

// 		if (ITEM_TYPE_HTTPAGENT == item->type)
// 		{
// 			httpitem = (ZBX_DC_HTTPITEM *)zbx_hashset_search(&config->httpitems, &itemid);

// 			dc_strpool_release(httpitem->timeout);
// 			dc_strpool_release(httpitem->url);
// 			dc_strpool_release(httpitem->query_fields);
// 			dc_strpool_release(httpitem->posts);
// 			dc_strpool_release(httpitem->status_codes);
// 			dc_strpool_release(httpitem->http_proxy);
// 			dc_strpool_release(httpitem->headers);
// 			dc_strpool_release(httpitem->ssl_cert_file);
// 			dc_strpool_release(httpitem->ssl_key_file);
// 			dc_strpool_release(httpitem->ssl_key_password);
// 			dc_strpool_release(httpitem->username);
// 			dc_strpool_release(httpitem->password);
// 			dc_strpool_release(httpitem->trapper_hosts);

// 			zbx_hashset_remove_direct(&config->httpitems, httpitem);
// 		}

// 		/* Script items */

// 		if (ITEM_TYPE_SCRIPT == item->type)
// 		{
// 			scriptitem = (ZBX_DC_SCRIPTITEM *)zbx_hashset_search(&config->scriptitems, &itemid);

// 			dc_strpool_release(scriptitem->timeout);
// 			dc_strpool_release(scriptitem->script);

// 			zbx_vector_ptr_destroy(&scriptitem->params);
// 			zbx_hashset_remove_direct(&config->scriptitems, scriptitem);
// 		}

// 		/* items */

// 		item_hk_local.hostid = item->hostid;
// 		item_hk_local.key = item->key;

// 		if (NULL == (item_hk = (ZBX_DC_ITEM_HK *)zbx_hashset_search(&config->items_hk, &item_hk_local)))
// 		{
// 			/* item keys should be unique for items within a host, otherwise items with  */
// 			/* same key share index and removal of last added item already cleared index */
// 			THIS_SHOULD_NEVER_HAPPEN;
// 		}
// 		else if (item == item_hk->item_ptr)
// 		{
// 			dc_strpool_release(item_hk->key);
// 			zbx_hashset_remove_direct(&config->items_hk, item_hk);
// 		}

// 		if (ZBX_LOC_QUEUE == item->location)
// 			zbx_binary_heap_remove_direct(&config->queues[item->poller_type], item->itemid);

// 		dc_strpool_release(item->key);
// 		dc_strpool_release(item->delay);
// 		dc_strpool_release(item->history_period);
// 		dc_strpool_release(item->name);
// 		dc_strpool_release(item->description);

// 		if (NULL != item->delay_ex)
// 			dc_strpool_release(item->delay_ex);

// 		if (NULL != item->triggers)
// 			config->items.mem_free_func(item->triggers);

// 		zbx_vector_ptr_destroy(&item->tags);

// 		if (NULL != item->preproc_item)
// 			dc_preprocitem_free(item->preproc_item);

// 		if (NULL != item->master_item)
// 			dc_masteritem_free(item->master_item);

// 		zbx_hashset_remove_direct(&config->items, item);
// 	}

// 	poller_item_notify_flush();

// 	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
// }