/*
** Copyright Glaber
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
#include "log.h"
#include "trends.h"
#include "glb_history.h"

#define MAX_TREND_TTL 2 * 86400  //trends older than that deleted and not exported

static zbx_hashset_t trends = {0};
static int last_trends_cleanup_hour = 0; //if trends aren't coming, they must be exported/cleaned by the end of hour

static trend_t *get_trend(const ZBX_DC_HISTORY *h, int now_hour) {

    trend_t *trend, trend_local;

    if (NULL != (trend = zbx_hashset_search(&trends, &h->metric.itemid)))
        return trend;

    bzero(&trend_local, sizeof(trend_t));

    trend_local.itemid = h->metric.itemid;
    trend_local.hostid = h->metric.hostid;
    trend_local.value_type = h->hist_value_type;
    trend_local.account_hour = now_hour;
    
    trend = zbx_hashset_insert(&trends, &trend_local, sizeof(trend_t));
       
    return trend;
}

static void reset_trend(trend_t *trend, int value_type, int now_hour) {
    
    trend->value_type = value_type; 

    switch (trend->value_type) {

    case ITEM_VALUE_TYPE_FLOAT: 
        trend->value_avg.dbl = 0.0;
        trend->value_min.dbl = 0.0;
        trend->value_max.dbl = 0.0;
        break;

    case ITEM_VALUE_TYPE_UINT64:
        trend->value_avg.ui64 = 0;
        trend->value_max.ui64 = 0;
        trend->value_min.ui64 = 0;
        break;
    }
        
    trend->num = 0;
    trend->account_hour = now_hour;
}

static void export_trend(trend_t *trend, const ZBX_DC_HISTORY *h) {
    
    if (trend->num == 0)
        return;

    switch (trend->value_type) {
        case ITEM_VALUE_TYPE_UINT64:
            trend->value_avg.ui64 = trend->value_avg.ui64 / trend->num;
        break;
        case ITEM_VALUE_TYPE_FLOAT:
            trend->value_avg.dbl = trend->value_avg.dbl / trend->num;
        break;
    }

    trend->item_key = h->item_key;
    trend->host_name = h->host_name;
    glb_history_add_trend(trend);
    trend->item_key = NULL;
    trend->host_name = NULL;

}

static void cleanup_old_trends(int now_hour) {
    zbx_hashset_iter_t iter;

    //metric_processing_data_t proc_data;
    trend_t *trend;
    static int last_cleanup_hour = 0;

    if (last_cleanup_hour == now_hour ) 
        return; //only do cleanup once in the hour 
    
    last_cleanup_hour = now_hour;

    zbx_hashset_iter_reset(&trends, &iter);
    //logic: metrics that hasn't arived for more then TTL are dropped
    //side-effect: metrics having delay > MAX_TREND_TTL are never written to the trends
    while ( NULL !=(trend = zbx_hashset_iter_next(&iter))) {
        if (trend->account_hour + MAX_TREND_TTL <= now_hour ) {
            zbx_hashset_iter_remove(&iter);
        }
    }
}

static void account_metric(trend_t *trend, ZBX_DC_HISTORY *h) {
    
    if (VARIANT_VALUE_ERROR == h->metric.value.type || 
        VARIANT_VALUE_NONE == h->metric.value.type )
        return;

    switch (trend->value_type)
	{
		case ITEM_VALUE_TYPE_FLOAT:
            if (FAIL == zbx_variant_convert(&h->metric.value, VARIANT_VALUE_DBL)) {
                LOG_INF("Couldn't convert item %ld history val type:%s val:%s to DBL to account in trends", h->metric.itemid,
                        zbx_variant_type_desc(&h->metric.value), zbx_variant_value_desc(&h->metric.value));
                return;
            }
			if (trend->num == 0 || h->metric.value.data.dbl < trend->value_min.dbl)
				trend->value_min.dbl = h->metric.value.data.dbl;
			if (trend->num == 0 || h->metric.value.data.dbl > trend->value_max.dbl)
				trend->value_max.dbl = h->metric.value.data.dbl;
			trend->value_avg.dbl += h->metric.value.data.dbl;
			break;
		case ITEM_VALUE_TYPE_UINT64:
            if (FAIL == zbx_variant_convert(&h->metric.value, VARIANT_VALUE_UINT64)) {
                LOG_INF("Couldn't convert item %ld history val type:%s val:%s to UINT64 to account in trends", h->metric.itemid,
                        zbx_variant_type_desc(&h->metric.value), zbx_variant_value_desc(&h->metric.value));
                return;
            }

			if (trend->num == 0 || h->metric.value.data.ui64 < trend->value_min.ui64)
				trend->value_min.ui64 = h->metric.value.data.ui64;
			if (trend->num == 0 || h->metric.value.data.ui64 > trend->value_max.ui64)
				trend->value_max.ui64 = h->metric.value.data.ui64;
            trend->value_avg.ui64 += h->metric.value.data.ui64;
			break;
	}
	trend->num++;
}


//TODO: fix "late" arriving trends problem issue
int trends_account_metric(ZBX_DC_HISTORY *h) {
    
    trend_t *trend;
    
    int now = time(NULL);
    int now_hour = now - now % 3600;

    if (ITEM_VALUE_TYPE_UINT64 != h->hist_value_type && 
        ITEM_VALUE_TYPE_FLOAT != h->hist_value_type) 
            return FAIL;

    trend = get_trend(h, now_hour);
    
    if (trend->account_hour != now_hour ||
        trend->value_type != h->hist_value_type) 
    {   
        DEBUG_ITEM(trend->itemid, "Exporting trend trend value type is %d, proc_value type is %d, trend accout hour is %d, now hour is %d", 
         trend->value_type, (int) h->hist_value_type,  trend->account_hour, now_hour);

        export_trend(trend, h);
        reset_trend(trend, h->hist_value_type, now_hour);
    }
 
    account_metric(trend, h);
    cleanup_old_trends(now_hour);
};

int trends_init_cache() {
    zbx_hashset_create(&trends, 1000, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
};

int trends_destroy_cache() {
    zbx_hashset_destroy(&trends);
};

u_int64_t trend_get_hostid(trend_t *trend) {
    return trend->hostid;
}
u_int64_t trend_get_itemid(trend_t *trend) {
    return trend->itemid;
}

char *trend_get_hostname(trend_t *trend) {
    if (NULL != trend->host_name)
        return trend->host_name;
    return NULL;
}

char *trend_get_itemkey(trend_t *trend) {
    if (NULL != trend->item_key)
        return trend->item_key;
    return NULL;
}