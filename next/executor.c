/*
 * executor.c
 *
 * Common routines related to query execution phase
 * ----
 * Copyright 2011-2021 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2021 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "pg_strom.h"

/*
 * XpuConnection
 */
struct XpuConnection
{
	dlist_node		chain;	/* link to gpuserv_connection_slots */
	char			devname[32];
	volatile pgsocket sockfd;
	volatile int	terminated;		/* positive: normal exit
									 * negative: exit by errors */
	ResourceOwner	resowner;
	pthread_t		worker;
	pthread_mutex_t	mutex;
	int				num_running_cmds;
	int				num_ready_cmds;
	dlist_head		ready_cmds_list;	/* ready, but not fetched yet  */
	dlist_head		active_cmds_list;	/* currently in-use */
	kern_errorbuf	errorbuf;
};

/* see xact.c */
extern int				nParallelCurrentXids;
extern TransactionId   *ParallelCurrentXids;

/* static variables */
static dlist_head		xpu_connections_list;
static bool				pgstrom_use_debug_code;		/* GUC */
static shmem_request_hook_type shmem_request_next = NULL;
static shmem_startup_hook_type shmem_startup_next = NULL;
static pg_atomic_uint32	*pgstrom_query_plan_id = NULL;

/*
 * Worker thread to receive response messages
 */
static void *
__xpuConnectAllocCommand(void *__priv, size_t sz)
{
	return malloc(sz);
}

static void
__xpuConnectAttachCommand(void *__priv, XpuCommand *xcmd)
{
	XpuConnection *conn = __priv;

	xcmd->priv = conn;
	pthreadMutexLock(&conn->mutex);
	Assert(conn->num_running_cmds > 0);
	conn->num_running_cmds--;
	if (xcmd->tag == XpuCommandTag__Error)
	{
		if (conn->errorbuf.errcode == ERRCODE_STROM_SUCCESS)
		{
			Assert(xcmd->u.error.errcode != ERRCODE_STROM_SUCCESS);
			memcpy(&conn->errorbuf, &xcmd->u.error, sizeof(kern_errorbuf));
		}
		free(xcmd);
	}
	else
	{
		Assert(xcmd->tag == XpuCommandTag__Success ||
			   xcmd->tag == XpuCommandTag__CPUFallback);
		dlist_push_tail(&conn->ready_cmds_list, &xcmd->chain);
		conn->num_ready_cmds++;
	}
	SetLatch(MyLatch);
	pthreadMutexUnlock(&conn->mutex);
}
TEMPLATE_XPU_CONNECT_RECEIVE_COMMANDS(__xpuConnect)

static void *
__xpuConnectSessionWorker(void *__priv)
{
	XpuConnection *conn = __priv;

	for (;;)
	{
		struct pollfd pfd;
		int		sockfd;
		int		nevents;

		sockfd = conn->sockfd;
		if (sockfd < 0)
			break;
		pfd.fd = sockfd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		nevents = poll(&pfd, 1, -1);
		if (nevents < 0)
		{
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[%s; %s:%d] failed on poll(2): %m\n",
					conn->devname, __FILE_NAME__, __LINE__);
			break;
		}
		else if (nevents > 0)
		{
			Assert(nevents == 1);
			if (pfd.revents & ~POLLIN)
			{
				pthreadMutexLock(&conn->mutex);
				conn->terminated = 1;
				SetLatch(MyLatch);
				pthreadMutexUnlock(&conn->mutex);
				return NULL;
			}
			else if (pfd.revents & POLLIN)
			{
				if (__xpuConnectReceiveCommands(conn->sockfd,
												conn,
												conn->devname) < 0)
					break;
			}
		}
	}
	pthreadMutexLock(&conn->mutex);
	conn->terminated = -1;
	SetLatch(MyLatch);
	pthreadMutexUnlock(&conn->mutex);

	return NULL;
}

/*
 * xpuClientSendCommand
 */
void
xpuClientSendCommand(XpuConnection *conn, const XpuCommand *xcmd)
{
	int			sockfd = conn->sockfd;
	const char *buf = (const char *)xcmd;
	size_t		len = xcmd->length;
	ssize_t		nbytes;

	pthreadMutexLock(&conn->mutex);
	conn->num_running_cmds++;
	pthreadMutexUnlock(&conn->mutex);

	while (len > 0)
	{
		nbytes = write(sockfd, buf, len);
		if (nbytes > 0)
		{
			buf += nbytes;
			len -= nbytes;
		}
		else if (nbytes == 0)
			elog(ERROR, "unable to send xPU command to the service");
		else if (errno == EINTR)
			CHECK_FOR_INTERRUPTS();
		else
			elog(ERROR, "failed on write(2): %m");
	}
}

/*
 * xpuClientSendCommandIOV
 */
static void
xpuClientSendCommandIOV(XpuConnection *conn, struct iovec *iov, int iovcnt)
{
	int			sockfd = conn->sockfd;
	ssize_t		nbytes;

	Assert(iovcnt > 0);
	pthreadMutexLock(&conn->mutex);
	conn->num_running_cmds++;
	pthreadMutexUnlock(&conn->mutex);

	while (iovcnt > 0)
	{
		nbytes = writev(sockfd, iov, iovcnt);
		if (nbytes > 0)
		{
			do {
				if (iov->iov_len <= nbytes)
				{
					nbytes -= iov->iov_len;
					iov++;
					iovcnt--;
				}
				else
				{
					iov->iov_base = (char *)iov->iov_base + nbytes;
					iov->iov_len -= nbytes;
					break;
				}
			} while (iovcnt > 0 && nbytes > 0);
		}
		else if (nbytes == 0)
			elog(ERROR, "unable to send xPU command to the service");
		else if (errno == EINTR)
			CHECK_FOR_INTERRUPTS();
		else
			elog(ERROR, "failed on writev(2): %m");
	}
}

/*
 * xpuClientPutResponse
 */
void
xpuClientPutResponse(XpuCommand *xcmd)
{
	XpuConnection  *conn = xcmd->priv;
	
	pthreadMutexLock(&conn->mutex);
	dlist_delete(&xcmd->chain);
	pthreadMutexUnlock(&conn->mutex);
	free(xcmd);
}

/*
 * xpuClientCloseSession
 */
void
xpuClientCloseSession(XpuConnection *conn)
{
	XpuCommand *xcmd;
	dlist_node *dnode;

	/* ensure termination of worker thread */
	close(conn->sockfd);
	conn->sockfd = -1;
	pg_memory_barrier();
	pthread_kill(conn->worker, SIGPOLL);
	pthread_join(conn->worker, NULL);

	while (!dlist_is_empty(&conn->ready_cmds_list))
	{
		dnode = dlist_pop_head_node(&conn->ready_cmds_list);
		xcmd = dlist_container(XpuCommand, chain, dnode);
		free(xcmd);
	}
	while (!dlist_is_empty(&conn->active_cmds_list))
	{
		dnode = dlist_pop_head_node(&conn->active_cmds_list);
		xcmd = dlist_container(XpuCommand, chain, dnode);
		free(xcmd);
	}
	dlist_delete(&conn->chain);
	free(conn);
}

