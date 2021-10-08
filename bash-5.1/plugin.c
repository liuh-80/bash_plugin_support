/* plugin.c -- Bash plugin support. */

/* Copyright (C) 1987-2016 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#if !defined (__GNUC__) && !defined (HAVE_ALLOCA_H) && defined (_AIX)
  #pragma alloca
#endif /* _AIX && RISC6000 && !__GNUC__ */

// disable bash memory management when build for UT, PTR_T defined in xmalloc.h, define here for disable warning
#if defined (BASH_PLUGIN_UT)
#        define _XMALLOC_H_
#        define PTR_T	void *
#        define malloc	mock_malloc
#        define free	mock_free
#else
#endif

#include <stdio.h>
#include <dlfcn.h>
#include "chartypes.h"
#include "bashtypes.h"
#if !defined (_MINIX) && defined (HAVE_SYS_FILE_H)
#  include <sys/file.h>
#endif
#include "filecntl.h"
#include "posixstat.h"
#include <signal.h>
#include <syslog.h>
#if defined (HAVE_SYS_PARAM_H)
#  include <sys/param.h>
#endif

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include "posixtime.h"

#if defined (HAVE_SYS_RESOURCE_H) && !defined (RLIMTYPE)
#  include <sys/resource.h>
#endif

#if defined (HAVE_SYS_TIMES_H) && defined (HAVE_TIMES)
#  include <sys/times.h>
#endif

#include <errno.h>

#if !defined (errno)
extern int errno;
#endif

#define NEED_FPURGE_DECL
#define NEED_SH_SETLINEBUF_DECL

#include "bashansi.h"
#include "bashintl.h"

#include "shell.h"
#include <y.tab.h>    /* use <...> so we pick it up from the build directory */
#include "error.h"
#include "flags.h"
#include "builtins.h"
#include "hashlib.h"
#include "jobs.h"
#include "execute_cmd.h"
#include "findcmd.h"
#include "redir.h"
#include "trap.h"
#include "pathexp.h"
#include "hashcmd.h"


#if defined (BASH_SHELL_EXECVE_PLUGIN)
#include "plugin.h"
#endif /* BASH_SHELL_EXECVE_PLUGIN */

#if defined (COND_COMMAND)
#  include "test.h"
#endif

#include "builtins/common.h"

#include "builtins/getopt.h"

#include <glob/strmatch.h>
#include <tilde/tilde.h>

#if defined (BUFFERED_INPUT)
#  include "input.h"
#endif

#if defined (ALIAS)
#  include "alias.h"
#endif

#if defined (HISTORY)
#  include "bashhist.h"
#endif

#if defined (HAVE_MBSTR_H) && defined (HAVE_MBSCHR)
#  include <mbstr.h>        /* mbschr */
#endif

/* configuration buffer size */
#define CONFIG_BUFFER_SIZE 256

/* configuration plugin setting key */
#define CONFIG_PLUGIN_SETTING_KEY "plugin="

/* configuration termination characters */
#define CONFIG_SETTING_TERMINATIONS " \t\n\r\f"

/* configuration comments start character */
#define CONFIG_SETTING_COMMENTS_START_CHAR '#'

/* plugin configration file */
const char *plugin_config_file = "/etc/bash_plugins.conf";

/* plugin on_shell_execve function name */
static const char *on_shell_execve_function_name = "on_shell_execve";

/* plugin plugin_init function name */
static const char *plugin_init_function_name = "plugin_init";

/* plugin plugin_uninit function name */
static const char *plugin_uninit_function_name = "plugin_uninit";

/* plugin handle for test */
PLUGIN_NODE *global_plugin_list = NULL;

