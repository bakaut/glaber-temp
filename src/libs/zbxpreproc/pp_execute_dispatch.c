/*
** Glaber
** Copyright (C) 2001-2100 Glaber
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
#include "zbxjson.h"
#include "metric.h"
#include "zbxcacheconfig.h"
#include "item_preproc.h"
#include "pp_execute_json_discovery.h"
#include "glb_state.h"
#include "glb_preproc.h"
#include "zbxserver.h"
#include "zbxsysinfo.h"
#include "../../zabbix_server/preprocessor/glb_preproc_ipc.h"

extern int		CONFIG_FORKS[ZBX_PROCESS_TYPE_COUNT];
#define MAX_PARAMS 32


typedef struct
{
	char *params[MAX_PARAMS];
	char count;
} 
preproc_params_t;

int  item_preproc_parse_params(char *params_str, preproc_params_t *params)
{
	bzero(params, sizeof(preproc_params_t));

	if (NULL == params_str)
	{
		params->count = -1;
		return FAIL;
	}

	char *ptr = strtok(params_str, "\n");

	while (NULL != ptr && params->count < MAX_PARAMS)
	{
		params->params[params->count] = ptr;
		ptr = strtok(NULL, "\n");
		params->count++;
	}

	return SUCCEED;
}

static void redirect_metric(const metric_t *orig_metric, u_int64_t new_hostid, u_int64_t new_itemid, u_int64_t flags,const zbx_variant_t *value) {
	metric_t metric = {0};
	DEBUG_ITEM(orig_metric->itemid, "Redirect: [%ld, %ld] -> [%ld, %ld]", orig_metric->hostid, orig_metric->itemid, new_hostid, new_itemid);
	DEBUG_ITEM(new_itemid, "Redirect: [%ld, %ld] -> [%ld, %ld]", orig_metric->hostid, orig_metric->itemid, new_hostid, new_itemid);

	metric.hostid = new_hostid;
	metric.itemid = new_itemid;
	metric.flags = flags;
	metric.ts = orig_metric->ts;
	metric.value = *value;
		
	DEBUG_ITEM(new_itemid, "Dispatching orignal metric %" PRIu64 " to %" PRIu64 " with flag %" PRIu64, orig_metric->itemid, new_itemid, flags);
	preprocess_send_metric_hi_priority(&metric);
}
/***************************************************************************
 * dispatches or routes the item to another host/item stated in the cfg    *
 * *************************************************************************/
int pp_execute_dispatch(const metric_t *orig_metric, zbx_variant_t *value,  const char *params_in)
{
	zbx_uint64_pair_t host_item_ids;
	u_int64_t flags;
	char *hostname = NULL, *error = NULL, params_copy[MAX_STRING_LEN];
	preproc_params_t params;

	DEBUG_ITEM(orig_metric->itemid, "In %s: starting", __func__);

	if (FAIL == item_preproc_convert_value(value, ZBX_VARIANT_STR, &error) || NULL == params_in)
		return SUCCEED;

    zbx_strlcpy(params_copy, params_in, MAX_STRING_LEN);
	item_preproc_parse_params(params_copy, &params);

	if (params.count != 2)
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: Wrong number of params: %d instead of 2", params.count);
		return SUCCEED;
	}

	if (NULL == (hostname = get_param_by_name_from_json(value->data.str, params.params[0])))
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: cannot find host name '%s'", params.params[0]);
		return SUCCEED;
	}

	zbx_host_key_t host_key = {.host = hostname, .key = params.params[1]};

	if (SUCCEED == DCconfig_get_itemid_by_key(&host_key, &host_item_ids, &flags))
	{
		redirect_metric(orig_metric, host_item_ids.first, host_item_ids.second, flags, value);

		zbx_variant_clear(value);
		zbx_variant_set_none(value);
	
		return SUCCEED; // this intentional to be able to stop processing via 'custom on fail checkbox'
	}

	DEBUG_ITEM(orig_metric->itemid, "Couldn find itemid for host %s item %s", host_key.host, host_key.key);
	DEBUG_ITEM(orig_metric->itemid, "In %s: finished", __func__);

	return SUCCEED; // we actially failed, but return succeed to continue the item preproc steps to process unmatched items
}