/*
 * xpuclientCleanupConnections
 */
static void
xpuclientCleanupConnections(ResourceReleasePhase phase,
							bool isCommit,
							bool isTopLevel,
							void *arg)
{
	dlist_mutable_iter	iter;

	if (phase != RESOURCE_RELEASE_BEFORE_LOCKS)
		return;

	dlist_foreach_modify(iter, &xpu_connections_list)
	{
		XpuConnection  *conn = dlist_container(XpuConnection,
											   chain, iter.cur);
		if (conn->resowner == CurrentResourceOwner)
		{
			if (isCommit)
				elog(LOG, "Bug? GPU connection %d is not closed on ExecEnd",
					 conn->sockfd);
			xpuClientCloseSession(conn);
		}
	}
}

/* ----------------------------------------------------------------
 *
 * Routines to build session-information
 *
 * ----------------------------------------------------------------
 */
static void
__build_session_param_info(pgstromTaskState *pts,
						   kern_session_info *session,
						   StringInfo buf)
{
	pgstromPlanInfo *pp_info = pts->pp_info;
	ExprContext	   *econtext = pts->css.ss.ps.ps_ExprContext;
	ParamListInfo	param_info = econtext->ecxt_param_list_info;
	ListCell	   *lc;

	Assert(param_info != NULL);
	session->nparams = param_info->numParams;
	foreach (lc, pp_info->used_params)
	{
		Param	   *param = lfirst(lc);
		Datum		param_value;
		bool		param_isnull;
		uint32_t	offset;

		Assert(param->paramid >= 0 &&
			   param->paramid < session->nparams);
		if (param->paramkind == PARAM_EXEC)
		{
			/* See ExecEvalParamExec */
			ParamExecData  *prm = &(econtext->ecxt_param_exec_vals[param->paramid]);

			if (prm->execPlan)
			{
				/* Parameter not evaluated yet, so go do it */
				ExecSetParamPlan(prm->execPlan, econtext);
				/* ExecSetParamPlan should have processed this param... */
				Assert(prm->execPlan == NULL);
			}
			param_isnull = prm->isnull;
			param_value  = prm->value;
		}
		else if (param->paramkind == PARAM_EXTERN)
		{
			/* See ExecEvalParamExtern */
			ParamExternData *prm, prmData;

			if (param_info->paramFetch != NULL)
				prm = param_info->paramFetch(param_info,
											 param->paramid,
											 false, &prmData);
			else
				prm = &param_info->params[param->paramid - 1];
			if (!OidIsValid(prm->ptype))
				elog(ERROR, "no value found for parameter %d", param->paramid);
			if (prm->ptype != param->paramtype)
				elog(ERROR, "type of parameter %d (%s) does not match that when preparing the plan (%s)",
					 param->paramid,
					 format_type_be(prm->ptype),
					 format_type_be(param->paramtype));
			param_isnull = prm->isnull;
			param_value  = prm->value;
		}
		else
		{
			elog(ERROR, "Bug? unexpected parameter kind: %d",
				 (int)param->paramkind);
		}

		if (param_isnull)
			offset = 0;
		else
		{
			int16	typlen;
			bool	typbyval;

			get_typlenbyval(param->paramtype, &typlen, &typbyval);
			if (typbyval)
			{
				offset = __appendBinaryStringInfo(buf,
												  (char *)&param_value,
												  typlen);
			}
			else if (typlen > 0)
			{
				offset = __appendBinaryStringInfo(buf,
												  DatumGetPointer(param_value),
												  typlen);
			}
			else if (typlen == -1)
			{
				struct varlena *temp = PG_DETOAST_DATUM(param_value);

				offset = __appendBinaryStringInfo(buf,
												  DatumGetPointer(temp),
												  VARSIZE(temp));
				if (param_value != PointerGetDatum(temp))
					pfree(temp);
			}
			else
			{
				elog(ERROR, "Not a supported data type for kernel parameter: %s",
					 format_type_be(param->paramtype));
			}
		}
		Assert(param->paramid >= 0 && param->paramid < nparams);
		session->poffset[param->paramid] = offset;
	}
}



static uint32_t
__build_kvars_slot_cmeta(StringInfo buf,
						 pgstromTaskState *pts,
						 List *kvars_depth_list,
						 List *kvars_resno_list)
{
	kern_vars_defitem *kvars_defitem;
	int			nitems = list_length(kvars_depth_list);
	int			slot_id = 0;
	ListCell   *lc1, *lc2;

	kvars_defitem = alloca(sizeof(kern_vars_defitem) * nitems);
	forboth (lc1, kvars_depth_list,
			 lc2, kvars_resno_list)
	{
		kern_vars_defitem *kvar = &kvars_defitem[slot_id];

		kvar->var_depth = lfirst_int(lc1);
		kvar->var_resno = lfirst_int(lc2);
		kvar->var_slot_id = slot_id++;
	}
	return __appendBinaryStringInfo(buf, kvars_defitem,
									sizeof(kern_vars_defitem) * nitems);
}

static uint32_t
__build_session_xact_state(StringInfo buf)
{
	Size		bufsz;
	char	   *buffer;

	bufsz = EstimateTransactionStateSpace();
	buffer = alloca(bufsz);
	SerializeTransactionState(bufsz, buffer);

	return __appendBinaryStringInfo(buf, buffer, bufsz);
}

static uint32_t
__build_session_timezone(StringInfo buf)
{
	uint32_t	offset = 0;

	if (session_timezone)
	{
		offset = __appendBinaryStringInfo(buf, session_timezone,
										  sizeof(struct pg_tz));
	}
	return offset;
}

static uint32_t
__build_session_encode(StringInfo buf)
{
	xpu_encode_info encode;

	memset(&encode, 0, sizeof(xpu_encode_info));
	strncpy(encode.encname,
			GetDatabaseEncodingName(),
			sizeof(encode.encname));
	encode.enc_maxlen = pg_database_encoding_max_length();
	encode.enc_mblen = NULL;

	return __appendBinaryStringInfo(buf, &encode, sizeof(xpu_encode_info));
}

