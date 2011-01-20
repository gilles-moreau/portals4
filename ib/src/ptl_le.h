#ifndef PTL_LE_H
#define PTL_LE_H

struct ct;

extern obj_type_t *type_le;

#define TYPE_LE			(0)

/*
 * common struct members between LE and ME objects
 */
#define PTL_LE_OBJ				\
	int			type;		\
	struct list_head	list;		\
	ptl_pt_index_t		pt_index;	\
	pt_t			*pt;		\
	ptl_list_t		ptl_list;	\
	void			*user_ptr;	\
	void			*start;		\
	unsigned int		num_iov;	\
	ptl_size_t		length;		\
	struct ct		*ct;		\
	union {					\
	uint32_t		uid;		\
	uint32_t		jid;		\
	};					\
	unsigned int		options;	\
	mr_t			*mr;		\
	mr_t			**mr_list;	\
	struct ibv_sge		*sge_list;

typedef struct le {
	PTL_BASE_OBJ
	PTL_LE_OBJ
} le_t;

void le_init(void *arg);
void le_release(void *arg);

static inline int le_alloc(ni_t *ni, le_t **le_p)
{
	return obj_alloc(type_le, (obj_t *)ni, (obj_t **)le_p);
}

static inline int le_get(ptl_handle_le_t handle, le_t **le_p)
{
	return obj_get(type_le, (ptl_handle_any_t)handle, (obj_t **)le_p);
}

static inline void le_ref(le_t *le)
{
	obj_ref((obj_t *)le);
}

static inline int le_put(le_t *le)
{
	return obj_put((obj_t *)le);
}

static inline ptl_handle_le_t le_to_handle(le_t *le)
{
        return (ptl_handle_le_t)le->obj_handle;
}

int le_append_check(int type, ni_t *ni, ptl_pt_index_t pt_index,
		    ptl_le_t *le_init, ptl_list_t ptl_list,
		    ptl_handle_le_t *le_handle);

int le_get_mr(ni_t *ni, ptl_le_t *le_init, le_t *le);

int le_append_pt(ni_t *ni, le_t *le);

void le_unlink(le_t *le);

#endif /* PTL_LE_H */