/***************************************************************************
 * dispatches or routes the item to another host/item stated in the cfg    *
 * *************************************************************************/
int pp_execute_dispatch_by_ip(const metric_t *orig_metric, zbx_variant_t *value, const char *params_in)
{
	u_int64_t hostid, new_itemid, new_flags;
	char *ip_str = NULL, *key = NULL, params_copy[MAX_STRING_LEN], *error = NULL;
	preproc_params_t params;

	DEBUG_ITEM(orig_metric->itemid, "In %s: starting", __func__);

	if (FAIL == item_preproc_convert_value(value, ZBX_VARIANT_STR, &error))
		return SUCCEED;
    
    if (NULL == params_in)
        return SUCCEED;

    zbx_strlcpy(params_copy, params_in, MAX_STRING_LEN);

	if (FAIL == item_preproc_parse_params(params_copy, &params))
		return SUCCEED;

	if (params.count != 2)
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: Wrong number of params: %d instead of 2", params.count);
        return SUCCEED;
	}

	key = params.params[1];

	if (NULL == (ip_str = get_param_by_name_from_json(value->data.str, params.params[0])))
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: cannot find ip addr '%s'", params.params[0]);
        return SUCCEED;
	}
    
    //ip often comes with port, so ignore everything after semicolon
	char *semicolon = NULL;

	if (NULL != (semicolon = strchr(ip_str, ':'))) {
		*semicolon ='\0';
		DEBUG_ITEM(orig_metric->itemid, "Found semicolon, new IP string is %s", ip_str);
	}		

	if (0 == (hostid = glb_state_host_find_by_ip(ip_str))) {
		DEBUG_ITEM(orig_metric->itemid,"Cannot dispatch: cannot find host for ip addr '%s'", ip_str);
    	return SUCCEED;
	}

	if (SUCCEED == DCconfig_get_itemid_by_item_key_hostid(hostid, key, &new_itemid, &new_flags))
	{
	
		redirect_metric(orig_metric, hostid, new_itemid, new_flags, value);
		
		zbx_variant_clear(value);
		zbx_variant_set_none(value);

		return SUCCEED; 
	}

	DEBUG_ITEM(orig_metric->itemid, "Couldn find itemid for host %ld item %s", hostid, key);

	DEBUG_ITEM(orig_metric->itemid, "In %s: finished", __func__);
	return SUCCEED; //actially failed, but return succeed to continue the item preproc steps to process unmatched items
}

/***************************************************************************
 * dispatches or routes the item to another host/item stated in the cfg    *
 * *************************************************************************/
int pp_execute_dispatch_local(const metric_t *orig_metric, zbx_variant_t *value,  const char *params_in)
{
	//zbx_uint64_pair_t host_item_ids;
	u_int64_t itemid, flags;
	char *key = NULL, *error = NULL, params_copy[MAX_STRING_LEN];
	preproc_params_t params = {0};

	DEBUG_ITEM(orig_metric->itemid, "In %s: starting", __func__);

	if (FAIL == item_preproc_convert_value(value, ZBX_VARIANT_STR, &error) || NULL == params_in)
		return SUCCEED;

    zbx_strlcpy(params_copy, params_in, MAX_STRING_LEN);
	if (FAIL == item_preproc_parse_params(params_copy, &params))
		return SUCCEED;

	if (params.count != 1)
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: Wrong number of params: %d instead of 1", params.count);
		return SUCCEED;
	}

	if (NULL == (key = get_param_by_name_from_json(value->data.str, params.params[0])))
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: cannot find key '%s'", params.params[0]);
		return SUCCEED;
	}

	if (SUCCEED == DCconfig_get_itemid_by_item_key_hostid(orig_metric->hostid, key, &itemid, &flags)) 
	{
		redirect_metric(orig_metric, orig_metric->hostid, itemid, flags, value);

		zbx_variant_clear(value);
		zbx_variant_set_none(value);
	
		return SUCCEED; // this intentional to be able to stop processing via 'custom on fail checkbox'
	}
}