const XpuCommand *
pgstromBuildSessionInfo(pgstromTaskState *pts,
						uint32_t join_inner_handle)
{
	pgstromPlanInfo *pp_info = pts->pp_info;
	ExprContext	   *econtext = pts->css.ss.ps.ps_ExprContext;
	ParamListInfo	param_info = econtext->ecxt_param_list_info;
	uint32_t		nparams = (param_info ? param_info->numParams : 0);
	uint32_t		session_sz;
	kern_session_info *session;
	XpuCommand	   *xcmd;
	StringInfoData	buf;
	bytea		   *xpucode;

	initStringInfo(&buf);
	session_sz = offsetof(kern_session_info, poffset[nparams]);
	session = alloca(session_sz);
	memset(session, 0, session_sz);
	__appendZeroStringInfo(&buf, session_sz);
	if (param_info)
		__build_session_param_info(pts, session, &buf);
	if (pp_info->kexp_scan_kvars_load)
	{
		xpucode = pp_info->kexp_scan_kvars_load;
		session->xpucode_scan_load_vars =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_scan_quals)
	{
		xpucode = pp_info->kexp_scan_quals;
		session->xpucode_scan_quals =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_join_kvars_load_packed)
	{
		xpucode = pp_info->kexp_join_kvars_load_packed;
		session->xpucode_join_load_vars_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_join_quals_packed)
	{
		xpucode = pp_info->kexp_join_quals_packed;
		session->xpucode_join_quals_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_hash_keys_packed)
	{
		xpucode = pp_info->kexp_hash_keys_packed;
		session->xpucode_hash_values_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_gist_quals_packed)
	{
		xpucode = pp_info->kexp_gist_quals_packed;
		session->xpucode_gist_quals_packed =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	if (pp_info->kexp_projection)
	{
		xpucode = pp_info->kexp_projection;
		session->xpucode_projection =
			__appendBinaryStringInfo(&buf,
									 VARDATA(xpucode),
									 VARSIZE(xpucode) - VARHDRSZ);
	}
	/* other database session information */
	Assert(list_length(kvars_depth_list) == list_length(kvars_resno_list));
	session->query_plan_id = ((uint64_t)MyProcPid << 32) |
		(uint64_t)pg_atomic_fetch_add_u32(pgstrom_query_plan_id, 1);
	session->kcxt_extra_bufsz = pp_info->extra_bufsz;
	session->kvars_slot_width = list_length(pp_info->kvars_depth);
	Assert(session->kvars_slot_width == list_length(pp_info->kvars_resno));
	session->kvars_slot_items = __build_kvars_slot_cmeta(&buf, pts,
														 pp_info->kvars_depth,
														 pp_info->kvars_resno);
	session->xpucode_use_debug_code = pgstrom_use_debug_code;
	session->xactStartTimestamp = GetCurrentTransactionStartTimestamp();
	session->session_xact_state = __build_session_xact_state(&buf);
	session->session_timezone = __build_session_timezone(&buf);
	session->session_encode = __build_session_encode(&buf);
	session->pgsql_port_number = PostPortNumber;
	session->join_inner_handle = join_inner_handle;
	memcpy(buf.data, session, session_sz);

	/* setup XpuCommand */
	xcmd = palloc(offsetof(XpuCommand, u.session) + buf.len);
	memset(xcmd, 0, offsetof(XpuCommand, u.session));
	xcmd->magic = XpuCommandMagicNumber;
	xcmd->tag = XpuCommandTag__OpenSession;
	xcmd->length = offsetof(XpuCommand, u.session) + buf.len;
	memcpy(&xcmd->u.session, buf.data, buf.len);
	pfree(buf.data);

	return xcmd;
}

/*
 * __pickupNextXpuCommand
 *
 * MEMO: caller must hold 'conn->mutex'
 */
static XpuCommand *
__pickupNextXpuCommand(XpuConnection *conn)
{
	XpuCommand	   *xcmd;
	dlist_node	   *dnode;

	Assert(conn->num_ready_cmds > 0);
	dnode = dlist_pop_head_node(&conn->ready_cmds_list);
	xcmd = dlist_container(XpuCommand, chain, dnode);
	dlist_push_tail(&conn->active_cmds_list, &xcmd->chain);
	conn->num_ready_cmds--;

	return xcmd;
}

static XpuCommand *
__waitAndFetchNextXpuCommand(pgstromTaskState *pts, bool try_final_callback)
{
	XpuConnection  *conn = pts->conn;
	XpuCommand	   *xcmd;
	struct iovec	xcmd_iov[10];
	int				xcmd_iovcnt;
	int				ev;

	pthreadMutexLock(&conn->mutex);
	for (;;)
	{
		ResetLatch(MyLatch);

		/* device error checks */
		if (conn->errorbuf.errcode != ERRCODE_STROM_SUCCESS)
		{
			pthreadMutexUnlock(&conn->mutex);
			ereport(ERROR,
					(errcode(conn->errorbuf.errcode),
					 errmsg("%s:%d  %s",
							conn->errorbuf.filename,
							conn->errorbuf.lineno,
							conn->errorbuf.message),
					 errhint("device at %s, function at %s",
							 conn->devname,
							 conn->errorbuf.funcname)));

		}
		if (!dlist_is_empty(&conn->ready_cmds_list))
		{
			/* ok, ready commands we have */
			break;
		}
		else if (conn->num_running_cmds > 0)
		{
			/* wait for the running commands */
			pthreadMutexUnlock(&conn->mutex);
		}
		else
		{
			pthreadMutexUnlock(&conn->mutex);
			if (!try_final_callback)
				return NULL;
			if (!pts->cb_final_chunk || pts->final_done)
				return NULL;
			xcmd = pts->cb_final_chunk(pts, xcmd_iov, &xcmd_iovcnt);
			if (!xcmd)
				return NULL;
			xpuClientSendCommandIOV(conn, xcmd_iov, xcmd_iovcnt);
		}
		CHECK_FOR_INTERRUPTS();

		ev = WaitLatch(MyLatch,
					   WL_LATCH_SET |
					   WL_TIMEOUT |
					   WL_POSTMASTER_DEATH,
					   1000L,
					   PG_WAIT_EXTENSION);
		if (ev & WL_POSTMASTER_DEATH)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("Unexpected Postmaster dead")));
		pthreadMutexLock(&conn->mutex);
	}
	xcmd = __pickupNextXpuCommand(conn);
	pthreadMutexUnlock(&conn->mutex);

	return xcmd;
}

