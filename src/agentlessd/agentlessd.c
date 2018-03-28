/* Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#include "os_crypto/md5/md5_op.h"
#include "agentlessd.h"

/* Prototypes */
static int  save_agentless_entry(const char *host, const char *script, const char *agttype);
static int  send_intcheck_msg(const char *script, const char *host, const char *msg);
static int  send_log_msg(const char *script, const char *host, const char *msg);
static int  gen_diff_alert(const char *host, const char *script, time_t alert_diff_time);
static int  check_diff_file(const char *host, const char *script);
static FILE *open_diff_file(const char *host, const char *script);
static int  run_periodic_cmd(agentlessd_entries *entry, int test_it);

/* Global variables */
agentlessd_config lessdc;

static const char *STR_MORE_CHANGES = "More changes...";


/* Save agentless entry for the control tools to gather */
static int save_agentless_entry(const char *host, const char *script, const char *agttype)
{
    FILE *fp;
    char sys_location[1024 + 1];

    sys_location[1024] = '\0';
    snprintf(sys_location, 1024, "%s/(%s) %s",
             AGENTLESS_ENTRYDIRPATH, script, host);

    fp = fopen(sys_location, "w");
    if (fp) {
        fprintf(fp, "type: %s\n", agttype);
        fclose(fp);
    } else {
        merror(FOPEN_ERROR, sys_location, errno, strerror(errno));
    }

    return (0);
}