/* Load plugin by plugin path */
int
append_plugin(
        plugin_handle,
        on_shell_execve,
        plugin_init,
        plugin_uninit)
     void *plugin_handle;
     on_shell_execve_t *on_shell_execve;
     plugin_init_t *plugin_init;
     plugin_uninit_t *plugin_uninit;
{
    /* Create and initialize new plugin */
    PLUGIN_NODE *new_plugin_node = (PLUGIN_NODE*)malloc(sizeof(PLUGIN_NODE));
    if (new_plugin_node == NULL)
    {
        /* When allocate memory failed, stop and return. also output log to both syslog and stderr with LOG_PERROR*/
        syslog(LOG_PERROR, "Plugin: failed to allocate memory for plugin node.\n");
        return PLUGIN_LOAD_FAILED;
    }

    new_plugin_node->next = NULL;
    new_plugin_node->plugin_handle = plugin_handle;
    new_plugin_node->on_shell_execve = on_shell_execve;
    new_plugin_node->plugin_init = plugin_init;
    new_plugin_node->plugin_uninit = plugin_uninit;

#ifdef DEBUG
    itrace("Plugin: append plugin node %p to global list %p\n", new_plugin_node, global_plugin_list);
#endif
    
    /* Find the pointer to the latest plugin node's 'next' field */
    PLUGIN_NODE **current_plugin_node = &global_plugin_list;
    while (*current_plugin_node != NULL) {
        current_plugin_node = &((*current_plugin_node)->next);

#ifdef DEBUG
        itrace("Plugin: founded next plugin node: %p\n", *current_plugin_node);
#endif
    }
    
    /* append new plugin to tail node */
    *current_plugin_node = new_plugin_node;

#ifdef DEBUG
    itrace("Plugin: append new plugin node %p to %p\n", new_plugin_node, current_plugin_node);
#endif

    return PLUGIN_LOAD_SUCCESS;
}


/* Load plugin by plugin path */
void
try_load_plugin_by_path(plugin_path)
     const char *plugin_path;
{
    /* Plugin handle */
    void *plugin_handle;
    if ( (plugin_handle = dlopen(plugin_path, RTLD_LAZY)) == NULL) {
#ifdef DEBUG
        itrace("Plugin: can't load plugin %s: %s\n", plugin_path, dlerror());
#endif
        return;
    }

    /* Check if plugin support shell execve method */
    on_shell_execve_t* plugin_on_shell_execve_handle = dlsym(plugin_handle, on_shell_execve_function_name);
    if (dlerror() != NULL) {
        dlclose(plugin_handle);

#ifdef DEBUG
        itrace("Plugin: can't find on_shell_execve function %s: %s\n", plugin_path, dlerror());
#endif
        return;
    }
    

    /* Check if plugin support un-initialization method */
    plugin_uninit_t* plugin_uninit_handle = dlsym(plugin_handle, plugin_uninit_function_name);
    if (dlerror() != NULL) {
        dlclose(plugin_handle);

#ifdef DEBUG
        itrace("Plugin: can't find plugin_uninit function %s: %s\n", plugin_path, dlerror());
#endif
        return;
    }

    /* Check if plugin support initialization method */
    plugin_init_t* plugin_init_handle = dlsym(plugin_handle, plugin_init_function_name);
    if (dlerror() != NULL) {
        dlclose(plugin_handle);

#ifdef DEBUG
        itrace("Plugin: can't find plugin_init function %s: %s\n", plugin_path, dlerror());
#endif
        return;
    }
    else {
        /* Initialize plugin */
        plugin_init_handle();
    }

    /* Add plugin to plugin list */
    int plugin_load_result = append_plugin(plugin_handle,
                    plugin_on_shell_execve_handle,
                    plugin_init_handle,
                    plugin_uninit_handle);

    if (plugin_load_result == PLUGIN_LOAD_SUCCESS) {
#ifdef DEBUG
        itrace("Plugin: plugin %s loaded\n", plugin_path);
#endif
    }
    else {
        /* Output plugin load error message, also output log to both syslog and stderr with LOG_PERROR*/
        syslog(LOG_PERROR,"Plugin: plugin %s load failed, result: %d\n", plugin_path, plugin_load_result);
    }
}