static XpuCommand *
__fetchNextXpuCommand(pgstromTaskState *pts)
{
	XpuConnection  *conn = pts->conn;
	XpuCommand	   *xcmd;
	struct iovec	xcmd_iov[10];
	int				xcmd_iovcnt;
	int				ev;

	while (!pts->scan_done)
	{
		CHECK_FOR_INTERRUPTS();

		pthreadMutexLock(&conn->mutex);
		/* device error checks */
		if (conn->errorbuf.errcode != ERRCODE_STROM_SUCCESS)
		{
			pthreadMutexUnlock(&conn->mutex);
			ereport(ERROR,
					(errcode(conn->errorbuf.errcode),
					 errmsg("%s:%d  %s",
							conn->errorbuf.filename,
							conn->errorbuf.lineno,
							conn->errorbuf.message),
					 errhint("device at %s, function at %s",
							 conn->devname,
							 conn->errorbuf.funcname)));
		}

		if ((conn->num_running_cmds +
			 conn->num_ready_cmds) < pgstrom_max_async_tasks &&
			(dlist_is_empty(&conn->ready_cmds_list) ||
			 conn->num_running_cmds < pgstrom_max_async_tasks / 2))
		{
			/*
			 * xPU service still has margin to enqueue new commands.
			 * If we have no ready commands or number of running commands
			 * are less than pg_strom.max_async_tasks/2, we try to load
			 * the next chunk and enqueue this command.
			 */
			pthreadMutexUnlock(&conn->mutex);
			xcmd = pts->cb_next_chunk(pts, xcmd_iov, &xcmd_iovcnt);
			if (!xcmd)
			{
				Assert(pts->scan_done);
				break;
			}
			xpuClientSendCommandIOV(conn, xcmd_iov, xcmd_iovcnt);
		}
		else if (!dlist_is_empty(&conn->ready_cmds_list))
		{
			xcmd = __pickupNextXpuCommand(conn);
			pthreadMutexUnlock(&conn->mutex);

			return xcmd;
		}
		else if (conn->num_running_cmds > 0)
		{
			/*
			 * This block means we already runs enough number of concurrent
			 * tasks, but none of them are already finished.
			 * So, let's wait for the response.
			 */
			ResetLatch(MyLatch);
			pthreadMutexUnlock(&conn->mutex);

			ev = WaitLatch(MyLatch,
						   WL_LATCH_SET |
						   WL_TIMEOUT |
						   WL_POSTMASTER_DEATH,
						   1000L,
						   PG_WAIT_EXTENSION);
			if (ev & WL_POSTMASTER_DEATH)
				ereport(FATAL,
						(errcode(ERRCODE_ADMIN_SHUTDOWN),
						 errmsg("Unexpected Postmaster dead")));
		}
		else
		{
			/*
			 * Unfortunately, we touched the threshold. Take a short wait
			 */
			pthreadMutexUnlock(&conn->mutex);
			pg_usleep(20000L);		/* 20ms */
		}
	}
	return __waitAndFetchNextXpuCommand(pts, true);
}

static XpuCommand *
pgstromScanChunkGpuCache(pgstromTaskState *pts,
						 struct iovec *xcmd_iov, int *xcmd_iovcnt)
{
	elog(ERROR, "not implemented yet");
	return NULL;
}

/*
 * pgstromScanNextTuple
 */
static TupleTableSlot *
pgstromScanNextTuple(pgstromTaskState *pts)
{
	TupleTableSlot *slot = pts->css.ss.ss_ScanTupleSlot;

	if (pgstromFetchFallbackTuple(pts, slot))
		return slot;
	for (;;)
	{
		kern_data_store *kds = pts->curr_kds;
		int64_t		index = pts->curr_index++;

		if (index < kds->nitems)
		{
			kern_tupitem   *tupitem = KDS_GET_TUPITEM(kds, index);

			pts->curr_htup.t_len = tupitem->t_len;
			pts->curr_htup.t_data = &tupitem->htup;
			return ExecStoreHeapTuple(&pts->curr_htup, slot, false);
		}
		if (++pts->curr_chunk < pts->curr_resp->u.results.chunks_nitems)
		{
			pts->curr_kds = (kern_data_store *)((char *)kds + kds->length);
			pts->curr_index = 0;
			continue;
		}
		return NULL;
	}
}

/*
 * fixup_inner_varnode
 *
 * Any var-nodes are rewritten at setrefs.c to indicate a particular item
 * on the cscan->custom_scan_tlist. However, inner expression must reference
 * the inner relation, so we need to fix up it again.
 */
typedef struct
{
	CustomScan *cscan;
	Plan	   *inner_plan;
} fixup_inner_varnode_context;

static Node *
__fixup_inner_varnode_walker(Node *node, void *data)
{
	fixup_inner_varnode_context *con = data;

	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *)node;
		List	   *tlist_dev = con->cscan->custom_scan_tlist;
		TargetEntry *tle;

		Assert(var->varno == INDEX_VAR &&
			   var->varattno >= 1 &&
			   var->varattno <= list_length(tlist_dev));
		tle = list_nth(tlist_dev, var->varattno - 1);
		return (Node *)makeVar(INNER_VAR,
							   tle->resorigcol,
							   var->vartype,
							   var->vartypmod,
							   var->varcollid,
							   0);
	}
	return expression_tree_mutator(node, __fixup_inner_varnode_walker, con);
}

static List *
fixup_inner_varnode(List *exprs, CustomScan *cscan, Plan *inner_plan)
{
	fixup_inner_varnode_context con;

	memset(&con, 0, sizeof(con));
	con.cscan = cscan;
	con.inner_plan = inner_plan;

	return (List *)__fixup_inner_varnode_walker((Node *)exprs, &con);
}

/*
 * __setupTaskStateRequestBuffer
 */
static void
__setupTaskStateRequestBuffer(pgstromTaskState *pts,
							  TupleDesc tdesc_src,
							  TupleDesc tdesc_dst,
							  char format)
{
	XpuCommand	   *xcmd;
	kern_data_store *kds;
	size_t			bufsz;
	size_t			off;

	initStringInfo(&pts->xcmd_buf);
	bufsz = MAXALIGN(offsetof(XpuCommand, u.scan.data));
	if (tdesc_src)
		bufsz += estimate_kern_data_store(tdesc_src);
	if (tdesc_dst)
		bufsz += estimate_kern_data_store(tdesc_dst);
	enlargeStringInfo(&pts->xcmd_buf, bufsz);

	xcmd = (XpuCommand *)pts->xcmd_buf.data;
	memset(xcmd, 0, offsetof(XpuCommand, u.scan.data));
	xcmd->magic = XpuCommandMagicNumber;
	if ((pts->task_kind & DEVTASK__SCAN) != 0)
		xcmd->tag = XpuCommandTag__XpuScanExec;
	else if ((pts->task_kind & DEVTASK__JOIN) != 0)
		xcmd->tag = XpuCommandTag__XpuJoinExec;
	else if ((pts->task_kind & DEVTASK__GROUPBY) != 0)
		xcmd->tag = XpuCommandTag__XpuGroupByExec;
	else
		elog(ERROR, "unsupported task kind: %08x", pts->task_kind);
	xcmd->length = bufsz;

	off = offsetof(XpuCommand, u.scan.data);
	if (tdesc_dst)
	{
		xcmd->u.scan.kds_dst_offset = off;
		kds  = (kern_data_store *)((char *)xcmd + off);
		off += setup_kern_data_store(kds, tdesc_dst, 0, KDS_FORMAT_ROW);
	}
	if (tdesc_src)
	{
		xcmd->u.scan.kds_src_offset = off;
		kds  = (kern_data_store *)((char *)xcmd + off);
		off += setup_kern_data_store(kds, tdesc_src, 0, format);
	}
	pts->xcmd_buf.len = off;
}

/*
 * pgstromExecInitTaskState
 */
