/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2008 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "hydra.h"
#include "pmip.h"
#include "pmip_pmi.h"
#include "ckpoint.h"
#include "demux.h"
#include "topo.h"
#include "hydt_ftb.h"

extern struct HYD_pmcd_pmip HYD_pmcd_pmip;
struct HYD_pmcd_pmip_pmi_handle *HYD_pmcd_pmip_pmi_handle = { 0 };

static int storage_len = 0;
static char storage[HYD_TMPBUF_SIZE], *sptr = storage, r[HYD_TMPBUF_SIZE];
static int using_pmi_port = 0;

static HYD_status stdio_cb(int fd, HYD_event_t events, void *userp)
{
    int closed, i, sent, recvd, stdfd;
    char buf[HYD_TMPBUF_SIZE];
    struct HYD_pmcd_hdr hdr;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    stdfd = (int) (size_t) userp;

    status = HYDU_sock_read(fd, buf, HYD_TMPBUF_SIZE, &recvd, &closed, 0);
    HYDU_ERR_POP(status, "sock read error\n");

    if (recvd) {
        if (stdfd == STDOUT_FILENO) {
            HYD_pmcd_init_header(&hdr);
            hdr.cmd = STDOUT;
            for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
                if (HYD_pmcd_pmip.downstream.out[i] == fd)
                    break;
        }
        else {
            HYD_pmcd_init_header(&hdr);
            hdr.cmd = STDERR;
            for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
                if (HYD_pmcd_pmip.downstream.err[i] == fd)
                    break;
        }

        HYDU_ASSERT(i < HYD_pmcd_pmip.local.proxy_process_count, status);

        hdr.pgid = HYD_pmcd_pmip.local.pgid;
        hdr.proxy_id = HYD_pmcd_pmip.local.id;
        hdr.rank = HYD_pmcd_pmip.downstream.pmi_rank[i];
        hdr.buflen = recvd;

        {
            int upstream_sock_closed;

            status = HYDU_sock_write(HYD_pmcd_pmip.upstream.control, &hdr, sizeof(hdr), &sent,
                                     &upstream_sock_closed);
            HYDU_ERR_POP(status, "sock write error\n");
            HYDU_ASSERT(!upstream_sock_closed, status);

            status = HYDU_sock_write(HYD_pmcd_pmip.upstream.control, buf, recvd, &sent,
                                     &upstream_sock_closed);
            HYDU_ERR_POP(status, "sock write error\n");
            HYDU_ASSERT(!upstream_sock_closed, status);
        }
    }

    if (closed) {
        /* The connection has closed */
        status = HYDT_dmx_deregister_fd(fd);
        HYDU_ERR_POP(status, "unable to deregister fd\n");

        if (stdfd == STDOUT_FILENO) {
            for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
                if (HYD_pmcd_pmip.downstream.out[i] == fd)
                    HYD_pmcd_pmip.downstream.out[i] = HYD_FD_CLOSED;
        }
        else {
            for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
                if (HYD_pmcd_pmip.downstream.err[i] == fd)
                    HYD_pmcd_pmip.downstream.err[i] = HYD_FD_CLOSED;
        }

        close(fd);
    }

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status check_pmi_cmd(char **buf, int *pmi_version, int *repeat)
{
    int full_command, buflen, cmdlen;
    char *bufptr, lenptr[7];
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    *repeat = 0;

    /* We need to read at least 6 bytes before we can decide if this
     * is PMI-1 or PMI-2 */
    if (storage_len < 6)
        goto fn_exit;

    /* FIXME: This should really be a "FIXME" for the client, since
     * there's not much we can do on the server side.
     *
     * We initialize to whatever PMI version we detect while reading
     * the PMI command, instead of relying on what the init command
     * gave us. This part of the code should not know anything about
     * PMI-1 vs. PMI-2. But the simple PMI client-side code in MPICH2
     * is so hacked up, that commands can arrive out-of-order and this
     * is necessary. This was discussed in the group and we felt that
     * it is unsafe to change the order of the PMI command arrival in
     * the client code (even if we are really correcting it), since
     * other PMs might rely on the "incorrect order of commands".
     */

    /* Parse the string and if a full command is found, make sure that
     * bufptr points to the last byte of the command */
    full_command = 0;
    if (!strncmp(sptr, "cmd=", strlen("cmd=")) || !strncmp(sptr, "mcmd=", strlen("mcmd="))) {
        /* PMI-1 format command; read the rest of it */
        *pmi_version = 1;

        if (!strncmp(sptr, "cmd=", strlen("cmd="))) {
            /* A newline marks the end of the command */
            for (bufptr = sptr; bufptr < sptr + storage_len; bufptr++) {
                if (*bufptr == '\n') {
                    full_command = 1;
                    break;
                }
            }
        }
        else {  /* multi commands */
            for (bufptr = sptr; bufptr < sptr + storage_len - strlen("endcmd\n") + 1; bufptr++) {
                if (bufptr[0] == 'e' && bufptr[1] == 'n' && bufptr[2] == 'd' &&
                    bufptr[3] == 'c' && bufptr[4] == 'm' && bufptr[5] == 'd' &&
                    bufptr[6] == '\n') {
                    full_command = 1;
                    bufptr += strlen("endcmd\n") - 1;
                    break;
                }
            }
        }
    }
    else {
        *pmi_version = 2;

        /* We already made sure we had at least 6 bytes */
        memcpy(lenptr, sptr, 6);
        lenptr[6] = 0;
        cmdlen = atoi(lenptr);

        if (storage_len >= cmdlen + 6) {
            full_command = 1;
            bufptr = sptr + 6 + cmdlen - 1;
        }
    }

    if (full_command) {
        /* We have a full command */
        buflen = bufptr - sptr + 1;
        HYDU_MALLOC(*buf, char *, buflen, status);
        memcpy(*buf, sptr, buflen);
        sptr += buflen;
        storage_len -= buflen;
        (*buf)[buflen - 1] = '\0';

        if (storage_len == 0)
            sptr = storage;
        else
            *repeat = 1;
    }
    else {
        /* We don't have a full command. Copy the rest of the data to
         * the front of the storage buffer. */

        /* FIXME: This dual memcpy is crazy and needs to be
         * fixed. Single memcpy should be possible, but we need to be
         * a bit careful not to corrupt the buffer. */
        if (sptr != storage) {
            memcpy(r, sptr, storage_len);
            memcpy(storage, r, storage_len);
            sptr = storage;
        }
        *buf = NULL;
    }

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status pmi_cb(int fd, HYD_event_t events, void *userp)
{
    char *buf = NULL, *pmi_cmd = NULL, *args[HYD_NUM_TMP_STRINGS] = { 0 };
    int closed, repeat, sent, i = -1, linelen, pid = -1;
    struct HYD_pmcd_hdr hdr;
    struct HYD_pmcd_pmip_pmi_handle *h;
    char ftb_event_payload[HYDT_FTB_MAX_PAYLOAD_DATA];
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    HYD_pmcd_init_header(&hdr);

    /* PMI-1 does not tell us how much to read. We read how much ever
     * we can, parse out full PMI commands from it, and process
     * them. When we don't have a full PMI command, we store the
     * rest. */
    status = HYDU_sock_read(fd, storage + storage_len, HYD_TMPBUF_SIZE - storage_len,
                            &linelen, &closed, 0);
    HYDU_ERR_POP(status, "unable to read PMI command\n");

    /* Try to find the PMI FD */
    for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++) {
        if (HYD_pmcd_pmip.downstream.pmi_fd[i] == fd) {
            pid = i;
            break;
        }
    }

    if (closed) {
        /* If a PMI application terminates, we clean up the remaining
         * processes. For a correct PMI application, we should never
         * get closed socket event as we deregister this socket as
         * soon as we get the finalize message. For non-PMI
         * applications, this is harder to identify, so we just let
         * the user cleanup the processes on a failure.
         *
         * We check of we found the PMI FD, and if the FD is "PMI
         * active" (which means that this is an MPI application).
         */
        if (pid != -1 && HYD_pmcd_pmip.downstream.pmi_fd_active[pid]) {
            MPL_snprintf(ftb_event_payload, HYDT_FTB_MAX_PAYLOAD_DATA,
                         "pgid:%d rank:%d",
                         HYD_pmcd_pmip.local.pgid, HYD_pmcd_pmip.downstream.pmi_rank[pid]);
            status = HYDT_ftb_publish("FTB_MPI_PROCS_DEAD", ftb_event_payload);
            HYDU_ERR_POP(status, "FTB publish failed\n");

            /* If this is not a forced cleanup, store a temporary
             * erroneous exit status. In case the application does not
             * return a non-zero exit status, we will use this. */
            if (HYD_pmcd_pmip.downstream.forced_cleanup == 0)
                HYD_pmcd_pmip.downstream.exit_status[pid] = 1;

            /* Deregister failed socket */
            status = HYDT_dmx_deregister_fd(fd);
            HYDU_ERR_POP(status, "unable to deregister fd\n");
            close(fd);

            if (HYD_pmcd_pmip.user_global.auto_cleanup) {
                HYD_pmcd_pmip_kill_localprocs();
            }
            else {
                /* If the user doesn't want to automatically cleanup,
                 * signal the remaining processes, and send this
                 * information upstream */

                /* FIXME: This code needs to change from sending the
                 * SIGUSR1 signal to a PMI-2 notification message. */
                for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
                    if (HYD_pmcd_pmip.downstream.pid[i] != -1)
                        kill(HYD_pmcd_pmip.downstream.pid[i], SIGUSR1);

                hdr.cmd = PROCESS_TERMINATED;
                hdr.pid = HYD_pmcd_pmip.downstream.pmi_rank[pid];
                status = HYDU_sock_write(HYD_pmcd_pmip.upstream.control, &hdr, sizeof(hdr),
                                         &sent, &closed);
                HYDU_ERR_POP(status, "unable to send PMI header upstream\n");
                HYDU_ASSERT(!closed, status);
            }
        }
        goto fn_exit;
    }
    else {
        storage_len += linelen;
        storage[storage_len] = 0;
    }

    /* We were able to read the PMI command correctly. If we were able
     * to identify what PMI FD this is, activate it. If we were not
     * able to identify the PMI FD, we will activate it when we get
     * the PMI initialization command. */
    if (pid != -1 && !HYD_pmcd_pmip.downstream.pmi_fd_active[pid])
        HYD_pmcd_pmip.downstream.pmi_fd_active[pid] = 1;

    do {
        status = check_pmi_cmd(&buf, &hdr.pmi_version, &repeat);
        HYDU_ERR_POP(status, "error checking the PMI command\n");

        if (buf == NULL)
            break;

        if (hdr.pmi_version == 1)
            HYD_pmcd_pmip_pmi_handle = HYD_pmcd_pmip_pmi_v1;
        else
            HYD_pmcd_pmip_pmi_handle = HYD_pmcd_pmip_pmi_v2;

        status = HYD_pmcd_pmi_parse_pmi_cmd(buf, hdr.pmi_version, &pmi_cmd, args);
        HYDU_ERR_POP(status, "unable to parse PMI command\n");

        if (HYD_pmcd_pmip.user_global.debug) {
            HYDU_dump(stdout, "got pmi command (from %d): %s\n", fd, pmi_cmd);
            HYDU_print_strlist(args);
        }

        h = HYD_pmcd_pmip_pmi_handle;
        while (h->handler) {
            if (!strcmp(pmi_cmd, h->cmd)) {
                status = h->handler(fd, args);
                HYDU_ERR_POP(status, "PMI handler returned error\n");
                goto fn_exit;
            }
            h++;
        }

        if (HYD_pmcd_pmip.user_global.debug) {
            HYDU_dump(stdout, "we don't understand this command %s; forwarding upstream\n",
                      pmi_cmd);
        }

        /* We don't understand the command; forward it upstream */
        hdr.cmd = PMI_CMD;
        hdr.pid = fd;
        hdr.buflen = strlen(buf);
        status =
            HYDU_sock_write(HYD_pmcd_pmip.upstream.control, &hdr, sizeof(hdr), &sent, &closed);
        HYDU_ERR_POP(status, "unable to send PMI header upstream\n");
        HYDU_ASSERT(!closed, status);

        status =
            HYDU_sock_write(HYD_pmcd_pmip.upstream.control, buf, hdr.buflen, &sent, &closed);
        HYDU_ERR_POP(status, "unable to send PMI command upstream\n");
        HYDU_ASSERT(!closed, status);

    } while (repeat);

  fn_exit:
    if (pmi_cmd)
        HYDU_FREE(pmi_cmd);
    HYDU_free_strlist(args);
    if (buf)
        HYDU_FREE(buf);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status handle_pmi_response(int fd, struct HYD_pmcd_hdr hdr)
{
    int count, closed, sent;
    char *buf = NULL, *pmi_cmd = NULL, *args[HYD_NUM_TMP_STRINGS] = { 0 };
    struct HYD_pmcd_pmip_pmi_handle *h;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    HYDU_MALLOC(buf, char *, hdr.buflen + 1, status);

    status = HYDU_sock_read(fd, buf, hdr.buflen, &count, &closed, HYDU_SOCK_COMM_MSGWAIT);
    HYDU_ERR_POP(status, "unable to read PMI response from proxy\n");
    HYDU_ASSERT(!closed, status);

    buf[hdr.buflen] = 0;

    status = HYD_pmcd_pmi_parse_pmi_cmd(buf, hdr.pmi_version, &pmi_cmd, args);
    HYDU_ERR_POP(status, "unable to parse PMI command\n");

    h = HYD_pmcd_pmip_pmi_handle;
    while (h->handler) {
        if (!strcmp(pmi_cmd, h->cmd)) {
            status = h->handler(fd, args);
            HYDU_ERR_POP(status, "PMI handler returned error\n");
            goto fn_exit;
        }
        h++;
    }

    if (HYD_pmcd_pmip.user_global.debug) {
        HYDU_dump(stdout, "we don't understand the response %s; forwarding downstream\n",
                  pmi_cmd);
    }

    status = HYDU_sock_write(hdr.pid, buf, hdr.buflen, &sent, &closed);
    HYDU_ERR_POP(status, "unable to forward PMI response to MPI process\n");

    if (HYD_pmcd_pmip.user_global.auto_cleanup) {
        HYDU_ASSERT(!closed, status);
    }
    else {
        /* Ignore the error and drop the PMI response */
    }

  fn_exit:
    if (pmi_cmd)
        HYDU_FREE(pmi_cmd);
    HYDU_free_strlist(args);
    if (buf)
        HYDU_FREE(buf);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status pmi_listen_cb(int fd, HYD_event_t events, void *userp)
{
    int accept_fd;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    status = HYDU_sock_accept(fd, &accept_fd);
    HYDU_ERR_POP(status, "accept error\n");

    status = HYDT_dmx_register_fd(1, &accept_fd, HYD_POLLIN, userp, pmi_cb);
    HYDU_ERR_POP(status, "unable to register fd\n");

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static int local_to_global_id(int local_id)
{
    int rem1, layer, rem2;
    int ret;

    if (local_id < HYD_pmcd_pmip.system_global.filler_process_map.current)
        ret = HYD_pmcd_pmip.system_global.filler_process_map.left + local_id;
    else {
        /* rem1 gives the number of processes remaining after the
         * filling the holes */
        rem1 = local_id - HYD_pmcd_pmip.system_global.filler_process_map.current;

        /* layer gives the layer of filling in which our process lies
         * starting from layer 0; in each layer, we fill all proxies
         * in the global list */
        layer = rem1 / HYD_pmcd_pmip.system_global.global_core_map.current;

        /* rem2 gives our relative index in the layer we belong to */
        rem2 = rem1 % HYD_pmcd_pmip.system_global.global_core_map.current;

        ret = (HYD_pmcd_pmip.system_global.filler_process_map.total +
               (layer * HYD_pmcd_pmip.system_global.global_core_map.total) +
               HYD_pmcd_pmip.system_global.global_core_map.left + rem2);
    }

    return ret;
}

static HYD_status launch_procs(void)
{
    int i, j, arg, process_id;
    char *str, *envstr, *list, *pmi_port;
    char *client_args[HYD_NUM_TMP_STRINGS];
    struct HYD_env *env, *force_env = NULL;
    struct HYD_exec *exec;
    struct HYD_pmcd_hdr hdr;
    int sent, closed, pmi_fds[2] = { HYD_FD_UNSET, HYD_FD_UNSET };
    struct HYDT_topo_cpuset_t cpuset;
    char ftb_event_payload[HYDT_FTB_MAX_PAYLOAD_DATA];
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    HYD_pmcd_pmip.local.proxy_process_count = 0;
    for (exec = HYD_pmcd_pmip.exec_list; exec; exec = exec->next)
        HYD_pmcd_pmip.local.proxy_process_count += exec->proc_count;

    HYDU_MALLOC(HYD_pmcd_pmip.downstream.out, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.err, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.pid, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.exit_status, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.pmi_rank, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.pmi_fd, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);
    HYDU_MALLOC(HYD_pmcd_pmip.downstream.pmi_fd_active, int *,
                HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), status);

    /* Initialize the PMI_FD and PMI FD active state, and exit status */
    for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++) {
        /* The exit status is populated when the processes terminate */
        HYD_pmcd_pmip.downstream.exit_status[i] = -1;

        /* If we use PMI_FD, the pmi_fd and pmi_fd_active arrays will
         * be filled out in this function. But if we are using
         * PMI_PORT, we will fill them out later when the processes
         * send the PMI initialization message. Note that non-MPI
         * processes are never "PMI active" when we use the PMI
         * PORT. */
        HYD_pmcd_pmip.downstream.pmi_fd[i] = HYD_FD_UNSET;
        HYD_pmcd_pmip.downstream.pmi_fd_active[i] = 0;

        if (HYD_pmcd_pmip.system_global.pmi_rank == -1)
            HYD_pmcd_pmip.downstream.pmi_rank[i] = local_to_global_id(i);
        else
            HYD_pmcd_pmip.downstream.pmi_rank[i] = HYD_pmcd_pmip.system_global.pmi_rank;
    }

    status = HYDT_topo_init(HYD_pmcd_pmip.local.local_binding ?
                            HYD_pmcd_pmip.local.local_binding :
                            HYD_pmcd_pmip.user_global.binding,
                            HYD_pmcd_pmip.user_global.topolib);
    HYDU_ERR_POP(status, "unable to initialize process topology\n");

    if (HYD_pmcd_pmip.user_global.debug) {
        char *map;

        status = HYDT_topo_get_topomap(&map);
        HYDU_ERR_POP(status, "error reading topology map\n");
        if (map)
            HYDU_dump(stdout, "topomap: %s\n", map);
        HYDU_FREE(map);

        status = HYDT_topo_get_processmap(&map);
        HYDU_ERR_POP(status, "error reading process map\n");
        if (map)
            HYDU_dump(stdout, "processmap: %s\n", map);
        HYDU_FREE(map);
    }

    status = HYDT_ckpoint_init(HYD_pmcd_pmip.user_global.ckpointlib,
                               HYD_pmcd_pmip.user_global.ckpoint_num);
    HYDU_ERR_POP(status, "unable to initialize checkpointing\n");

    if (HYD_pmcd_pmip.system_global.pmi_port || HYD_pmcd_pmip.user_global.ckpoint_prefix) {
        using_pmi_port = 1;
        if (HYD_pmcd_pmip.system_global.pmi_port)
            pmi_port = HYD_pmcd_pmip.system_global.pmi_port;
        else {
            status = HYDU_sock_create_and_listen_portstr(HYD_pmcd_pmip.user_global.iface,
                                                         NULL, NULL, &pmi_port, pmi_listen_cb,
                                                         NULL);
            HYDU_ERR_POP(status, "unable to create PMI port\n");
        }
    }

    if (HYD_pmcd_pmip.exec_list->exec[0] == NULL) {     /* Checkpoint restart cast */
        status = HYDU_env_create(&env, "PMI_PORT", pmi_port);
        HYDU_ERR_POP(status, "unable to create env\n");

        /* Restart the proxy -- we use the first prefix in the list */
        MPL_snprintf(ftb_event_payload, HYDT_FTB_MAX_PAYLOAD_DATA, "pgid:%d ranks:%d-%d",
                     HYD_pmcd_pmip.local.pgid, HYD_pmcd_pmip.downstream.pmi_rank[0],
                     HYD_pmcd_pmip.downstream.pmi_rank
                     [HYD_pmcd_pmip.local.proxy_process_count - 1]);
        status = HYDT_ckpoint_restart(HYD_pmcd_pmip.local.pgid, HYD_pmcd_pmip.local.id,
                                      env, HYD_pmcd_pmip.local.proxy_process_count,
                                      HYD_pmcd_pmip.downstream.pmi_rank,
                                      HYD_pmcd_pmip.downstream.pmi_rank[0] ? NULL :
                                      &HYD_pmcd_pmip.downstream.in,
                                      HYD_pmcd_pmip.downstream.out,
                                      HYD_pmcd_pmip.downstream.err,
                                      HYD_pmcd_pmip.downstream.pid,
                                      HYD_pmcd_pmip.local.ckpoint_prefix_list[0]);
        if (status)
            status = HYDT_ftb_publish("FTB_MPI_PROCS_RESTART_FAIL", ftb_event_payload);
        else
            status = HYDT_ftb_publish("FTB_MPI_PROCS_RESTARTED", ftb_event_payload);
        HYDU_ERR_POP(status, "checkpoint restart FTB publishing failure\n");
        goto fn_spawn_complete;
    }

    /* Spawn the processes */
    process_id = 0;
    for (exec = HYD_pmcd_pmip.exec_list; exec; exec = exec->next) {

        /* Increasing priority order: (1) global inherited env; (2)
         * global user env; (3) local user env; (4) system env. We
         * just set them one after the other, overwriting the previous
         * written value if needed. */

        if (!exec->env_prop && HYD_pmcd_pmip.user_global.global_env.prop)
            exec->env_prop = HYDU_strdup(HYD_pmcd_pmip.user_global.global_env.prop);

        if (!exec->env_prop) {
            /* user didn't specify anything; add inherited env to optional env */
            for (env = HYD_pmcd_pmip.user_global.global_env.inherited; env; env = env->next) {
                status = HYDU_append_env_to_list(env->env_name, env->env_value, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
            }
        }
        else if (!strcmp(exec->env_prop, "all")) {
            /* user explicitly asked us to pass all the environment */
            for (env = HYD_pmcd_pmip.user_global.global_env.inherited; env; env = env->next) {
                status = HYDU_append_env_to_list(env->env_name, env->env_value, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
            }
        }
        else if (!strncmp(exec->env_prop, "list", strlen("list"))) {
            if (exec->env_prop)
                list = HYDU_strdup(exec->env_prop + strlen("list:"));
            else
                list = HYDU_strdup(HYD_pmcd_pmip.user_global.global_env.prop +
                                   strlen("list:"));

            envstr = strtok(list, ",");
            while (envstr) {
                env = HYDU_env_lookup(envstr, HYD_pmcd_pmip.user_global.global_env.inherited);
                if (env) {
                    status = HYDU_append_env_to_list(env->env_name, env->env_value,
                                                     &force_env);
                    HYDU_ERR_POP(status, "unable to add env to list\n");
                }
                envstr = strtok(NULL, ",");
            }
        }

        /* global user env */
        for (env = HYD_pmcd_pmip.user_global.global_env.user; env; env = env->next) {
            status = HYDU_append_env_to_list(env->env_name, env->env_value, &force_env);
            HYDU_ERR_POP(status, "unable to add env to list\n");
        }

        /* local user env */
        for (env = exec->user_env; env; env = env->next) {
            status = HYDU_append_env_to_list(env->env_name, env->env_value, &force_env);
            HYDU_ERR_POP(status, "unable to add env to list\n");
        }

        /* system env */
        for (env = HYD_pmcd_pmip.user_global.global_env.system; env; env = env->next) {
            status = HYDU_append_env_to_list(env->env_name, env->env_value, &force_env);
            HYDU_ERR_POP(status, "unable to add env to list\n");
        }

        /* Job ID information */
        if (HYD_pmcd_pmip.system_global.jobid) {
            status = HYDU_append_env_to_list("HYDRA_JOBID", HYD_pmcd_pmip.system_global.jobid,
                                             &force_env);
            HYDU_ERR_POP(status, "unable to add env to list\n");
        }

        /* Set the interface hostname based on what the user provided */
        if (HYD_pmcd_pmip.local.iface_ip_env_name) {
            if (HYD_pmcd_pmip.user_global.iface) {
                char *ip;

                status = HYDU_sock_get_iface_ip(HYD_pmcd_pmip.user_global.iface, &ip);
                HYDU_ERR_POP(status, "unable to get IP address for %s\n",
                             HYD_pmcd_pmip.user_global.iface);

                /* The user asked us to use a specific interface; let's find it */
                status = HYDU_append_env_to_list(HYD_pmcd_pmip.local.iface_ip_env_name,
                                                 ip, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
            }
            else if (HYD_pmcd_pmip.local.hostname) {
                /* The second choice is the hostname the user gave */
                status = HYDU_append_env_to_list(HYD_pmcd_pmip.local.iface_ip_env_name,
                                                 HYD_pmcd_pmip.local.hostname, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
            }
        }

        if (exec->wdir && chdir(exec->wdir) < 0)
            HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR,
                                "unable to change wdir to %s (%s)\n", exec->wdir,
                                HYDU_strerror(errno));

        for (i = 0; i < exec->proc_count; i++) {
            if (using_pmi_port) {
                /* If we are using the PMI_PORT format */

                /* PMI_PORT */
                status = HYDU_append_env_to_list("PMI_PORT", pmi_port, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");

                /* PMI_ID */
                str = HYDU_int_to_str(HYD_pmcd_pmip.downstream.pmi_rank[process_id]);
                status = HYDU_append_env_to_list("PMI_ID", str, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
                HYDU_FREE(str);
            }
            else {
                /* PMI_RANK */
                str = HYDU_int_to_str(HYD_pmcd_pmip.downstream.pmi_rank[process_id]);
                status = HYDU_append_env_to_list("PMI_RANK", str, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
                HYDU_FREE(str);

                /* PMI_FD */
                if (HYD_pmcd_pmip.system_global.pmi_fd) {
                    /* If a global PMI port is provided, use it */
                    str = HYDU_strdup(HYD_pmcd_pmip.system_global.pmi_fd);
                }
                else {
                    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pmi_fds) < 0)
                        HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR, "pipe error\n");

                    status = HYDT_dmx_register_fd(1, &pmi_fds[0], HYD_POLLIN, NULL, pmi_cb);
                    HYDU_ERR_POP(status, "unable to register fd\n");

                    status = HYDU_sock_cloexec(pmi_fds[0]);
                    HYDU_ERR_POP(status, "unable to set socket to close on exec\n");

                    HYD_pmcd_pmip.downstream.pmi_fd[process_id] = pmi_fds[0];
                    str = HYDU_int_to_str(pmi_fds[1]);
                }

                status = HYDU_append_env_to_list("PMI_FD", str, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
                HYDU_FREE(str);

                /* PMI_SIZE */
                str = HYDU_int_to_str(HYD_pmcd_pmip.system_global.global_process_count);
                status = HYDU_append_env_to_list("PMI_SIZE", str, &force_env);
                HYDU_ERR_POP(status, "unable to add env to list\n");
                HYDU_FREE(str);
            }

            for (j = 0, arg = 0; exec->exec[j]; j++)
                client_args[arg++] = HYDU_strdup(exec->exec[j]);
            client_args[arg++] = NULL;

            HYDT_topo_pid_to_cpuset(process_id, &cpuset);
            status = HYDU_create_process(client_args, force_env,
                                         HYD_pmcd_pmip.downstream.pmi_rank[process_id] ? NULL :
                                         &HYD_pmcd_pmip.downstream.in,
                                         &HYD_pmcd_pmip.downstream.out[process_id],
                                         &HYD_pmcd_pmip.downstream.err[process_id],
                                         &HYD_pmcd_pmip.downstream.pid[process_id], cpuset);
            HYDU_ERR_POP(status, "create process returned error\n");

            HYDU_free_strlist(client_args);

            if (pmi_fds[1] != HYD_FD_UNSET) {
                close(pmi_fds[1]);
                pmi_fds[1] = HYD_FD_CLOSED;
            }

            process_id++;
        }

        HYDU_env_free_list(force_env);
        force_env = NULL;
    }

    /* Send the PID list upstream */
    HYD_pmcd_init_header(&hdr);
    hdr.cmd = PID_LIST;
    status =
        HYDU_sock_write(HYD_pmcd_pmip.upstream.control, &hdr, sizeof(hdr), &sent, &closed);
    HYDU_ERR_POP(status, "unable to send PID_LIST command upstream\n");
    HYDU_ASSERT(!closed, status);

    status = HYDU_sock_write(HYD_pmcd_pmip.upstream.control,
                             HYD_pmcd_pmip.downstream.pid,
                             HYD_pmcd_pmip.local.proxy_process_count * sizeof(int), &sent,
                             &closed);
    HYDU_ERR_POP(status, "unable to send PID list upstream\n");
    HYDU_ASSERT(!closed, status);

  fn_spawn_complete:
    /* Everything is spawned, register the required FDs  */
    status = HYDT_dmx_register_fd(HYD_pmcd_pmip.local.proxy_process_count,
                                  HYD_pmcd_pmip.downstream.out, HYD_POLLIN,
                                  (void *) (size_t) STDOUT_FILENO, stdio_cb);
    HYDU_ERR_POP(status, "unable to register fd\n");

    status = HYDT_dmx_register_fd(HYD_pmcd_pmip.local.proxy_process_count,
                                  HYD_pmcd_pmip.downstream.err, HYD_POLLIN,
                                  (void *) (size_t) STDERR_FILENO, stdio_cb);
    HYDU_ERR_POP(status, "unable to register fd\n");

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status parse_exec_params(char **t_argv)
{
    char **argv = t_argv;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    do {
        /* Get the executable arguments  */
        status = HYDU_parse_array(&argv, HYD_pmcd_pmip_match_table);
        HYDU_ERR_POP(status, "error parsing input array\n");

        /* No more arguments left */
        if (!(*argv))
            break;
    } while (1);

    /* verify the arguments we got */
    if (HYD_pmcd_pmip.system_global.global_core_map.left == -1 ||
        HYD_pmcd_pmip.system_global.global_core_map.current == -1 ||
        HYD_pmcd_pmip.system_global.global_core_map.right == -1)
        HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR,
                            "cannot find global core map (%d,%d,%d)\n",
                            HYD_pmcd_pmip.system_global.global_core_map.left,
                            HYD_pmcd_pmip.system_global.global_core_map.current,
                            HYD_pmcd_pmip.system_global.global_core_map.right);

    if (HYD_pmcd_pmip.system_global.filler_process_map.left == -1 ||
        HYD_pmcd_pmip.system_global.filler_process_map.current == -1 ||
        HYD_pmcd_pmip.system_global.filler_process_map.right == -1)
        HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR,
                            "cannot find available cores (%d,%d,%d)\n",
                            HYD_pmcd_pmip.system_global.filler_process_map.left,
                            HYD_pmcd_pmip.system_global.filler_process_map.current,
                            HYD_pmcd_pmip.system_global.filler_process_map.right);

    if (HYD_pmcd_pmip.local.proxy_core_count == -1)
        HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR, "proxy core count not available\n");

    if (HYD_pmcd_pmip.exec_list == NULL && HYD_pmcd_pmip.user_global.ckpoint_prefix == NULL)
        HYDU_ERR_SETANDJUMP(status, HYD_INTERNAL_ERROR,
                            "no executable given and doesn't look like a restart either\n");

    /* Set default values */
    if (HYD_pmcd_pmip.user_global.binding == NULL && HYD_pmcd_pmip.local.local_binding == NULL)
        HYD_pmcd_pmip.user_global.binding = HYDU_strdup("none");

    if (HYD_pmcd_pmip.user_global.topolib == NULL && HYDRA_DEFAULT_TOPOLIB)
        HYD_pmcd_pmip.user_global.topolib = HYDU_strdup(HYDRA_DEFAULT_TOPOLIB);

    if (HYD_pmcd_pmip.user_global.ckpointlib == NULL && HYDRA_DEFAULT_CKPOINTLIB)
        HYD_pmcd_pmip.user_global.ckpointlib = HYDU_strdup(HYDRA_DEFAULT_CKPOINTLIB);

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

static HYD_status procinfo(int fd)
{
    char **arglist;
    int num_strings, str_len, recvd, i, closed;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    /* Read information about the application to launch into a string
     * array and call parse_exec_params() to interpret it and load it into
     * the proxy handle. */
    status = HYDU_sock_read(fd, &num_strings, sizeof(int), &recvd, &closed,
                            HYDU_SOCK_COMM_MSGWAIT);
    HYDU_ERR_POP(status, "error reading data from upstream\n");
    HYDU_ASSERT(!closed, status);

    HYDU_MALLOC(arglist, char **, (num_strings + 1) * sizeof(char *), status);

    for (i = 0; i < num_strings; i++) {
        status = HYDU_sock_read(fd, &str_len, sizeof(int), &recvd, &closed,
                                HYDU_SOCK_COMM_MSGWAIT);
        HYDU_ERR_POP(status, "error reading data from upstream\n");
        HYDU_ASSERT(!closed, status);

        HYDU_MALLOC(arglist[i], char *, str_len, status);

        status = HYDU_sock_read(fd, arglist[i], str_len, &recvd, &closed,
                                HYDU_SOCK_COMM_MSGWAIT);
        HYDU_ERR_POP(status, "error reading data from upstream\n");
        HYDU_ASSERT(!closed, status);
    }
    arglist[num_strings] = NULL;

    /* Get the parser to fill in the proxy params structure. */
    status = parse_exec_params(arglist);
    HYDU_ERR_POP(status, "unable to parse argument list\n");

    HYDU_free_strlist(arglist);
    HYDU_FREE(arglist);

    /* Save this fd as we need to send back the exit status on
     * this. */
    HYD_pmcd_pmip.upstream.control = fd;

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}

HYD_status HYD_pmcd_pmip_control_cmd_cb(int fd, HYD_event_t events, void *userp)
{
    int cmd_len, closed, i;
    struct HYD_pmcd_hdr hdr;
    char ftb_event_payload[HYDT_FTB_MAX_PAYLOAD_DATA];
    char *buf;
    HYD_status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    /* We got a command from upstream */
    status = HYDU_sock_read(fd, &hdr, sizeof(hdr), &cmd_len, &closed, HYDU_SOCK_COMM_MSGWAIT);
    HYDU_ERR_POP(status, "error reading command from launcher\n");
    HYDU_ASSERT(!closed, status);

    if (hdr.cmd == PROC_INFO) {
        status = procinfo(fd);
        HYDU_ERR_POP(status, "error parsing process info\n");

        status = launch_procs();
        HYDU_ERR_POP(status, "launch_procs returned error\n");
    }
    else if (hdr.cmd == CKPOINT) {
        HYDU_dump(stdout, "requesting checkpoint\n");

        MPL_snprintf(ftb_event_payload, HYDT_FTB_MAX_PAYLOAD_DATA, "pgid:%d ranks:%d-%d",
                     HYD_pmcd_pmip.local.pgid, HYD_pmcd_pmip.downstream.pmi_rank[0],
                     HYD_pmcd_pmip.downstream.pmi_rank
                     [HYD_pmcd_pmip.local.proxy_process_count - 1]);
        status = HYDT_ckpoint_checkpoint(HYD_pmcd_pmip.local.pgid, HYD_pmcd_pmip.local.id,
                                         HYD_pmcd_pmip.local.ckpoint_prefix_list[0]);

        HYDU_ERR_POP(status, "checkpoint suspend failed\n");
        HYDU_dump(stdout, "checkpoint completed\n");
    }
    else if (hdr.cmd == PMI_RESPONSE) {
        status = handle_pmi_response(fd, hdr);
        HYDU_ERR_POP(status, "unable to handle PMI response\n");
    }
    else if (hdr.cmd == SIGNAL) {
        /* FIXME: This code needs to change from sending the signal to
         * a PMI-2 notification message. */
        for (i = 0; i < HYD_pmcd_pmip.local.proxy_process_count; i++)
            if (HYD_pmcd_pmip.downstream.pid[i] != -1)
                kill(HYD_pmcd_pmip.downstream.pid[i], hdr.signum);
    }
    else if (hdr.cmd == STDIN) {
        int count;

        if (hdr.buflen) {
            if (HYD_pmcd_pmip.downstream.in == HYD_FD_CLOSED)
                goto fn_exit;

            HYDU_MALLOC(buf, char *, hdr.buflen, status);
            HYDU_ERR_POP(status, "unable to allocate memory\n");

            status = HYDU_sock_read(fd, buf, hdr.buflen, &count, &closed,
                                    HYDU_SOCK_COMM_MSGWAIT);
            HYDU_ERR_POP(status, "unable to read from control socket\n");
            HYDU_ASSERT(!closed, status);

            status = HYDU_sock_write(HYD_pmcd_pmip.downstream.in, buf, hdr.buflen, &count,
                                     &closed);
            HYDU_ERR_POP(status, "unable to write to downstream stdin\n");

            if (HYD_pmcd_pmip.user_global.auto_cleanup) {
                HYDU_ASSERT(!closed, status);
            }
            else {
                close(HYD_pmcd_pmip.downstream.in);
                HYD_pmcd_pmip.downstream.in = HYD_FD_CLOSED;
            }

            HYDU_FREE(buf);
        }
        else {
            close(HYD_pmcd_pmip.downstream.in);
        }
    }
    else {
        status = HYD_INTERNAL_ERROR;
    }

    HYDU_ERR_POP(status, "error handling proxy command\n");

  fn_exit:
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}
