#include "zbxcommon.h"
#include "log.h"
#include "zbxjson.h"
#include "zbxalgo.h"
#include "zbxhistory.h"
#include "zbxself.h"
#include "history.h"
#include "module.h"
#include "zbxstr.h"
#include "zbx_item_constants.h"
#include "zbx_trigger_constants.h"
#include "zbxcacheconfig.h"

#if defined(HAVE_LIBCURL) && LIBCURL_VERSION_NUM >= 0x071c00

#define GLB_DEFAULT_CLICKHOUSE_TYPES "dbl, str, log, uint, text"
#define GLB_DEFAULT_CLICKHOUSE_DISABLE_HOST_ITEMS_NAMES	0
#define GLB_DEFAULT_CLICKHOUSE_DISABLE_NANOSECONDS	1
#define GLB_DEFAULT_CLICKHOUSE_DBNAME	"glaber"
#define GLB_DEFAULT_CLICKHOUSE_PRELOAD_VALUES 0
#define GLB_DEFAULT_CLICKHOUSE_DISABLE_READ	30
#define GLB_DEFAULT_CLICKHOUSE_DISABLE_TRENDS 0
#define GLB_CLICKHOUSE_WRITE_BATCH	1000000
#define GLB_CLICKHOUSE_FLUSH_TIMEOUT 3

#define MAX_REASONABLE_BUFFER_SIZE 10000000
#define ESCAPE_CHARS "'\\"

typedef struct
{
	char	*data;
	size_t	alloc;
	size_t	offset;
}
zbx_httppage_t;

typedef struct {
	char *buffer;
	size_t alloc;
	size_t offset;
	int lastflush;
	int num;
} 
glb_clickhouse_buffer_t;

typedef struct
{
	char	*url;
	char 	*safe_url;
	char 	dbname[MAX_STRING_LEN];
	u_int8_t read_types[ITEM_VALUE_TYPE_MAX];
	u_int8_t write_types[ITEM_VALUE_TYPE_MAX];
	u_int8_t read_aggregate_types[ITEM_VALUE_TYPE_MAX];
	u_int8_t disable_host_item_names;
	u_int8_t disable_nanoseconds;
	u_int16_t disable_read_timeout;
	int startup_time;

	char *sql_buffer;
    size_t buf_alloc;

	zbx_httppage_t page_r;
	struct curl_slist	*curl_headers;
	glb_clickhouse_buffer_t hist_tbuffer[ITEM_VALUE_TYPE_MAX];	
	glb_clickhouse_buffer_t trends_tbuffer[ITEM_VALUE_TYPE_MAX];	

}
glb_clickhouse_data_t;


static char *trend_tables[] = {"trends_dbl", "", "", "trends_uint",""};
static char *hist_tables[] = {"history_dbl", "history_str", "history_log", "history_uint", "history_str"};

static size_t	curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t	r_size = size * nmemb;

	zbx_httppage_t	*page = (zbx_httppage_t	*)userdata;
	zbx_strncpy_alloc(&page->data, &page->alloc, &page->offset, ptr, r_size);

	return r_size;
}

static void	clickhouse_log_error(CURL *handle, CURLcode error, const char *errbuf,zbx_httppage_t *page_r)
{
	long	http_code;

	if (CURLE_HTTP_RETURNED_ERROR == error)
	{
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);
		if (0 != page_r->offset)
			LOG_WRN("cannot get values from clickhouse, HTTP error: %ld, message: %s",http_code, page_r->data);
		else
			LOG_WRN("cannot get values from clickhouse, HTTP error: %ld", http_code);
	}
	else
		LOG_WRN("cannot get values from clickhouse: %s",'\0' != *errbuf ? errbuf : curl_easy_strerror(error));
}

static void	clickhouse_destroy(void *data)
{
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;

	zbx_free(conf->url);
	zbx_free(conf->safe_url);
	zbx_free(conf->sql_buffer);
	zbx_free(conf->page_r.data);
	zbx_free(data);
}

