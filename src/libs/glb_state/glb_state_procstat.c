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

#include "glb_state.h"
#include "zbxjson.h"
#include "../../zabbix_server/glb_poller/internal.h"

#define MAX_PROCSTAT_NAME_SIZE 64

typedef struct {
    int server_num;
    int proc_num;
    char proc_name[MAX_PROCSTAT_NAME_SIZE];
    
    size_t heap_usage;
} procstat_t;

typedef struct {
    elems_hash_t *stats; //problems refernced by their ids
    mem_funcs_t memf;
    //strpool_t strpool;

} procstat_conf_t;

static procstat_conf_t *conf = NULL; 

ELEMS_CREATE(stat_create_cb) {
    elem->data = memf->malloc_func(NULL, sizeof(procstat_t));
    procstat_t* stat = elem->data;
    
    bzero(stat, sizeof(procstat_t));
}

ELEMS_FREE(stat_free_cb) {
    procstat_t *stat = elem->data;
        
    memf->free_func(stat);
    elem->data = NULL;
}


ELEMS_CALLBACK(add_procs_stat_cb) {
    struct zbx_json *j = data;
    char procid[MAX_PROCSTAT_NAME_SIZE+6];
    procstat_t *stat = elem->data;
    char *ptr;

    zbx_snprintf(procid, MAX_PROCSTAT_NAME_SIZE, "%s %d", stat->proc_name, stat->proc_num);
    
    while (NULL !=(ptr = strstr(procid, " ")))
        *ptr = '_';

    zbx_json_addobject(j, NULL);

    zbx_json_addstring(j, "procid", procid, ZBX_JSON_TYPE_STRING);
    zbx_json_adduint64string(j, "heap_usage", stat->heap_usage);
    zbx_json_addstring(j, "process_name", stat->proc_name, ZBX_JSON_TYPE_STRING);
    zbx_json_adduint64string(j, "process_num", stat->proc_num);
    zbx_json_adduint64string(j, "server_num", elem->id);

    zbx_json_close(j);
}

INTERNAL_METRIC_CALLBACK(procs_stat_cb) {
    size_t alloc = 0, offset = 0;
    struct zbx_json j;

    zbx_json_init(&j, 16384);
    zbx_json_addarray(&j, NULL);

    elems_hash_iterate(conf->stats, add_procs_stat_cb, &j, 0);
    zbx_json_close(&j);

    //NOTE: need to strip outer {} that zbx_json_init adds by default
    j.buffer[j.buffer_offset] = 0;
    *result = zbx_strdup(NULL, j.buffer + 1 );

    zbx_json_free(&j);

    return SUCCEED;
}
    

int glb_state_procstat_init(mem_funcs_t *memf)
{
    if (NULL == (conf = memf->malloc_func(NULL, sizeof(procstat_conf_t)))) {
        LOG_WRN("Cannot allocate memory for procstat cache struct");
        exit(-1);
    };
    
    conf->stats = elems_hash_init(memf, stat_create_cb, stat_free_cb );
    conf->memf = *memf;

    glb_register_internal_metric_handler("proc_stat",  procs_stat_cb);
    
    return SUCCEED;
}

int glb_state_procstat_destroy() {
    elems_hash_destroy(conf->stats);
}

static size_t getHeapUsage( )
{
    long rss = 0L;
    FILE* fp = NULL;
    
    if ( (fp = fopen( "/proc/self/statm", "r" )) == NULL )
        return (size_t)0L;      /* Can't open? */

    if ( fscanf( fp, "%*s%ld", &rss ) != 1 )
    {
        fclose( fp );
        return (size_t)0L;      /* Can't read? */
    }
    
    fclose( fp );
    return (size_t)rss * (size_t)sysconf( _SC_PAGESIZE);
}

ELEMS_CALLBACK(procstat_update_cb) {
    procstat_t *stat = elem->data, *newstat = data;
    
    *stat = *newstat;
   // LOG_INF("Reported %s%d proc heap size is %"PRIu64,newstat->proc_name, newstat->proc_num, newstat->heap_usage);
}

void glb_state_procstat_update(int server_num, int proc_num, const char* proc_name) {
    procstat_t newstat = {.server_num = server_num, .proc_num = proc_num};
    
    newstat.heap_usage = getHeapUsage();

    zbx_snprintf(newstat.proc_name, MAX_PROCSTAT_NAME_SIZE, "%s", proc_name);
    elems_hash_process(conf->stats, server_num, procstat_update_cb, &newstat, 0);
}