void
pgstromExecInitTaskState(CustomScanState *node, EState *estate, int eflags)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	pgstromPlanInfo	*pp_info = pts->pp_info;
	CustomScan *cscan = (CustomScan *)pts->css.ss.ps.plan;
	Relation	rel = pts->css.ss.ss_currentRelation;
	TupleDesc	tupdesc_src = RelationGetDescr(rel);
	TupleDesc	tupdesc_dst;
	int			depth;
	List	   *tlist_dev = NIL;
	List	   *base_quals = NIL;
	ListCell   *lc;

	/* sanity checks */
	Assert(rel != NULL &&
		   outerPlanState(node) == NULL &&
		   innerPlanState(node) == NULL &&
		   pp_info->num_rels == list_length(cscan->custom_plans));
	/*
	 * PG-Strom supports:
	 * - regular relation with 'heap' access method
	 * - foreign-table with 'arrow_fdw' driver
	 */
	if (RelationGetForm(rel)->relkind == RELKIND_RELATION ||
		RelationGetForm(rel)->relkind == RELKIND_MATVIEW)
	{
		SMgrRelation smgr = RelationGetSmgr(rel);
		Oid			am_oid = RelationGetForm(rel)->relam;
		const char *kds_pathname = relpath(smgr->smgr_rnode, MAIN_FORKNUM);

		if (am_oid != HEAP_TABLE_AM_OID)
			elog(ERROR, "PG-Strom does not support table access method: %s",
				 get_am_name(am_oid));

		/* setup BRIN-index if any */
		pgstromBrinIndexExecBegin(pts,
								  pp_info->brin_index_oid,
								  pp_info->brin_index_conds,
								  pp_info->brin_index_quals);
		if ((pts->task_kind & DEVKIND__NVIDIA_GPU) != 0)
			pts->optimal_gpus = GetOptimalGpuForRelation(rel);
		if ((pts->task_kind & DEVKIND__NVIDIA_DPU) != 0)
			pts->ds_entry = GetOptimalDpuForRelation(rel, &kds_pathname);
		pts->kds_pathname = kds_pathname;
	}
	else if (RelationGetForm(rel)->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (!pgstromArrowFdwExecInit(pts,
									 pp_info->scan_quals,
									 pp_info->outer_refs))
			elog(ERROR, "Bug? only arrow_fdw is supported in PG-Strom");
	}
	else
	{
		elog(ERROR, "Bug? PG-Strom does not support relation type of '%s'",
			 RelationGetRelationName(rel));
	}

	/*
	 * Re-initialization of scan tuple-descriptor and projection-info,
	 * because commit 1a8a4e5cde2b7755e11bde2ea7897bd650622d3e of
	 * PostgreSQL makes to assign result of ExecTypeFromTL() instead
	 * of ExecCleanTypeFromTL; that leads incorrect projection.
	 * So, we try to remove junk attributes from the scan-descriptor.
	 *
	 * And, device projection returns a tuple in heap-format, so we
	 * prefer TTSOpsHeapTuple, instead of the TTSOpsVirtual.
	 */
	tupdesc_dst = ExecCleanTypeFromTL(cscan->custom_scan_tlist);
	ExecInitScanTupleSlot(estate, &pts->css.ss, tupdesc_dst,
						  &TTSOpsHeapTuple);
	ExecAssignScanProjectionInfoWithVarno(&pts->css.ss, INDEX_VAR);

	/*
	 * Init resources for CPU fallbacks
	 */
	base_quals = (List *)
		fixup_varnode_to_origin((Node *)pp_info->scan_quals,
								cscan->custom_scan_tlist);
	pts->base_quals = ExecInitQual(base_quals, &pts->css.ss.ps);
	foreach (lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (!tle->resjunk)
			tlist_dev = lappend(tlist_dev, tle);
	}
	pts->base_slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
											  table_slot_callbacks(rel));
	pts->base_proj = ExecBuildProjectionInfo(tlist_dev,
											 pts->css.ss.ps.ps_ExprContext,
											 pts->css.ss.ss_ScanTupleSlot,
											 &pts->css.ss.ps,
											 RelationGetDescr(rel));
	/*
	 * init inner relations
	 */
	depth = 1;
	foreach (lc, cscan->custom_plans)
	{
		pgstromTaskInnerState *istate = &pts->inners[depth-1];
		pgstromPlanInnerInfo *pp_inner = &pp_info->inners[depth-1];
		Plan	   *plan = lfirst(lc);
		PlanState  *ps = ExecInitNode(plan, estate, eflags);
		List	   *hash_inner_keys;
		ListCell   *cell;
		devtype_info *dtype;

		istate->ps = ps;
		istate->econtext = CreateExprContext(estate);
		istate->depth = depth;
		istate->join_type = pp_inner->join_type;
		istate->join_quals = ExecInitQual(pp_inner->join_quals,
										  &pts->css.ss.ps);
		istate->other_quals = ExecInitQual(pp_inner->other_quals,
										   &pts->css.ss.ps);
		foreach (cell, pp_inner->hash_outer_keys)
		{
			ExprState  *es = ExecInitExpr((Expr *)lfirst(cell),
										  &pts->css.ss.ps);
			dtype = pgstrom_devtype_lookup(exprType((Node *)es->expr));
			if (!dtype)
				elog(ERROR, "failed on lookup device type of %s",
					 nodeToString(es->expr));
			istate->hash_outer_keys = lappend(istate->hash_outer_keys, es);
			istate->hash_outer_dtypes = lappend(istate->hash_outer_dtypes, dtype);
		}
		/* inner hash-keys references the result of inner-slot */
		hash_inner_keys = fixup_inner_varnode(pp_inner->hash_inner_keys,
											  cscan, plan);
		foreach (cell, hash_inner_keys)
		{
			ExprState  *es = ExecInitExpr((Expr *)lfirst(cell),
										  &pts->css.ss.ps);
			dtype = pgstrom_devtype_lookup(exprType((Node *)es->expr));
			if (!dtype)
				elog(ERROR, "failed on lookup device type of %s",
					 nodeToString(es->expr));
			istate->hash_inner_keys = lappend(istate->hash_inner_keys, es);
			istate->hash_inner_dtypes = lappend(istate->hash_inner_dtypes, dtype);
		}

		if (OidIsValid(pp_inner->gist_index_oid))
		{
			istate->gist_irel = index_open(pp_inner->gist_index_oid,
										   AccessShareLock);
			istate->gist_clause = ExecInitExpr((Expr *)pp_inner->gist_clause,
											   &pts->css.ss.ps);
		}
		pts->css.custom_ps = lappend(pts->css.custom_ps, ps);
		depth++;
	}
	
	/*
	 * Setup request buffer
	 */
	if (pts->arrow_state)		/* Apache Arrow */
	{
		pts->cb_next_chunk = pgstromScanChunkArrowFdw;
		pts->cb_next_tuple = pgstromScanNextTuple;
	    __setupTaskStateRequestBuffer(pts,
									  NULL,
									  tupdesc_dst,
									  KDS_FORMAT_ARROW);
	}
	else if (pts->gcache_state)		/* GPU-Cache */
	{
		pts->cb_next_chunk = pgstromScanChunkGpuCache;
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  NULL,
									  tupdesc_dst,
									  KDS_FORMAT_COLUMN);
	}
	else if (!bms_is_empty(pts->optimal_gpus) ||	/* GPU-Direct SQL */
			 pts->ds_entry)							/* DPU Storage */
	{
		pts->cb_next_chunk = pgstromRelScanChunkDirect;
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  tupdesc_src,
									  tupdesc_dst,
									  KDS_FORMAT_BLOCK);
	}
	else						/* Slow normal heap storage */
	{
		pts->cb_next_chunk = pgstromRelScanChunkNormal;
		pts->cb_next_tuple = pgstromScanNextTuple;
		__setupTaskStateRequestBuffer(pts,
									  tupdesc_src,
									  tupdesc_dst,
									  KDS_FORMAT_ROW);
	}
	/* CPU fallback routine */
	if ((pts->task_kind & DEVTASK__SCAN) != 0)
		pts->cb_cpu_fallback = ExecFallbackCpuScan;
	else if ((pts->task_kind & DEVTASK__JOIN) != 0)
		pts->cb_cpu_fallback = ExecFallbackCpuJoin;
	else if ((pts->task_kind & DEVTASK__GROUPBY) != 0)
		pts->cb_cpu_fallback = ExecFallbackCpuGroupBy;
	else
		elog(ERROR, "Bug? unknown DEVTASK");
	/* other fields init */
	pts->curr_vm_buffer = InvalidBuffer;
}