static int curl_post_request(glb_clickhouse_data_t *conf, char *postdata, char **response) {
 	
	CURLcode		err;
	CURL	*handle = NULL;
	char  errbuf[CURL_ERROR_SIZE];
	
	if (conf->page_r.alloc > MAX_REASONABLE_BUFFER_SIZE) {
		zbx_free(conf->page_r.data);
		bzero(&conf->page_r,sizeof(zbx_httppage_t));	
	}

	if (NULL == postdata)
		return SUCCEED;

	if (NULL == (handle = curl_easy_init()))
	{
		LOG_WRN("cannot initialize cURL session");
		return FAIL;
	} 

	curl_easy_setopt(handle, CURLOPT_URL, conf->url);
	curl_easy_setopt(handle, CURLOPT_POSTFIELDS, postdata);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &conf->page_r);
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, conf->curl_headers);
	curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);

	conf->page_r.offset = 0;
	*errbuf = '\0';

	if (CURLE_OK != (err = curl_easy_perform(handle)))
	{
		clickhouse_log_error(handle, err, errbuf,&conf->page_r);
        LOG_WRN("Failed url '%s' postdata '%s' ", conf->safe_url, postdata);
	} 

	curl_easy_cleanup(handle);
	curl_slist_free_all(conf->curl_headers);
	if (CURLE_OK != err) 
		return FAIL;
	
	if (NULL != response)
		*response = conf->page_r.data;
	
	return SUCCEED;
}

/* agregation just checks for fields presence without actial conversion */
static int parse_aggregate_response(u_int64_t itemid, char *response, struct zbx_json *json) {
	const char *p = NULL;
	size_t alloc = 0, offset = 0; 
	struct zbx_json_parse jp, jp_data;
	int valuecount = 0;

	if ( NULL == response) 
		return FAIL;

	if (SUCCEED != zbx_json_open(response, &jp)) {
		LOG_WRN("Couldn't parse json response %s", response);
		return FAIL;
	}
	
	if (SUCCEED != zbx_json_brackets_by_name(&jp, "data", &jp_data)) {
		LOG_WRN("Couldn't find data section in the response %s:", response);
		return FAIL;
	};
    
	LOG_DBG("Response is %s", response);
	
    while (NULL != (p = zbx_json_next(&jp_data, p)))
	{
        char clck[MAX_ID_LEN], value[MAX_STRING_LEN];
		char request[MAX_STRING_LEN],min_value[MAX_ID_LEN],max_value[MAX_ID_LEN],avg_value[MAX_ID_LEN], i[MAX_ID_LEN], count[MAX_ID_LEN];
		zbx_json_type_t type;
        
        struct zbx_json_parse	jp_row;
		
        if (SUCCEED == zbx_json_brackets_open(p, &jp_row) &&
            SUCCEED == zbx_json_value_by_name(&jp_row, "clcck", clck,MAX_ID_LEN, &type) &&
            SUCCEED == zbx_json_value_by_name(&jp_row, "count", count, MAX_ID_LEN,&type) &&
			SUCCEED == zbx_json_value_by_name(&jp_row, "max", max_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "avg", avg_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "min", min_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "i", i, MAX_ID_LEN,&type) )
		{
			zbx_json_addobject(json,NULL);
			zbx_json_adduint64 (json, "itemid", itemid);
			zbx_json_addstring( json, "clock", clck, ZBX_JSON_TYPE_INT);
			zbx_json_addstring( json, "count", count, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "max", max_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "min", min_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "avg", avg_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "i", i, ZBX_JSON_TYPE_INT);
			zbx_json_close(json);
        } else {
            zabbix_log(LOG_LEVEL_DEBUG,"Couldn't parse JSON row: %s",p);
        };
	}
	
	return SUCCEED;
}


