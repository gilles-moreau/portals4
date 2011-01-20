/*
 * ptl_rpc.h
 */

#ifndef PTL_RPC_H
#define PTL_RPC_H

#define PTL_CTL_PORT		(0x3456)

/*
 * messages 
 */
struct rpc_msg {
	enum {
		INVALID,
		QUERY_INIT_DATA = 1,
		QUERY_XRC_DOMAIN = 2,

		/* 8th bit indicate a reply. */
		REPLY_INIT_DATA = 128,
		REPLY_XRC_DOMAIN = 129,
	} type;

	uint32_t sequence;

	union {
		/* QUERY_INIT_DATA */
		struct {
		} query_init_data;

		/* REPLY_INIT_DATA */
		struct {
			/* Shared memory */
			char shmem_filename[1024];
			size_t shmem_filesize;
		} reply_init_data;

		/* QUERY_XRC_DOMAIN */
		struct {
			char net_name[50];		/* network interface name, eg. ib0 */
		} query_xrc_domain;

		/* REPLY_XRC_DOMAIN */
		struct {
			/* file name for the XRC domain. */
			char xrc_domain_fname[1024];
			char ib_name[50];
		} reply_xrc_domain;
	};
};

/*
 * type of end point
 */
enum rpc_type {
	rpc_type_server,
	rpc_type_client,
};

/*
 * per rpc session info
 */
struct session {
	struct list_head		session_list;
	int				fd;			/* connected socket */
	uint32_t			sequence;
	pthread_mutex_t			mutex;
	pthread_cond_t			cond;
	struct rpc_msg			rpc_msg;
};

/*
 * per rpc end point info
 */
struct rpc {
	enum rpc_type			type;
	int				fd;			/* listening socket */

	pthread_t			io_thread;
	int				io_thread_run;

	/* List of all active sessions. */
	struct list_head		session_list;
	struct session			*to_server; /* for client only */
	pthread_spinlock_t		session_list_lock;

	void (*callback)(struct session *session);
};

int rpc_init(enum rpc_type type, unsigned int ctl_port, struct rpc **rpc_p,
			 void (*callback)(struct session *session));
int rpc_fini(struct rpc *rpc);
//int rpc_get_pid(gbl_t *gbl);
int rpc_get(struct session *session,
			struct rpc_msg *msg_in, struct rpc_msg *msg_out);
int rpc_send(struct session *session, struct rpc_msg *msg_out);

#endif /* RPC_PTL_H */