/*
 * pgstromExecScanAccess
 */
static TupleTableSlot *
pgstromExecScanAccess(pgstromTaskState *pts)
{
	TupleTableSlot *slot = NULL;
	XpuCommand	   *resp;

	while (!pts->curr_resp || !(slot = pts->cb_next_tuple(pts)))
	{
	next_chunks:
		if (pts->curr_resp)
			xpuClientPutResponse(pts->curr_resp);
		pts->curr_resp = __fetchNextXpuCommand(pts);
		if (!pts->curr_resp)
			return NULL;
		resp = pts->curr_resp;
		if (resp->tag == XpuCommandTag__Success)
		{
			if (resp->u.results.chunks_nitems == 0)
				goto next_chunks;
			pts->curr_kds = (kern_data_store *)
				((char *)resp + resp->u.results.chunks_offset);
			pts->curr_chunk = 0;
			pts->curr_index = 0;
		}
		else
		{
			Assert(resp->tag == XpuCommandTag__CPUFallback);
			//run CPU fallback
			//attach alternative KDS
			elog(ERROR, "CPU fallback is not ready");
		}
	}
	return slot;
}

/*
 * pgstromExecScanReCheck
 */
static bool
pgstromExecScanReCheck(pgstromTaskState *pts, TupleTableSlot *epq_slot)
{
	/*
	 * NOTE: Only immutable operators/functions are executable
	 * on the GPU devices, so its decision will never changed.
	 */
	return true;
}

TupleTableSlot *
pgstromExecTaskState(pgstromTaskState *pts)
{
	return ExecScan(&pts->css.ss,
					(ExecScanAccessMtd) pgstromExecScanAccess,
					(ExecScanRecheckMtd) pgstromExecScanReCheck);
}

/*
 * pgstromExecEndTaskState
 */
void
pgstromExecEndTaskState(CustomScanState *node)
{
	pgstromTaskState   *pts = (pgstromTaskState *)node;
	pgstromSharedState *ps_state = pts->ps_state;
	ListCell   *lc;

	if (pts->curr_vm_buffer != InvalidBuffer)
		ReleaseBuffer(pts->curr_vm_buffer);
	if (pts->conn)
		xpuClientCloseSession(pts->conn);
	if (pts->br_state)
		pgstromBrinIndexExecEnd(pts);
	if (pts->arrow_state)
		pgstromArrowFdwExecEnd(pts->arrow_state);
	if (pts->base_slot)
		ExecDropSingleTupleTableSlot(pts->base_slot);
	if (pts->css.ss.ss_currentScanDesc)
		table_endscan(pts->css.ss.ss_currentScanDesc);
	if (pts->h_kmrels)
		__munmapShmem(pts->h_kmrels);
	if (!IsParallelWorker())
	{
		if (ps_state && ps_state->preload_shmem_handle != 0)
			__shmemDrop(ps_state->preload_shmem_handle);
	}
	foreach (lc, pts->css.custom_ps)
		ExecEndNode((PlanState *) lfirst(lc));
}

/*
 * pgstromExecResetTaskState
 */
void
pgstromExecResetTaskState(CustomScanState *node)
{
	pgstromTaskState *pts = (pgstromTaskState *) node;

	if (pts->conn)
	{
		xpuClientCloseSession(pts->conn);
		pts->conn = NULL;
	}
	if (pts->br_state)
		pgstromBrinIndexExecReset(pts);
	if (pts->arrow_state)
		pgstromArrowFdwExecReset(pts->arrow_state);
}

/*
 * pgstromSharedStateEstimateDSM
 */
Size
pgstromSharedStateEstimateDSM(CustomScanState *node,
							  ParallelContext *pcxt)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	Relation	relation = node->ss.ss_currentRelation;
	EState	   *estate   = node->ss.ps.state;
	Snapshot	snapshot = estate->es_snapshot;
	int			num_rels = list_length(node->custom_ps);
	Size		len = 0;

	if (pts->br_state)
		len += pgstromBrinIndexEstimateDSM(pts);
	len += MAXALIGN(offsetof(pgstromSharedState, inners[num_rels]));
	if (!pts->arrow_state)
		len += table_parallelscan_estimate(relation, snapshot);
	return MAXALIGN(len);
}

/*
 * pgstromSharedStateInitDSM
 */
void
pgstromSharedStateInitDSM(CustomScanState *node,
						  ParallelContext *pcxt,
						  void *coordinate)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	Relation	relation = node->ss.ss_currentRelation;
	EState	   *estate   = node->ss.ps.state;
	Snapshot	snapshot = estate->es_snapshot;
	int			num_rels = list_length(node->custom_ps);
	size_t		dsm_length = offsetof(pgstromSharedState, inners[num_rels]);
	char	   *dsm_addr = coordinate;
	pgstromSharedState *ps_state;
	TableScanDesc scan = NULL;

	Assert(!IsBackgroundWorker);
	if (pts->br_state)
		dsm_addr += pgstromBrinIndexInitDSM(pts, dsm_addr);
	Assert(!pts->css.ss.ss_currentScanDesc);
	if (dsm_addr)
	{
		ps_state = (pgstromSharedState *) dsm_addr;
		memset(ps_state, 0, dsm_length);
		ps_state->ss_handle = dsm_segment_handle(pcxt->seg);
		ps_state->ss_length = dsm_length;
		dsm_addr += MAXALIGN(dsm_length);
		if (pts->arrow_state)
			pgstromArrowFdwInitDSM(pts->arrow_state, ps_state);
		else
		{
			ParallelTableScanDesc pdesc = (ParallelTableScanDesc) dsm_addr;

			table_parallelscan_initialize(relation, pdesc, snapshot);
			scan = table_beginscan_parallel(relation, pdesc);
		}
	}
	else
	{
		ps_state = MemoryContextAllocZero(estate->es_query_cxt, dsm_length);
		ps_state->ss_handle = DSM_HANDLE_INVALID;
		ps_state->ss_length = dsm_length;
		if (pts->arrow_state)
			pgstromArrowFdwInitDSM(pts->arrow_state, ps_state);
		else
			scan = table_beginscan(relation, estate->es_snapshot, 0, NULL);
	}
	ps_state->num_rels = num_rels;
	ConditionVariableInit(&ps_state->preload_cond);
	SpinLockInit(&ps_state->preload_mutex);
	if (num_rels > 0)
		ps_state->preload_shmem_handle = __shmemCreate(pts->ds_entry);
	pts->ps_state = ps_state;
	pts->css.ss.ss_currentScanDesc = scan;
}