static int parse_trends_response(char *response, struct zbx_json *json) {
	const char *p = NULL;
	size_t alloc = 0, offset = 0; 
	struct zbx_json_parse jp, jp_data;
	int valuecount = 0;

	if ( NULL == response) 
		return FAIL;
	
	if (SUCCEED != zbx_json_open(response, &jp)) {
		LOG_WRN("Couldn't parse json response %s", response);
		return FAIL;
	}
	
	if (SUCCEED != zbx_json_brackets_by_name(&jp, "data", &jp_data)) {
		LOG_WRN("Couldn't find data section in the response %s:", response);
		return FAIL;
	};
    
	LOG_DBG("Response is %s", response);

    while (NULL != (p = zbx_json_next(&jp_data, p)))
	{
        char itemid[MAX_ID_LEN], clck[MAX_ID_LEN], ns[MAX_ID_LEN],value[MAX_STRING_LEN];
		char request[MAX_STRING_LEN],min_value[MAX_ID_LEN],max_value[MAX_ID_LEN],avg_value[MAX_ID_LEN], count[MAX_ID_LEN];
		zbx_json_type_t type;
        
        struct zbx_json_parse	jp_row;

        if (SUCCEED == zbx_json_brackets_open(p, &jp_row) &&
			SUCCEED == zbx_json_value_by_name(&jp_row, "clcck", clck,MAX_ID_LEN, &type) &&
            SUCCEED == zbx_json_value_by_name(&jp_row, "num", count, MAX_ID_LEN,&type) &&
			SUCCEED == zbx_json_value_by_name(&jp_row, "value_max", max_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "value_avg", avg_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "value_min", min_value, MAX_ID_LEN,&type) && 
			SUCCEED == zbx_json_value_by_name(&jp_row, "itemid", itemid, MAX_ID_LEN,&type)
		)
		{
			zbx_json_addobject(json,NULL);
			zbx_json_addstring (json, "itemid", itemid, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "clock", clck, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "value_max", max_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "value_min", min_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "value_avg", avg_value, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring( json, "num", count, ZBX_JSON_TYPE_STRING);
			zbx_json_close(json);
		} else {
           LOG_INF("Couldn't parse JSON row: %s",p);
        };
	}
	return SUCCEED;
}


static int	get_trend_values_json(void *data, int value_type, zbx_uint64_t itemid, int start, int end, struct zbx_json *json) {
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;
	
    size_t buf_offset = 0;
	
	char *response;

	buf_offset = 0;
	
	LOG_DBG("In %s() trends request for item %ld", __func__,itemid);

	if (end < start ) {
		zabbix_log(LOG_LEVEL_WARNING,"%s: wrong params requested: start:%d, end:%d",__func__, start, end);
		return FAIL;
	}

	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, 
		"SELECT itemid, \
			count as num, \
			toUnixTimestamp(clock) as clcck ,\
			value_avg, \
			value_min, \
			value_max \
		FROM %s.%s  \
		WHERE clock BETWEEN %d AND %d AND \
		itemid = %ld \
		ORDER BY clock \
		FORMAT JSON",  conf->dbname, trend_tables[value_type], start, end, itemid); 

	LOG_DBG("Sending query to '%s' post data: '%s'", conf->safe_url, conf->sql_buffer);

	if (SUCCEED != curl_post_request(conf, conf->sql_buffer, &response)) 
		return FAIL;
	
	LOG_DBG("Recieved from clickhouse: %s", response);
	
	if (SUCCEED != parse_trends_response(response, json)) 
			return FAIL;
	
	LOG_DBG("Resulting trends JSON is %s", json->buffer);

	return SUCCEED;
}

static int	get_trend_aggregates_json(void *data, int value_type, zbx_uint64_t itemid, int start, int end, int steps, struct zbx_json *json)
{
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;
	
	size_t buf_offset = 0;
	
	char *response;

	buf_offset = 0;
	
	LOG_DBG("In %s() trends request for item %ld", __func__,itemid);

	if (0 == conf->read_aggregate_types[value_type])	
			return SUCCEED;

	if (end < start || 1 > steps ) {
		zabbix_log(LOG_LEVEL_WARNING,"%s: wrong params requested: start:%d, end:%d, steps:%d",__func__, start, end);
		return FAIL;
	}
	
	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, 
		"SELECT itemid, \
			sum(count) as count, \
			round( multiply((toUnixTimestamp(clock)-%ld), %ld) / %ld ,0) as i,\
			max(toUnixTimestamp(clock)) as clcck ,\
			avg(value_avg) as avg, \
			min(value_min) as min , \
			max(value_max) as max \
		FROM %s.%s  \
		WHERE clock BETWEEN %d AND %d AND \
		itemid = %ld \
		GROUP BY itemid, i \
		ORDER BY i \
		FORMAT JSON",  start, steps, end-start, conf->dbname, trend_tables[value_type], start, end, itemid); 
	
	LOG_DBG("Sending query to '%s' post data: '%s'", conf->safe_url, conf->sql_buffer);
		
	if (SUCCEED != curl_post_request(conf, conf->sql_buffer, &response)) 
		return FAIL;
	
	LOG_DBG("Recieved from clickhouse: %s", response);
	
	if (SUCCEED != parse_aggregate_response(itemid, response, json)) 
			return FAIL;

	return SUCCEED;
}