/* Send integrity checking message */
static int send_intcheck_msg(const char *script, const char *host, const char *msg)
{
    char sys_location[1024 + 1];

    sys_location[1024] = '\0';
    snprintf(sys_location, 1024, "(%s) %s->%s", script, host, SYSCHECK);

    if (SendMSG(lessdc.queue, msg, sys_location, SYSCHECK_MQ) < 0) {
        merror(QUEUE_SEND);

        if ((lessdc.queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
            merror_exit(QUEUE_FATAL, DEFAULTQPATH);
        }

        /* If we reach here, we can try to send it again */
        SendMSG(lessdc.queue, msg, sys_location, SYSCHECK_MQ);
    }

    return (0);
}

/* Send generic log message */
static int send_log_msg(const char *script, const char *host, const char *msg)
{
    char sys_location[1024 + 1];

    sys_location[1024] = '\0';
    snprintf(sys_location, 1024, "(%s) %s->%s", script, host, SYSCHECK);

    if (SendMSG(lessdc.queue, msg, sys_location, LOCALFILE_MQ) < 0) {
        merror(QUEUE_SEND);
        if ((lessdc.queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
            merror_exit(QUEUE_FATAL, DEFAULTQPATH);
        }

        /* If we reach here, we can try to send it again */
        SendMSG(lessdc.queue, msg, sys_location, LOCALFILE_MQ);
    }
    return (0);
}

/* Generate diffs alert */
static int gen_diff_alert(const char *host, const char *script, time_t alert_diff_time)
{
    size_t n;
    FILE *fp;
    char buf[2048 + 1];
    char diff_alert[4096 + 1];

    buf[2048] = '\0';
    diff_alert[4096] = '\0';

    snprintf(buf, 2048, "%s/%s->%s/diff.%d",
             DIFF_DIR_PATH, host, script, (int)alert_diff_time);

    fp = fopen(buf, "r");
    if (!fp) {
        merror("Unable to generate diff alert.");
        return (0);
    }

    n = fread(buf, 1, 2048 - 1, fp);

    switch (n) {
    case 0:
        merror("Unable to generate diff alert (fread).");
        fclose(fp);
        return (0);
    case 2047:
        n -= strlen(STR_MORE_CHANGES);

        while (n > 0 && buf[n - 1] != '\n')
            n--;

        strcpy(buf + n, STR_MORE_CHANGES);
        break;
    default:
        buf[n] = '\0';
    }

    /* Create alert */
    snprintf(diff_alert, 4096 - 1, "ossec: agentless: Change detected:\n%s", buf);
    snprintf(buf, 1024, "(%s) %s->agentless", script, host);

    if (SendMSG(lessdc.queue, diff_alert, buf, LOCALFILE_MQ) < 0) {
        merror(QUEUE_SEND);

        if ((lessdc.queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
            merror_exit(QUEUE_FATAL, DEFAULTQPATH);
        }

        /* If we reach here, we can try to send it again */
        SendMSG(lessdc.queue, diff_alert, buf, LOCALFILE_MQ);
    }

    save_agentless_entry(host, script, "diff");

    fclose(fp);
    return (0);
}

/* Check if the file has changed */
static int check_diff_file(const char *host, const char *script)
{
    time_t date_of_change;
    char old_location[1024 + 1];
    char new_location[1024 + 1];
    char tmp_location[1024 + 1];
    char diff_cmd[2048 + 1];

    os_md5 md5sum_old;
    os_md5 md5sum_new;

    old_location[1024] = '\0';
    new_location[1024] = '\0';
    tmp_location[1024] = '\0';
    diff_cmd[2048] = '\0';

    snprintf(new_location, 1024, "%s/%s->%s/%s", DIFF_DIR_PATH, host, script,
             DIFF_NEW_FILE);
    snprintf(old_location, 1024, "%s/%s->%s/%s", DIFF_DIR_PATH, host, script,
             DIFF_LAST_FILE);

    /* If the file is not there, rename new location to last location */
    if (OS_MD5_File(old_location, md5sum_old, OS_TEXT) != 0) {
        if (rename(new_location, old_location) != 0) {
            merror(RENAME_ERROR, new_location, old_location, errno, strerror(errno));
        }
        return (0);
    }

    /* Get md5sum of the new file */
    if (OS_MD5_File(new_location, md5sum_new, OS_TEXT) != 0) {
        merror("Invalid internal state (missing '%s').", new_location);
        return (0);
    }

    /* If they match, keep the old file and remove the new */
    if (strcmp(md5sum_new, md5sum_old) == 0) {
        unlink(new_location);
        return (0);
    }

    /* Save the old file at timestamp and rename new to last */
    date_of_change = File_DateofChange(old_location);
    snprintf(tmp_location, 1024, "%s/%s->%s/state.%d", DIFF_DIR_PATH, host, script,
             (int)date_of_change);

    if (rename(old_location, tmp_location) != 0) {
        merror(RENAME_ERROR, old_location, tmp_location, errno, strerror(errno));
        return (0);
    }
    if (rename(new_location, old_location) != 0) {
        merror(RENAME_ERROR, new_location, old_location, errno, strerror(errno));
        return (0);
    }

    /* Run diff */
    date_of_change = File_DateofChange(old_location);
    snprintf(diff_cmd, 2048, "diff \"%s\" \"%s\" > \"%s/%s->%s/diff.%d\" "
             "2>/dev/null",
             tmp_location, old_location,
             DIFF_DIR_PATH, host, script, (int)date_of_change);
    if (system(diff_cmd) != 256) {
        merror("Unable to run diff for %s->%s",
               host, script);
        return (0);
    }

    /* Generate alert */
    gen_diff_alert(host, script, date_of_change);

    return (0);
}

/* Get the diff file */
static FILE *open_diff_file(const char *host, const char *script)
{
    FILE *fp = NULL;
    char sys_location[1024 + 1];

    sys_location[1024] = '\0';
    snprintf(sys_location, 1024, "%s/%s->%s/%s", DIFF_DIR_PATH, host, script,
             DIFF_NEW_FILE);

    fp = fopen(sys_location, "w");

    /* If we can't open, try creating the directory */
    if (!fp) {
        snprintf(sys_location, 1024, "%s/%s->%s", DIFF_DIR_PATH, host, script);
        if (IsDir(sys_location) == -1) {
            if (mkdir(sys_location, 0770) == -1) {
                merror(MKDIR_ERROR, sys_location, errno, strerror(errno));
                return (NULL);
            }
        }

        snprintf(sys_location, 1024, "%s/%s->%s/%s", DIFF_DIR_PATH, host,
                 script, DIFF_NEW_FILE);
        fp = fopen(sys_location, "w");
        if (!fp) {
            merror(FOPEN_ERROR, sys_location, errno, strerror(errno));
            return (NULL);
        }
    }

    return (fp);
}

/* Run periodic commands */
static int run_periodic_cmd(agentlessd_entries *entry, int test_it)
{
    int i = 0;
    char *tmp_str;
    char buf[OS_SIZE_2048 + 1];
    char command[OS_SIZE_1024 + 1];
    FILE *fp;
    FILE *fp_store = NULL;

    buf[0] = '\0';
    command[0] = '\0';
    command[OS_SIZE_1024] = '\0';

    while (entry->server[i]) {
        /* Ignored entry */
        if (entry->server[i][0] == '\0') {
            i++;
            continue;
        }

        /* We only test for the first server entry */
        else if (test_it) {
            int ret_code = 0;
            snprintf(command, OS_SIZE_1024,
                     "%s/%s test test >/dev/null 2>&1",
                     AGENTLESSDIRPATH, entry->type);
            ret_code = system(command);

            /* Check if the test worked */
            if (ret_code != 0) {
                if (ret_code == 32512) {
                    merror("Expect command not found (or bad "
                           "arguments) for '%s'.",
                           entry->type);
                }
                merror("Test failed for '%s' (%d). Ignoring.",
                       entry->type, ret_code / 256);
                entry->error_flag = 99;
                return (-1);
            }

            minfo("Test passed for '%s'.", entry->type);
            return (0);
        }

        if (entry->server[i][0] == 's') {
            snprintf(command, OS_SIZE_1024, "%s/%s \"use_su\" \"%s\" %s 2>&1",
                     AGENTLESSDIRPATH, entry->type, entry->server[i] + 1,
                     entry->options);
        } else if (entry->server[i][0] == 'o') {
            snprintf(command, OS_SIZE_1024, "%s/%s \"use_sudo\" \"%s\" %s 2>&1",
                     AGENTLESSDIRPATH, entry->type, entry->server[i] + 1,
                     entry->options);
        } else {
            snprintf(command, OS_SIZE_1024, "%s/%s \"%s\" %s 2>&1",
                     AGENTLESSDIRPATH, entry->type, entry->server[i] + 1,
                     entry->options);
        }

        fp = popen(command, "r");
        if (fp) {
            while (fgets(buf, OS_SIZE_2048, fp) != NULL) {
                /* Remove newlines and carriage returns */
                tmp_str = strchr(buf, '\r');
                if (tmp_str) {
                    *tmp_str = '\0';
                }
                tmp_str = strchr(buf, '\n');
                if (tmp_str) {
                    *tmp_str = '\0';
                }

                if (strncmp(buf, "ERROR: ", 7) == 0) {
                    merror("%s: %s: %s",
                           entry->type, entry->server[i] + 1, buf + 7);
                    entry->error_flag++;
                    break;
                } else if (strncmp(buf, "INFO: ", 6) == 0) {
                    minfo("%s: %s: %s",
                            entry->type, entry->server[i] + 1, buf + 6);
                } else if (strncmp(buf, "FWD: ", 4) == 0) {
                    tmp_str = buf + 5;
                    send_intcheck_msg(entry->type, entry->server[i] + 1,
                                      tmp_str);
                } else if (strncmp(buf, "LOG: ", 4) == 0) {
                    tmp_str = buf + 5;
                    send_log_msg(entry->type, entry->server[i] + 1,
                                 tmp_str);
                } else if ((entry->state & LESSD_STATE_DIFF) &&
                           (strncmp(buf, "STORE: ", 7) == 0)) {
                    if (fp_store) {
                        fclose(fp_store);
                    }
                    fp_store = open_diff_file(entry->server[i] + 1,
                                              entry->type);
                } else if (fp_store) {
                    fprintf(fp_store, "%s\n", buf);
                } else {
                    mdebug1("Buffer: %s", buf);
                }
            }

            if (fp_store) {
                fclose(fp_store);
                fp_store = NULL;

                check_diff_file(entry->server[i] + 1, entry->type);
            } else {
                save_agentless_entry(entry->server[i] + 1,
                                     entry->type, "syscheck");
            }
            pclose(fp);
        } else {
            merror("Popen failed on '%s' for '%s'.",
                   entry->type, entry->server[i] + 1);
            entry->error_flag++;
        }

        i++;
    }

    return (0);
}

/* Main agentlessd */
void Agentlessd()
{
    time_t tm;
    struct tm *p;

    int today = 0;
    int test_it = 1;

    char str[OS_SIZE_1024 + 1];

    /* Wait a few seconds to settle */
    sleep(2);
    memset(str, '\0', OS_SIZE_1024 + 1);

    /* Get current time before starting */
    tm = time(NULL);
    p = localtime(&tm);

    today = p->tm_mday;

    /* Connect to the message queue. Exit if it fails. */
    if ((lessdc.queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
        merror_exit(QUEUE_FATAL, DEFAULTQUEUE);
    }

    // Start com request thread
    w_create_thread(lessdcom_main, NULL);

    /* Main monitor loop */
    while (1) {
        unsigned int i = 0;
        tm = time(NULL);
        p = localtime(&tm);

        /* Day changed, deal with log files */
        if (today != p->tm_mday) {
            today = p->tm_mday;
        }

        while (lessdc.entries[i]) {
            if (lessdc.entries[i]->error_flag >= 10) {
                if (lessdc.entries[i]->error_flag != 99) {
                    merror("Too many failures for '%s'. Ignoring it.",
                           lessdc.entries[i]->type);
                    lessdc.entries[i]->error_flag = 99;
                }

                i++;
                sleep(i);
                continue;
            }

            /* Run the check again if the frequency has elapsed */
            if ((lessdc.entries[i]->state & LESSD_STATE_PERIODIC) &&
                    ((lessdc.entries[i]->current_state +
                      lessdc.entries[i]->frequency) < tm)) {
                run_periodic_cmd(lessdc.entries[i], test_it);
                if (!test_it) {
                    lessdc.entries[i]->current_state = tm;
                }
            }

            i++;

            sleep(i);
        }

        /* We only check every minute */
        test_it = 0;
        sleep(60);
    }
}

cJSON *getAgentlessConfig(void) {

    cJSON *root = cJSON_CreateObject();
    cJSON *agent_list = cJSON_CreateArray();
    unsigned int i, j;
    i = 0;
    while (lessdc.entries[i]) {
        cJSON *agent = cJSON_CreateObject();
        cJSON *host_list = cJSON_CreateArray();
        for (j=0;lessdc.entries[i]->server[j];j++) {
            cJSON_AddItemToArray(host_list,cJSON_CreateString(lessdc.entries[i]->server[j]));
        }
        cJSON_AddItemToObject(agent,"host",host_list);
        cJSON_AddNumberToObject(agent,"port",lessdc.entries[i]->port);
        cJSON_AddNumberToObject(agent,"frequency",lessdc.entries[i]->frequency);
        if (lessdc.entries[i]->state & LESSD_STATE_PERIODIC && lessdc.entries[i]->state & LESSD_STATE_DIFF)
            cJSON_AddStringToObject(agent,"state","periodic_diff");
        else if (lessdc.entries[i]->state & LESSD_STATE_CONNECTED) cJSON_AddStringToObject(agent,"state","stay_connected");
        else if (lessdc.entries[i]->state & LESSD_STATE_PERIODIC) cJSON_AddStringToObject(agent,"state","periodic");
        if (lessdc.entries[i]->options) cJSON_AddStringToObject(agent,"arguments",lessdc.entries[i]->options);
        if (lessdc.entries[i]->command) cJSON_AddStringToObject(agent,"run_command",lessdc.entries[i]->command);
        if (lessdc.entries[i]->type) cJSON_AddStringToObject(agent,"type",lessdc.entries[i]->type);
        cJSON_AddItemToArray(agent_list,agent);
        i++;
    }

    cJSON_AddItemToObject(root,"agentless",agent_list);

    return root;
}