/*
 * pgstromSharedStateAttachDSM
 */
void
pgstromSharedStateAttachDSM(CustomScanState *node,
							shm_toc *toc,
							void *coordinate)
{
	pgstromTaskState *pts = (pgstromTaskState *)node;
	char	   *dsm_addr = coordinate;
	int			num_rels = list_length(pts->css.custom_ps);

	if (pts->br_state)
		dsm_addr += pgstromBrinIndexAttachDSM(pts, dsm_addr);
	pts->ps_state = (pgstromSharedState *)dsm_addr;
	Assert(pts->ps_state->num_rels == num_rels);
	dsm_addr += MAXALIGN(offsetof(pgstromSharedState, inners[num_rels]));

	if (pts->arrow_state)
		pgstromArrowFdwAttachDSM(pts->arrow_state, pts->ps_state);
	else
	{
		Relation	relation = pts->css.ss.ss_currentRelation;
		ParallelTableScanDesc pdesc = (ParallelTableScanDesc) dsm_addr;

		pts->css.ss.ss_currentScanDesc = table_beginscan_parallel(relation, pdesc);
	}
}

/*
 * pgstromSharedStateShutdownDSM
 */
void
pgstromSharedStateShutdownDSM(CustomScanState *node)
{
	pgstromTaskState   *pts = (pgstromTaskState *) node;
	pgstromSharedState *src_state = pts->ps_state;
	pgstromSharedState *dst_state;
	EState			   *estate = node->ss.ps.state;

	if (pts->br_state)
		pgstromBrinIndexShutdownDSM(pts);
	if (pts->arrow_state)
		pgstromArrowFdwShutdown(pts->arrow_state);
	if (src_state)
	{
		size_t	sz = offsetof(pgstromSharedState,
							  inners[src_state->num_rels]);
		dst_state = MemoryContextAllocZero(estate->es_query_cxt, sz);
		memcpy(dst_state, src_state, sz);
		pts->ps_state = dst_state;
	}
}

/*
 * pgstromExplainTaskState
 */
void
pgstromExplainTaskState(CustomScanState *node,
						List *ancestors,
						ExplainState *es)
{
	pgstromTaskState   *pts = (pgstromTaskState *)node;
	pgstromPlanInfo	   *pp_info = pts->pp_info;
	CustomScan		   *cscan = (CustomScan *)node->ss.ps.plan;
	List			   *dcontext;
	StringInfoData		buf;
	ListCell		   *lc;
	const char		   *xpu_label;
	char				label[100];
	char			   *str;
	double				ntuples;

	/* setup deparse context */
	dcontext = set_deparse_context_plan(es->deparse_cxt,
										node->ss.ps.plan,
										ancestors);
	if ((pts->task_kind & DEVKIND__NVIDIA_GPU) != 0)
		xpu_label = "GPU";
	else if ((pts->task_kind & DEVKIND__NVIDIA_DPU) != 0)
		xpu_label = "DPU";
	else
		xpu_label = "???";

	/* xPU Projection */
	initStringInfo(&buf);
	foreach (lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (tle->resjunk)
			continue;
		str = deparse_expression((Node *)tle->expr, dcontext, false, true);
		if (buf.len > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, str);
	}
	snprintf(label, sizeof(label),
			 "%s Projection", xpu_label);
	ExplainPropertyText(label, buf.data, es);

	/* xPU Scan Quals */
	if (pp_info->scan_quals)
	{
		List   *scan_quals = pp_info->scan_quals;
		Expr   *expr;

		resetStringInfo(&buf);
		if (list_length(scan_quals) > 1)
			expr = make_andclause(scan_quals);
		else
			expr = linitial(scan_quals);
		str = deparse_expression((Node *)expr, dcontext, false, true);
		appendStringInfo(&buf, "%s [rows: %.0f -> %.0f]",
						 str,
						 pp_info->scan_tuples,
						 pp_info->scan_rows);
		snprintf(label, sizeof(label), "%s Scan Quals", xpu_label);
		ExplainPropertyText(label, buf.data, es);
	}

	/* xPU JOIN */
	ntuples = pp_info->scan_rows;
	for (int i=0; i < pp_info->num_rels; i++)
	{
		pgstromPlanInnerInfo *pp_inner = &pp_info->inners[i];

		if (pp_inner->join_quals != NIL || pp_inner->other_quals != NIL)
		{
			const char *join_label;

			resetStringInfo(&buf);
			foreach (lc, pp_inner->join_quals)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, false, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			if (pp_inner->other_quals != NIL)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, false, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfo(&buf, "[%s]", str);
			}
			appendStringInfo(&buf, " ... [nrows: %.0f -> %.0f]",
							 ntuples, pp_inner->join_nrows);
			switch (pp_inner->join_type)
			{
				case JOIN_INNER: join_label = "Join"; break;
				case JOIN_LEFT:  join_label = "Left Outer Join"; break;
				case JOIN_RIGHT: join_label = "Right Outer Join"; break;
				case JOIN_FULL:  join_label = "Full Outer Join"; break;
				case JOIN_SEMI:  join_label = "Semi Join"; break;
				case JOIN_ANTI:  join_label = "Anti Join"; break;
				default:         join_label = "??? Join"; break;
			}
			snprintf(label, sizeof(label),
					 "%s %s Quals [%d]", xpu_label, join_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		ntuples = pp_inner->join_nrows;

		if (pp_inner->hash_outer_keys != NIL)
		{
			resetStringInfo(&buf);
			foreach (lc, pp_inner->hash_outer_keys)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, true, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			snprintf(label, sizeof(label),
					 "%s Outer Hash [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		if (pp_inner->hash_inner_keys != NIL)
		{
			resetStringInfo(&buf);
			foreach (lc, pp_inner->hash_inner_keys)
			{
				Node   *expr = lfirst(lc);

				str = deparse_expression(expr, dcontext, true, true);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, str);
			}
			snprintf(label, sizeof(label),
					 "%s Inner Hash [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
		if (pp_inner->gist_clause)
		{
			char   *idxname = get_rel_name(pp_inner->gist_index_oid);
			char   *colname = get_attname(pp_inner->gist_index_oid,
										  pp_inner->gist_index_col, false);
			resetStringInfo(&buf);

			str = deparse_expression(pp_inner->gist_clause,
									 dcontext, false, true);
			appendStringInfoString(&buf, str);
			if (idxname && colname)
				appendStringInfo(&buf, " on %s (%s)", idxname, colname);
			snprintf(label, sizeof(label),
					 "%s GiST Join [%d]", xpu_label, i+1);
			ExplainPropertyText(label, buf.data, es);
		}
	}

	/*
	 * Storage related info
	 */
	if (pts->arrow_state)
	{
		pgstromArrowFdwExplain(pts->arrow_state,
							   pts->css.ss.ss_currentRelation,
							   es, dcontext);
	}
	else if (pts->gcache_state)
	{
		/* GPU-Cache */
	}
	else if (!bms_is_empty(pts->optimal_gpus))
	{
		/* GPU-Direct */
		resetStringInfo(&buf);
		if (!es->analyze)
		{
			bool	is_first = true;
			int		k;

			appendStringInfo(&buf, "enabled (");
			for (k = bms_next_member(pts->optimal_gpus, -1);
				 k >= 0;
				 k = bms_next_member(pts->optimal_gpus, k))
			{
				if (!is_first)
					appendStringInfo(&buf, ", ");
				appendStringInfo(&buf, "GPU-%d", k);
				is_first = false;
			}
			appendStringInfo(&buf, ")");
		}
		else
		{
			pgstromSharedState *ps_state = pts->ps_state;
			XpuConnection  *conn = pts->conn;
			uint32			count;

			count = pg_atomic_read_u32(&ps_state->heap_direct_nblocks);
			appendStringInfo(&buf, "enabled (%s; direct=%u", conn->devname, count);
			count = pg_atomic_read_u32(&ps_state->heap_normal_nblocks);
			if (count > 0)
				appendStringInfo(&buf, ", buffer=%u", count);
			count = pg_atomic_read_u32(&ps_state->heap_fallback_nblocks);
			if (count > 0)
				appendStringInfo(&buf, ", fallback=%u", count);
			appendStringInfo(&buf, ")");
		}
		ExplainPropertyText("GPU-Direct SQL", buf.data, es);
	}
	else if (pts->ds_entry)
	{
		/* DPU-Entry */
		explainDpuStorageEntry(pts->ds_entry, es);
	}
	else
	{
		/* Normal Heap Storage */
	}
	/* State of BRIN-index */
	if (pts->br_state)
		pgstromBrinIndexExplain(pts, dcontext, es);

	/*
	 * Dump the XPU code (only if verbose)
	 */
	if (es->verbose)
	{
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Scan VarLoads OpCode",
								pp_info->kexp_scan_kvars_load);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Scan Quals OpCode",
								pp_info->kexp_scan_quals);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Join VarLoads OpCode",
								pp_info->kexp_join_kvars_load_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Join Quals OpCode",
								pp_info->kexp_join_quals_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"Join HashValue OpCode",
								pp_info->kexp_hash_keys_packed);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								"GiST-Index Join OpCode",
								pp_info->kexp_gist_quals_packed);
		snprintf(label, sizeof(label),
				 "%s Projection OpCode", xpu_label);
		pgstrom_explain_xpucode(&pts->css, es, dcontext,
								label,
								pp_info->kexp_projection);
	}
	pfree(buf.data);
}