static int parse_history_get_values(u_int64_t itemid, glb_clickhouse_data_t *conf, char *response, int value_type, zbx_vector_history_record_t *values) {
	struct zbx_json_parse	jp, jp_row, jp_data;
	const char		*p = NULL;
	zbx_history_record_t	hr;

    char  clck[MAX_ID_LEN], ns[MAX_ID_LEN], *value = NULL, *source = NULL;
    size_t value_alloc = 0;

    zbx_json_open(response, &jp);
    zbx_json_brackets_by_name(&jp, "data", &jp_data);
    
	//LOG_INF("Read history value for item %ld value type is %d", itemid, value_type);

    while (NULL != (p = zbx_json_next(&jp_data, p)))
	{
		zbx_json_type_t type;

        if (FAIL == zbx_json_brackets_open(p, &jp_row) ||
			FAIL == zbx_json_value_by_name(&jp_row, "clock", clck, MAX_ID_LEN, &type) ||
			FAIL == zbx_json_value_by_name_dyn(&jp_row, "value", &value, &value_alloc, &type ) )
		{
			LOG_DBG("Cannot parse row of data (might be ok, but empty): %s", p);
			continue;
		}
		
	   	if ( 0 == conf->disable_nanoseconds &&
			 SUCCEED == zbx_json_value_by_name(&jp_row, "ns", ns, MAX_ID_LEN, &type) ) 
		{
			hr.timestamp.ns =  strtoll(ns,NULL, 10); 
		} else 
			hr.timestamp.ns = 0;

	    hr.timestamp.sec = strtoll( clck,NULL, 10); ;
		hr.value = history_str2value(value, value_type);
		zbx_vector_history_record_append_ptr(values, &hr);
		
		if (ITEM_VALUE_TYPE_LOG == value_type) {
			//additionally parsing severity, source and logeventid
			hr.value.log->logeventid = 0;
			hr.value.log->severity = TRIGGER_SEVERITY_UNDEFINED;
			hr.value.log->source = NULL;
			
			if (SUCCEED == zbx_json_value_by_name(&jp_row, "logeventid", ns, MAX_ID_LEN, &type)) 
				hr.value.log->logeventid = strtoll(ns,NULL, 10); 
			
			if (SUCCEED == zbx_json_value_by_name(&jp_row, "severity", ns, MAX_ID_LEN, &type)) 
				hr.value.log->severity = strtoll(ns,NULL, 10); 
			
			if (SUCCEED ==  zbx_json_value_by_name(&jp_row, "source", ns, MAX_ID_LEN, &type )) 
				hr.value.log->source = zbx_strdup(NULL,ns);
		}
	}
	return SUCCEED;
}

static int	clickhouse_get_values(void *data, int value_type, zbx_uint64_t itemid, int start, int count, int end, unsigned char interactive,
		zbx_vector_history_record_t *values)
{
	int valuecount=0;

	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;

	size_t buf_offset = 0;

	buf_offset = 0;
    char *response;

	LOG_DBG("In %s()", __func__);
	
	if (0 == conf->read_types[value_type])	
			return SUCCEED;

    if (time(NULL)- conf->disable_read_timeout < conf->startup_time && 0 == interactive ) {
		LOG_DBG("waiting for cache load, exiting");
      	return SUCCEED;
	}
	
	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, 
			"SELECT  toUInt32(clock) clock,value");

	if (value_type==ITEM_VALUE_TYPE_LOG) 
		zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, ",source,severity,logeventid");
	
	if ( 0 == conf->disable_nanoseconds ) {
		zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, ",ns");
	}
	
	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, " FROM %s.%s WHERE itemid=%ld ",
		conf->dbname,hist_tables[value_type], itemid);

	if (1 == end-start) {
		zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "AND clock = %d ", end);
	} else {
		if (0 < start) {
			zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "AND clock > %d ", start);
		}
		if (0 < end ) {
			zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "AND clock <= %d ", end);
		}
	}

	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "ORDER BY clock DESC ");

	if (0 < count) 
	{
	    zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "LIMIT %d ", count);
	}

    zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, "format JSON ");

	DEBUG_ITEM(itemid, "Executing clickhouse query: %s", conf->sql_buffer);
	if (SUCCEED != curl_post_request(conf, conf->sql_buffer, &response)) {
		DEBUG_ITEM(itemid, "Query failed");
	 	return FAIL;
	}
    if (SUCCEED != parse_history_get_values(itemid, conf, response, value_type, values)) {
		DEBUG_ITEM(itemid, "Couldn't parse response %s", response);
		return FAIL;
	}

	zbx_vector_history_record_sort(values, (zbx_compare_func_t)zbx_history_record_compare_desc_func);
	LOG_DBG( "End of %s()", __func__);

	return SUCCEED;
}