/* Load plugin by config file */
void
load_plugin_by_config(config_filename)
     const char *config_filename;
{
    FILE *config_file;
    char buffer[CONFIG_BUFFER_SIZE];

    config_file = fopen(config_filename, "r");
    if(config_file == NULL) {
#ifdef DEBUG
            itrace("Plugin: can't open plugin config file %s: %s\n", config_filename, strerror(errno));
#endif
        return;
    }

    while(fgets(buffer, sizeof buffer, config_file)) {
        if(*buffer == CONFIG_SETTING_COMMENTS_START_CHAR || isspace(*buffer)) {
            /* ignore comments or white space. */
            continue; 
        }
        
        /* read to first whitespace. */
        strtok(buffer, CONFIG_SETTING_TERMINATIONS); 

        if(!strncmp(buffer, CONFIG_PLUGIN_SETTING_KEY, strlen(CONFIG_PLUGIN_SETTING_KEY))) {
            /* read plugin path. */
            char* plugin_path = strtok(buffer + strlen(CONFIG_PLUGIN_SETTING_KEY), CONFIG_SETTING_TERMINATIONS); 
#ifdef DEBUG
            itrace("Plugin: load plugin: %s\n", plugin_path);
#endif
            try_load_plugin_by_path(plugin_path);
        }
#ifdef DEBUG
        else {
            /* output debug message. */
            itrace("Plugin: unrecognized parameter: %s\n", buffer);
        }
#endif
    }

    fclose(config_file);
}

/* Free loaded plugins */
void
free_loaded_plugins()
{
    if (global_plugin_list == NULL) {
        return;
    }
	
#ifdef DEBUG
    itrace("Plugin: start free plugin from global list %p\n", global_plugin_list);
#endif

    /* Walk to last plugin */
    PLUGIN_NODE *next_plugin_node = global_plugin_list;
    while (next_plugin_node != NULL) {

        /* Unload plugin */
        next_plugin_node->plugin_uninit();
        dlclose(next_plugin_node->plugin_handle);

        /* Continue with next pligin */
		PLUGIN_NODE* current_plugin_node_memory = next_plugin_node;
        next_plugin_node = next_plugin_node->next;

#ifdef DEBUG
		itrace("Plugin: next plugin address %p\n", next_plugin_node);
#endif

		/* Free plugin node memory, this may also reset all allocated memory depends on c lib implementation */
		free(current_plugin_node_memory);
    }

    /* Reset plugin list */
	global_plugin_list = NULL;
}

/* Invoke loaded plugins */
int
invoke_loaded_plugins (user, shell_level, cmd, argv)
     char *user;
     int shell_level;
     char *cmd;
     char **argv;
{
    if (global_plugin_list == NULL) {
        return 0;
    }
	
#ifdef DEBUG
    itrace("Plugin: start invoke plugin from global list %p\n", global_plugin_list);
#endif

    /* Walk to last plugin */
    PLUGIN_NODE *next_plugin_node = global_plugin_list;
    while (next_plugin_node != NULL) {

        /* Call plugin method */
        int plugin_error_code = next_plugin_node->on_shell_execve(user, shell_level, cmd, argv);
        if (plugin_error_code != 0) {
#ifdef DEBUG
            itrace("Plugin: on_execve return error: %d\n", plugin_error_code);
#endif
            /* Exit when plugin failed */
            return plugin_error_code;
        }
        
        /* Continue with next pligin */
        next_plugin_node = next_plugin_node->next;
		
#ifdef DEBUG
		itrace("Plugin: next plugin address %p\n", next_plugin_node);
#endif
    }
    
    return 0;
}

/* Load all plugins。 */
void
load_plugins ()
{
    load_plugin_by_config(plugin_config_file);
}

/* Free all plugins */
void
free_plugins ()
{
    free_loaded_plugins();
}

/* Invoke plugins before shell execve */
int
invoke_plugin_on_shell_execve (user, cmd, argv)
     char *user;
     char *cmd;
     char **argv;
{
    const char* shell_level_str = get_string_value ("SHLVL");
    const int shell_level = atoi (shell_level_str);

    if (absolute_program (cmd)) {
        // find real path for relative path command
        char resolved_path[PATH_MAX];

        // real_path_buffer should not free here because we pass resolved_path as parameter.
        char* real_path_buffer = realpath(cmd, resolved_path);

        return invoke_loaded_plugins(user, shell_level, resolved_path, argv);
    }
    else {
        return invoke_loaded_plugins(user, shell_level, cmd, argv);
    }
}