/*
 * __xpuClientOpenSession
 */
void
__xpuClientOpenSession(pgstromTaskState *pts,
					   const XpuCommand *session,
					   pgsocket sockfd,
					   const char *devname)
{
	XpuConnection  *conn;
	XpuCommand	   *resp;
	int				rv;

	Assert(!pts->conn);
	conn = calloc(1, sizeof(XpuConnection));
	if (!conn)
	{
		close(sockfd);
		elog(ERROR, "out of memory");
	}
	strncpy(conn->devname, devname, 32);
	conn->sockfd = sockfd;
	conn->resowner = CurrentResourceOwner;
	conn->worker = pthread_self();	/* to be over-written by worker's-id */
	pthreadMutexInit(&conn->mutex);
	conn->num_running_cmds = 0;
	conn->num_ready_cmds = 0;
	dlist_init(&conn->ready_cmds_list);
	dlist_init(&conn->active_cmds_list);
	dlist_push_tail(&xpu_connections_list, &conn->chain);
	pts->conn = conn;

	/*
	 * Ok, sockfd and conn shall be automatically released on ereport()
	 * after that.
	 */
	if ((rv = pthread_create(&conn->worker, NULL,
							 __xpuConnectSessionWorker, conn)) != 0)
		elog(ERROR, "failed on pthread_create: %s", strerror(rv));

	/*
	 * Initialize the new session
	 */
	Assert(session->tag == XpuCommandTag__OpenSession);
	xpuClientSendCommand(conn, session);
	resp = __waitAndFetchNextXpuCommand(pts, false);
	if (!resp)
		elog(ERROR, "Bug? %s:OpenSession response is missing", conn->devname);
	if (resp->tag != XpuCommandTag__Success)
		elog(ERROR, "%s:OpenSession failed - %s (%s:%d %s)",
			 conn->devname,
			 resp->u.error.message,
			 resp->u.error.filename,
			 resp->u.error.lineno,
			 resp->u.error.funcname);
	xpuClientPutResponse(resp);
}

/*
 * pgstrom_request_executor
 */
static void
pgstrom_request_executor(void)
{
	if (shmem_request_next)
		(*shmem_request_next)();
	
	RequestAddinShmemSpace(MAXALIGN(sizeof(pg_atomic_uint32)));
}

/*
 * pgstrom_startup_executor
 */
static void
pgstrom_startup_executor(void)
{
	bool	found;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	pgstrom_query_plan_id = ShmemInitStruct("pgstrom_query_plan_id",
											MAXALIGN(sizeof(pg_atomic_uint32)),
											&found);
	if (!found)
		pg_atomic_init_u32(pgstrom_query_plan_id, 0);
}

/*
 * pgstrom_init_executor
 */
void
pgstrom_init_executor(void)
{
    DefineCustomBoolVariable("pg_strom.use_debug_code",
							 "Use debug-mode enabled device code",
							 NULL,
							 &pgstrom_use_debug_code,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE | GUC_SUPERUSER_ONLY,
							 NULL, NULL, NULL);
	dlist_init(&xpu_connections_list);
	RegisterResourceReleaseCallback(xpuclientCleanupConnections, NULL);
	/* shared memory setup */
	shmem_request_next = shmem_request_hook;
	shmem_request_hook = pgstrom_request_executor;
	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_executor;
}