static int	get_history_aggregates_json(void *data, int value_type, zbx_uint64_t itemid, int start, int end, int steps, struct zbx_json *json)
{
	int valuecount=0;
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;

	size_t buf_offset = 0;
	
	char *response;

    char *table_name=NULL;

	LOG_DBG("In %s()", __func__);
	
	if (0 == conf->read_aggregate_types[value_type])	
		return SUCCEED;
	
	if ( value_type != ITEM_VALUE_TYPE_FLOAT && value_type != ITEM_VALUE_TYPE_UINT64)
		return FAIL;

	if (end < start || steps <1 ) {
		LOG_WRN("%s: wrong params requested: start:%d end:%d, aggregates: %ld",__func__,start,end, steps);
		return FAIL;
	}
	
	if ( value_type != ITEM_VALUE_TYPE_FLOAT && value_type != ITEM_VALUE_TYPE_UINT64 ) {
		THIS_SHOULD_NEVER_HAPPEN;
		return FAIL;
	}
	
	table_name=hist_tables[value_type];
   
	zbx_snprintf_alloc(&conf->sql_buffer, &conf->buf_alloc, &buf_offset, 
	"SELECT itemid, \
		round( multiply((toUnixTimestamp(clock)-%ld), %ld) / %ld ,0) as i,\
		max(toUnixTimestamp(clock)) as clcck ,\
		avg(value) as avg, \
		count(value) as count, \
		min(value) as min , \
		max(value) as max \
	FROM %s.%s h \
	WHERE clock BETWEEN %ld AND %ld AND \
	itemid = %ld \
	GROUP BY itemid, i \
	ORDER BY i \
	FORMAT JSON", start, steps, end-start, 
				conf->dbname, table_name,  start, end, itemid);

	DEBUG_ITEM(itemid, "Executing aggregation query: %s", conf->sql_buffer);

	if (SUCCEED != curl_post_request(conf, conf->sql_buffer, &response)) { 
		DEBUG_ITEM(itemid, "Request failed");
		return FAIL;
	}
    
	if (SUCCEED != parse_aggregate_response(itemid, response, json))  {
		DEBUG_ITEM(itemid, "Couldn't parse response: %s", response);
		return FAIL;
	}

	DEBUG_ITEM(itemid, "Finished parsing, returning, buffer is '%s'", json->buffer);
	return SUCCEED;
}

static void submit_sql_buffer(glb_clickhouse_data_t *conf, glb_clickhouse_buffer_t *buf) {
	
	if ( (buf->num < GLB_CLICKHOUSE_WRITE_BATCH) &&
	      buf->lastflush + GLB_CLICKHOUSE_FLUSH_TIMEOUT > time(NULL) && buf->num > 0 ) 
		return;

	if (0 == buf->alloc || NULL == buf->buffer || '0' == buf->buffer[0])
		return;
	
	if (SUCCEED != curl_post_request(conf, buf->buffer, NULL)) 
		LOG_WRN("FAILED to flush %d values to clickhouse", buf->num);

	buf->offset=0;
	buf->num=0;
	buf->lastflush=time(NULL);
	
	if (buf->alloc > 0)
		buf->buffer[0]='0';

	if (buf->alloc > MAX_REASONABLE_BUFFER_SIZE) {
		zbx_free(buf->buffer);
		buf->buffer = NULL;
		buf->alloc=0;
	}
}

