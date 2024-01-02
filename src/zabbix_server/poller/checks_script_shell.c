/*
** Zabbix
** Copyright (C) 2001-2023 Glaber
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

#include "checks_script_shell.h"

#include "log.h"
#include "zbxexec.h"
#include "zbxsysinfo.h"
#include "zbxcrypto.h"
#include "../../libs/zbxsysinfo/sysinfo.h"


static void str2file_name_part(const char *str, char *dst_str, size_t max_dst_len)
{
	md5_state_t	state;
	md5_byte_t	md5[ZBX_MD5_DIGEST_SIZE];
	size_t		str_sz;

	str_sz = strlen(str);
	zbx_md5_init(&state);
	zbx_md5_append(&state, (const md5_byte_t *)str, (int)str_sz);
	zbx_md5_finish(&state, md5);
	zbx_md5buf2str(md5, dst_str);
}

extern char *CONFIG_SHELLSCRIPTS_DIR;
extern int CONFIG_SHELLSCRIPT_MAXSIZE;

static int check_script(u_int64_t itemid, const char *fullpath, char *script, char* error, size_t err_len) {

    int fd ;
    int len = strlen(ZBX_NULL2EMPTY_STR(script)), wr_len;
    
    DEBUG_ITEM(itemid, "Checking the script script len is %d", len);

    if ( 0 == len) {
        zbx_snprintf(error, err_len, "Script has zero length, nothing to execute");
        return FAIL;
    }

    if (CONFIG_SHELLSCRIPT_MAXSIZE > len) {    
        zbx_snprintf(error, err_len, "Script exceeds maximum allowed size of %d bytes", CONFIG_SHELLSCRIPT_MAXSIZE);
        return FAIL;
    }
        
    if (-1 != access(fullpath, X_OK))
        return SUCCEED;
    
    DEBUG_ITEM(itemid, "File '%s' doesn't exists or not executable, recreating", fullpath);
    
    if (-1 == (fd = open(fullpath,O_CREAT | O_WRONLY, S_IRWXU))) {
        zbx_snprintf(error, err_len, "Cannot open file '%s' for writing", fullpath);
        return FAIL;
    }

    //NOTE: for scripts we need to get rid of \r symbol in the interpretator name line (first line), so replace it with a newline
    char *pos;
    if (NULL != (pos= strchr(script, '\r'))) 
        pos[0] = '\n';


    if (len != (wr_len = write(fd , script, len))) {
        close(fd);
        DEBUG_ITEM(itemid, "Writing the script failed");
        zbx_snprintf(error,err_len, "Cannot write script to file: tried %d bytes, but only writted %d bytes", len, wr_len);
        unlink(fullpath);

        return FAIL;
    }
    
    DEBUG_ITEM(itemid, "Written the script succesifully");
    close(fd);
    return SUCCEED;    
}
   
int parse_script_params(const char *params_json, char *full_path, char *error, size_t err_len) {
    const char		*p;
	struct zbx_json_parse	jp;
	char			buf[MAX_STRING_LEN], *cmd = NULL;
	size_t			buf_alloc = 0;
    size_t          path_len = strlen(ZBX_NULL2EMPTY_STR(full_path));

    if (NULL == params_json || 0 == strlen(params_json))
        return SUCCEED;
       
    if (SUCCEED != zbx_json_open(params_json, &jp))
	{
		zbx_snprintf(error, err_len, "Cannot parse script parameters: %s", zbx_json_strerror());
		return FAIL;
	}

	for (p = NULL; NULL != (p = zbx_json_next_value(&jp, p, buf, sizeof(buf), NULL));)
	{
		char	*param_esc, *value_esc;
        char    value[MAX_STRING_LEN];
        size_t value_alloc = 0;
        zbx_json_type_t type;

		param_esc = zbx_dyn_escape_shell_single_quote(buf);
        
        if ( SUCCEED == zbx_json_value_by_name(&jp, param_esc, value, MAX_STRING_LEN, &type) &&  strlen(value) > 0 ) {
            
            value_esc = zbx_dyn_escape_shell_single_quote(value);
    		path_len += zbx_snprintf(full_path + path_len, MAX_STRING_LEN - path_len, " '%s=%s'", param_esc, value_esc);
            zbx_free(value_esc);
        }
        else 
           path_len += zbx_snprintf(full_path + path_len, MAX_STRING_LEN - path_len, " '%s'", param_esc);

		zbx_free(param_esc);
	}
  
    return SUCCEED;
}

int	get_value_shell_script(const DC_ITEM *item, AGENT_RESULT *result)  {
    char name_md5[MAX_STRING_LEN]; 
    char fullpath[MAX_STRING_LEN];
    char *params = NULL, *buf = NULL;
    
    char error[MAX_STRING_LEN];
    int timeout_seconds;

    str2file_name_part(item->params, name_md5, sizeof(name_md5));
    
    if (NULL == CONFIG_SHELLSCRIPTS_DIR) {
        SET_MSG_RESULT(result, strdup("'ShellScriptsDir' is not set, please set shell scripts location in the config file")); //no need to free after this
        DEBUG_ITEM(item->itemid, "Script write / check failed, returning error '%s'", result->msg);
        return NOTSUPPORTED;
    }
        
    zbx_snprintf(fullpath, MAX_STRING_LEN, "%s/%s", CONFIG_SHELLSCRIPTS_DIR, name_md5);
    
    DEBUG_ITEM(item->itemid, "Full script path is '%s'", fullpath);

    if (FAIL == check_script(item->itemid, fullpath, item->params, error, sizeof(error))) {
        DEBUG_ITEM(item->itemid, "Script check failed: '%s'", error);
        SET_MSG_RESULT(result, error);
        return NOTSUPPORTED;
    }

    DEBUG_ITEM(item->itemid, "Script check is succesifull");

    if (FAIL == parse_script_params(item->script_params, fullpath, error, sizeof(error))) {
        SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Script params parsing failed: '%s'", error)); //no need to free after this
        DEBUG_ITEM(item->itemid, "Script params parsing failed, returning error '%s'", result->msg);
        return NOTSUPPORTED;
    }

    if (FAIL == zbx_is_time_suffix(item->timeout, &timeout_seconds, strlen(item->timeout)))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Invalid timeout: %s", item->timeout));
		return NOTSUPPORTED;
	}

    if (0 == timeout_seconds)
        timeout_seconds = sysinfo_get_config_timeout();

	if (SUCCEED != zbx_execute(fullpath, &buf, error, sizeof(error), timeout_seconds, ZBX_EXIT_CODE_CHECKS_DISABLED, NULL)) {
        DEBUG_ITEM(item->itemid, "Execution failed");
        SET_MSG_RESULT(result, zbx_strdup(NULL, error));
        return NOTSUPPORTED;
    }
    
    zbx_rtrim(buf, ZBX_WHITESPACE);
    zbx_set_agent_result_type(result, ITEM_VALUE_TYPE_TEXT, buf);
    DEBUG_ITEM(item->itemid, "Result of the script execution is '%s'",buf);
    zbx_free(buf);

    return SUCCEED;
}

void HouseKeepScripts() {
    
    RUN_ONCE_IN(60);
    char cmd[MAX_STRING_LEN];
    if (NULL == CONFIG_SHELLSCRIPTS_DIR || 0 == strlen(CONFIG_SHELLSCRIPTS_DIR))
        return;

    zbx_snprintf(cmd, sizeof(cmd), "rm %s/*", CONFIG_SHELLSCRIPTS_DIR);
    int ret = system(cmd);
}