static int match_regexp_result(u_int64_t itemid, const char *value, const char *regexp) {
 	zbx_vector_expression_t	regexps;
 	
	int result;
	zbx_vector_expression_create(&regexps);

 	if ('@' == *regexp) {
 		DCget_expressions_by_name(&regexps, regexp + 1);
		
		if (0 == regexps.values_num)
		{
			DEBUG_ITEM(itemid, "Global regular expression \"%s\" does not exist.", regexp + 1);
			
			zbx_vector_expression_destroy(&regexps);
			return ZBX_REGEXP_NO_MATCH;
		}
	}

	result = zbx_regexp_match_ex(&regexps, value, regexp, ZBX_CASE_SENSITIVE);

	DEBUG_ITEM(itemid, "Regexp '%s' match result on '%s' is %d", regexp, value, result);

	zbx_regexp_clean_expressions(&regexps);
	zbx_vector_expression_destroy(&regexps);
	
	return result;
}

/***************************************************************************
 * dispatches or routes the item to another host/item stated in the cfg    *
 * *************************************************************************/
int pp_execute_dispatch_local_regexp(const metric_t *orig_metric, zbx_variant_t *value,  const char *params_in)
{
	u_int64_t itemid, flags;
	char *key = NULL, *error = NULL, params_copy[MAX_STRING_LEN];
	DC_ITEM			*items = NULL;
	int num, i, matched_result = 0;
	preproc_params_t params;
	
	DEBUG_ITEM(orig_metric->itemid, "In %s: starting", __func__);

	if (FAIL == item_preproc_convert_value(value, ZBX_VARIANT_STR, &error) || NULL == params_in)
		return SUCCEED;

    zbx_strlcpy(params_copy, params_in, MAX_STRING_LEN);
	
	if (FAIL == item_preproc_parse_params(params_copy, &params))
		return SUCCEED;

	if (params.count != 1)
	{
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: Wrong number of params: %d instead of 1", params.count);
		return SUCCEED;
	}

	if (NULL == (key = get_param_by_name_from_json(value->data.str, params.params[0])))
	{
		LOG_INF("Cannot dispatch by key '%s',", params.params[0]);
		DEBUG_ITEM(orig_metric->itemid, "Cannot dispatch: cannot find key '%s'", params.params[0]);
		return SUCCEED;
	}

	num = DCconfig_get_regexp_items_keys_by_hostid(orig_metric->hostid, &items);

	for (i = 0; i < num; i++)
	{
		int errcode = SUCCEED;
		const char *regexp;
		AGENT_REQUEST ar;
		zbx_init_agent_request(&ar);

		items[i].key = zbx_strdup(items[i].key, items[i].key_orig);
		if (SUCCEED != zbx_substitute_key_macros(&items[i].key, NULL, &items[i], NULL, NULL,
				MACRO_TYPE_ITEM_KEY, error, sizeof(error)))
			continue;

		if (SUCCEED != zbx_parse_item_key(items[i].key, &ar)) 
			continue;
		
		if (1 != get_rparams_num(&ar) ) {
			DEBUG_ITEM(items[i].itemid,"There should be only one param in the item key, item has %d params", get_rparams_num(&ar));
			zbx_free_agent_request(&ar);
			continue;
		}
		
		if (NULL != (regexp = get_rparam(&ar, 0))) {
			DEBUG_ITEM(items[i].itemid, "Got regexp from item: '%s'", regexp);
		}
		
		if (ZBX_REGEXP_MATCH == match_regexp_result(items[i].itemid, key, regexp)) {
			matched_result = 1;
			redirect_metric(orig_metric, items[i].host.hostid, items[i].itemid, items[i].flags, &orig_metric->value);
		}

	    zbx_free_agent_request(&ar);
	}

	DCconfig_clean_items(items, NULL, num);
	zbx_free(items);

	if (matched_result) {
		zbx_variant_clear(value);
		zbx_variant_set_none(value);
		return FAIL;	
	}
	
	return SUCCEED;
}