static int	add_history_values(void *data, ZBX_DC_HISTORY *hist, int history_num)
{
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;
	int			i,j;
	
	char *escaped_value=NULL;
	char *response;

	ZBX_DC_HISTORY		*h;
	
	LOG_DBG("In %s()", __func__);

	for (i = 0; i < history_num; i++)
	{
		h = (ZBX_DC_HISTORY *)&hist[i];
		
		if ( VARIANT_VALUE_ERROR ==h->metric.value.type || 
			VARIANT_VALUE_NONE == h->metric.value.type || (h->metric.flags & (ZBX_DC_FLAG_UNDEF | ZBX_DC_FLAG_NOHISTORY))) {
			DEBUG_ITEM(h->metric.itemid, "Not saving item's history to clickhouse, flags are %u", h->metric.flags);
			continue;
		}
		
		DEBUG_ITEM(h->metric.itemid, "Saving item's history to clickhouse, flags are %u", h->metric.flags);

		int value_type = h->hist_value_type;
		glb_clickhouse_buffer_t *buf = &conf->hist_tbuffer[value_type];
		
		if (value_type < 0 || value_type >= ITEM_VALUE_TYPE_MAX) {
			LOG_INF("Wrong value type: %d, internal programming bug", value_type);
			THIS_SHOULD_NEVER_HAPPEN;
		}
	
		if (0 == conf->write_types[h->hist_value_type])	{
			DEBUG_ITEM(h->metric.itemid, "Skipping unsupported value type %d", h->hist_value_type);
			continue;
		}
	
		if (buf->num == 0) {
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,
						"INSERT INTO %s.%s (day,itemid,clock,value", conf->dbname, hist_tables[value_type]); 
			
			if ( 0 == conf->disable_nanoseconds ) {
				zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",ns");
			}
	
			if ( 0 == conf->disable_host_item_names ) {
				zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",hostname, itemname");
			}

			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,") VALUES ");
		} else  zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",");
		
		//common part
		zbx_snprintf_alloc(&buf->buffer, &buf->alloc,&buf->offset,"(CAST(%d as date) ,%ld,%d",
				h->metric.ts.sec,h->metric.itemid, h->metric.ts.sec);
    	
		switch (h->hist_value_type)
		{
		case ITEM_VALUE_TYPE_UINT64:
		case ITEM_VALUE_TYPE_FLOAT:
		    zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",%s", 
							zbx_variant_value_desc(&h->metric.value));
			break;
		case ITEM_VALUE_TYPE_LOG:
		case ITEM_VALUE_TYPE_STR:
		case ITEM_VALUE_TYPE_TEXT:
			if (NULL == h->metric.value.data.str)
				escaped_value = zbx_strdup(NULL, "");
			else 
				escaped_value = zbx_dyn_escape_string(zbx_variant_value_desc(&h->metric.value), ESCAPE_CHARS);

			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,", '%s'", escaped_value);
			zbx_free(escaped_value);
			break;
		
		default:
			LOG_WRN("Unknown value type %d", h->hist_value_type);
			THIS_SHOULD_NEVER_HAPPEN;

			break;
		}

		if ( 0 == conf->disable_nanoseconds) {
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",%d", h->metric.ts.ns);
		}
		
		if ( 0 == conf->disable_host_item_names ) {
			char *host_name, *item_key;
			if ( h->host_name == NULL) 
				host_name = zbx_strdup(NULL,"");
			else 
				host_name = zbx_dyn_escape_string(h->host_name, ESCAPE_CHARS);   
			
			if (h->item_key == NULL)	
				item_key = zbx_strdup(NULL, "");
			else 
			    item_key = zbx_dyn_escape_string(h->item_key, ESCAPE_CHARS);   
			
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,",'%s','%s'", host_name, item_key);
			
			zbx_free(host_name);
			zbx_free(item_key);
		}

		zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,")");

		buf->num++;
	}
	
	int value_type;
	for ( value_type = 0; value_type < ITEM_VALUE_TYPE_MAX; value_type++ ) {
		glb_clickhouse_buffer_t *buf = &conf->hist_tbuffer[value_type];
		
	

		submit_sql_buffer(conf, buf);
	}

	LOG_DBG("End of %s()", __func__);

	return SUCCEED;
}

static int	add_trend_values(void *data, trend_t *trend)
{
	glb_clickhouse_data_t	*conf = (glb_clickhouse_data_t *)data;
	int		value_type = trend->value_type;

 	glb_clickhouse_buffer_t *buf = &conf->trends_tbuffer[value_type]; 	

	char *response;

	char *host_name, *item_key;	
	char *precision="%0.4f,";
	
	DEBUG_ITEM(trend->itemid, "Got trend data: itemid %ld %s:%s", trend->itemid, trend->host_name, trend->item_key);

	if (0 == buf->num) {
		zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,
			"INSERT INTO %s.%s (day, itemid, clock, value_min, value_max, value_avg, count, hostname, itemname) VALUES", 
			conf->dbname, trend_tables[value_type]);
	} else {
		zbx_snprintf_alloc(&buf->buffer,&buf->alloc, &buf->offset,",");
	}

	zbx_snprintf_alloc(&buf->buffer,&buf->alloc,&buf->offset,
		"(CAST(%d as date) ,%ld,%d,", trend->account_hour,trend->itemid,trend->account_hour);
    	
	switch (value_type) {
		
		case ITEM_VALUE_TYPE_FLOAT:
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset, precision, trend->value_min.dbl);
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset, precision, trend->value_max.dbl);
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset, precision, trend->value_avg.dbl);
			break;
		case ITEM_VALUE_TYPE_UINT64:
			zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,"%ld,%ld,%ld,",
		   			trend->value_min.ui64,
					trend->value_max.ui64,
					(trend->value_avg.ui64 / trend->num) );
			break;
		default:
			LOG_INF("Clickhouse export trend: type %d is not supported, itemid %ld", value_type, trend->itemid);
			THIS_SHOULD_NEVER_HAPPEN;
			break;
	}	
	
	host_name = zbx_dyn_escape_string(trend->host_name, ESCAPE_CHARS);   
	item_key = zbx_dyn_escape_string(trend->item_key, ESCAPE_CHARS);   

	zbx_snprintf_alloc(&buf->buffer, &buf->alloc, &buf->offset,	"%d, '%s','%s')", trend->num, host_name, item_key);
		
	zbx_free(host_name);
	zbx_free(item_key);

    buf->num++;
	
    for (value_type = 0; value_type < ITEM_VALUE_TYPE_MAX; value_type ++ ) {
		
		if (value_type != ITEM_VALUE_TYPE_UINT64 || value_type != ITEM_VALUE_TYPE_FLOAT)
			continue;

		submit_sql_buffer(conf, &conf->trends_tbuffer[value_type]);
	}

	LOG_DBG("End of %s()", __func__);
	return SUCCEED;
}


/************************************************************************************
 *                                                                                  *
 * Function: zbx_history_clickhouse_init                                               *
 *                                                                                  *
 * Purpose: initializes history storage interface                                   *
 *                                                                                  *
 * Parameters:  hist       - [IN] the history storage interface                     *
 *              value_type - [IN] the target value type                             *
 *              error      - [OUT] the error message                                *
 *                                                                                  *
 * Return value: SUCCEED - the history storage interface was initialized            *
 *               FAIL    - otherwise                                                *
 *                                                                                  *
 ************************************************************************************/
int	glb_history_clickhouse_init(char *params)
{
	glb_clickhouse_data_t	*conf;

    struct zbx_json_parse jp, jp_config;
	char  username[MAX_ID_LEN],password[MAX_ID_LEN],tmp_str[MAX_STRING_LEN];
	size_t alloc=0,offset=0, s_alloc = 0, s_offset = 0;
	zbx_json_type_t type;

	conf = (glb_clickhouse_data_t *)zbx_malloc(NULL, sizeof(glb_clickhouse_data_t));
	memset(conf, 0, sizeof(glb_clickhouse_data_t));

	conf->startup_time = time(NULL);
	zabbix_log(LOG_LEVEL_DEBUG,"in %s: starting init", __func__);

    if ( SUCCEED != zbx_json_open(params, &jp_config)) {
		zabbix_log(LOG_LEVEL_WARNING, "Couldn't parse configureation: '%s', most likely not a valid JSON", params);
		return FAIL;
	}

	zbx_strlcpy(tmp_str,"http://localhost:8123",MAX_STRING_LEN);

	if ( SUCCEED != zbx_json_value_by_name(&jp_config,"url", tmp_str, MAX_STRING_LEN,&type))  
    	LOG_WRN("%s: Couldn't find url param, using default '%s'",__func__,tmp_str);
	
	zbx_rtrim(tmp_str, "/");
	zbx_snprintf_alloc(&conf->url,&alloc,&offset,"%s",tmp_str);
	zbx_snprintf_alloc(&conf->safe_url,&s_alloc,&s_offset,"%s",tmp_str);
		    
	if (SUCCEED == zbx_json_value_by_name(&jp_config,"username", username, MAX_ID_LEN,&type)  && 
		SUCCEED == zbx_json_value_by_name(&jp_config,"password", password, MAX_ID_LEN,&type) ) {
		
		zbx_snprintf_alloc(&conf->url,&alloc,&offset,"/?user=%s&password=%s",username,password);
		zbx_snprintf_alloc(&conf->safe_url,&s_alloc,&s_offset,"/?user=%s&password=%s","<HIDDEN>","<HIDDEN>");
	}

	if (SUCCEED == zbx_json_value_by_name(&jp_config,"dbname", tmp_str, MAX_ID_LEN,&type) )
		zbx_strlcpy(conf->dbname,tmp_str,MAX_STRING_LEN);
	else 	zbx_strlcpy(conf->dbname,GLB_DEFAULT_CLICKHOUSE_DBNAME,MAX_STRING_LEN);

	
	zbx_strlcpy(tmp_str, GLB_DEFAULT_CLICKHOUSE_TYPES, MAX_STRING_LEN);
	zbx_json_value_by_name(&jp_config, "write_types", tmp_str, MAX_STRING_LEN,&type);
	glb_set_process_types(conf->write_types, tmp_str);
	
	if (glb_types_array_sum(conf->write_types) > 0) {
		zabbix_log(LOG_LEVEL_INFORMATION, "Init clickhouse module: WRITE types '%s'",tmp_str);
		glb_register_callback(GLB_MODULE_API_HISTORY_WRITE,(void (*)(void))add_history_values,conf);
	}

	zbx_strlcpy(tmp_str,GLB_DEFAULT_CLICKHOUSE_TYPES, MAX_STRING_LEN);
	zbx_json_value_by_name(&jp_config,"read_types", tmp_str, MAX_STRING_LEN,&type);
	glb_set_process_types(conf->read_types, tmp_str);
	
	if (glb_types_array_sum(conf->read_types) > 0) {
		zabbix_log(LOG_LEVEL_INFORMATION, "Init clickhouse module: READ types '%s'",tmp_str);
		glb_register_callback(GLB_MODULE_API_HISTORY_READ,(void (*)(void))clickhouse_get_values,conf);
	}
		
	zbx_strlcpy(tmp_str,GLB_DEFAULT_CLICKHOUSE_TYPES,MAX_STRING_LEN);
	zbx_json_value_by_name(&jp_config,"read_aggregate_types", tmp_str, MAX_STRING_LEN,&type);
	glb_set_process_types(conf->read_aggregate_types, tmp_str);
	
	if (glb_types_array_sum(conf->read_aggregate_types) > 0) {
		zabbix_log(LOG_LEVEL_INFORMATION, "Init clickhouse module: AGGREGATE READ types '%s'",tmp_str);
		glb_register_callback(GLB_MODULE_API_HISTORY_READ_AGG_JSON,(void (*)(void))get_history_aggregates_json,conf);
	}
	
	if ( (SUCCEED == zbx_json_value_by_name(&jp_config,"enable_trends", tmp_str, MAX_ID_LEN,&type))  &&
		 ( strcmp(tmp_str,"0") == 0 || strcmp(tmp_str,"false") ==0 )) {
			LOG_WRN("Trends are disabled");
	} else {
		glb_register_callback(GLB_MODULE_API_HISTORY_WRITE_TRENDS,(void (*)(void))add_trend_values, conf);
		glb_register_callback(GLB_MODULE_API_HISTORY_READ_TRENDS_AGG_JSON,(void (*)(void))get_trend_aggregates_json, conf);
		glb_register_callback(GLB_MODULE_API_HISTORY_READ_TRENDS_JSON,(void (*)(void))get_trend_values_json, conf);
	}

	conf->disable_read_timeout=GLB_DEFAULT_CLICKHOUSE_DISABLE_READ;
	if (SUCCEED ==zbx_json_value_by_name(&jp_config,"disable_reads", tmp_str, MAX_ID_LEN,&type) ) 
			conf->disable_read_timeout =strtol(tmp_str,NULL,10);

	conf->disable_nanoseconds = GLB_DEFAULT_CLICKHOUSE_DISABLE_NANOSECONDS;

	if (SUCCEED ==zbx_json_value_by_name(&jp_config,"disable_ns", tmp_str, MAX_ID_LEN, &type) ) 
			conf->disable_nanoseconds = strtol(tmp_str,NULL,10);
	
	conf->disable_host_item_names = GLB_DEFAULT_CLICKHOUSE_DISABLE_HOST_ITEMS_NAMES;
	if (SUCCEED ==zbx_json_value_by_name(&jp_config,"disable_host_item_names", tmp_str, MAX_ID_LEN,&type) ) 
			conf->disable_host_item_names=strtol(tmp_str,NULL,10);

	if (0 != curl_global_init(CURL_GLOBAL_ALL)) {
		zabbix_log(LOG_LEVEL_INFORMATION,"Cannot initialize cURL library");
		return FAIL;
	}

	//now, need to setup callbacks for the functions we're involved
	glb_register_callback(GLB_MODULE_API_DESTROY,(void (*)(void))clickhouse_destroy,conf);
	
	return SUCCEED;
}

#else

int	zbx_history_clickhouse_init(zbx_history_iface_t *hist, unsigned char value_type, char **error)
{
	ZBX_UNUSED(hist);
	ZBX_UNUSED(value_type);

	*error = zbx_strdup(*error, "cURL library support >= 7.28.0 is required for clickhouse history backend");
	return FAIL;
}

#endif
