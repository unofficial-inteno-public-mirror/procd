#include <libubox/avl-cmp.h>
#include "procd.h"
#include "service.h"

struct avl_tree services;
static struct blob_buf b;

static void
start_instance(struct service_instance *in)
{
	in->restart = false;
}

static void
instance_timeout(struct uloop_timeout *t)
{
	struct service_instance *in;

	in = container_of(t, struct service_instance, timeout);
	kill(in->proc.pid, SIGKILL);
	uloop_process_delete(&in->proc);
	in->proc.cb(&in->proc, -1);
}

static void
instance_exit(struct uloop_process *p, int ret)
{
	struct service_instance *in;

	in = container_of(p, struct service_instance, proc);
	uloop_timeout_cancel(&in->timeout);
	if (in->restart)
		start_instance(in);
}

static void
stop_instance(struct service_instance *in, bool restart)
{
	if (!in->proc.pending)
		return;

	kill(in->proc.pid, SIGTERM);
}

static bool
instance_config_changed(struct service_instance *in, struct service_instance *in_new)
{
	int len = blob_pad_len(in->config);

	if (len != blob_pad_len(in_new->config))
		return true;

	if (memcmp(in->config, in_new->config, blob_pad_len(in->config)) != 0)
		return true;

	return false;
}

static bool
update_instance(struct service_instance *in, struct service_instance *in_new)
{
	bool changed = instance_config_changed(in, in_new);

	in->config = in_new->config;
	if (!changed)
		return false;

	stop_instance(in, true);
	return true;
}

static void
free_instance(struct service_instance *in)
{
	uloop_process_delete(&in->proc);
	uloop_timeout_cancel(&in->timeout);
	free(in);
}

static void
init_instance(struct service_instance *in, struct blob_attr *config)
{
	in->config = config;
	in->timeout.cb = instance_timeout;
	in->proc.cb = instance_exit;
}

static void
service_instance_add(struct service *s, struct blob_attr *attr)
{
	struct service_instance *in;
	const char *name = blobmsg_name(attr);

	if (blobmsg_type(attr) != BLOBMSG_TYPE_TABLE)
		return;

	in = calloc(1, sizeof(*in));
	if (!in)
		return;

	init_instance(in, attr);
	vlist_add(&s->instances, &in->node, (void *) name);
}

static void
service_instance_update(struct vlist_tree *tree, struct vlist_node *node_new,
			struct vlist_node *node_old)
{
	struct service_instance *in_o = NULL, *in_n = NULL;

	if (node_old)
		in_o = container_of(node_old, struct service_instance, node);

	if (node_new)
		in_n = container_of(node_new, struct service_instance, node);

	if (in_o && in_n) {
		update_instance(in_o, in_n);
		free_instance(in_n);
	} else if (in_o) {
		stop_instance(in_o, false);
		free_instance(in_o);
	} else if (in_n) {
		start_instance(in_n);
	}
}

static struct service *
service_alloc(const char *name)
{
	struct service *s;

	s = calloc(1, sizeof(*s));
	vlist_init(&s->instances, avl_strcmp, service_instance_update);
	s->instances.keep_old = true;

	return s;
}

enum {
	SERVICE_ATTR_NAME,
	SERVICE_ATTR_SCRIPT,
	SERVICE_ATTR_INSTANCES,
	__SERVICE_ATTR_MAX
};

static const struct blobmsg_policy service_attrs[__SERVICE_ATTR_MAX] = {
	[SERVICE_ATTR_NAME] = { "name", BLOBMSG_TYPE_STRING },
	[SERVICE_ATTR_SCRIPT] = { "script", BLOBMSG_TYPE_STRING },
	[SERVICE_ATTR_INSTANCES] = { "instances", BLOBMSG_TYPE_TABLE },
};


static int
service_update(struct service *s, struct blob_attr *config, struct blob_attr **tb)
{
	struct blob_attr *old_config = s->config;
	struct blob_attr *cur;
	int rem;

	/* only the pointer changes, the content stays the same,
	 * no avl update necessary */
	s->name = s->avl.key = blobmsg_data(tb[SERVICE_ATTR_NAME]);
	s->config = config;

	if (tb[SERVICE_ATTR_INSTANCES]) {
		vlist_update(&s->instances);
		blobmsg_for_each_attr(cur, tb[SERVICE_ATTR_INSTANCES], rem) {
			service_instance_add(s, cur);
		}
		vlist_flush(&s->instances);
	}

	free(old_config);

	return 0;
}

static void
service_delete(struct service *s)
{
	vlist_flush_all(&s->instances);
	avl_delete(&services, &s->avl);
	free(s->config);
	free(s);
}

static int
service_handle_set(struct ubus_context *ctx, struct ubus_object *obj,
		   struct ubus_request_data *req, const char *method,
		   struct blob_attr *msg)
{
	struct blob_attr *tb[__SERVICE_ATTR_MAX], *cur;
	struct service *s = NULL;
	const char *name;
	int ret = UBUS_STATUS_INVALID_ARGUMENT;

	msg = blob_memdup(msg);
	if (!msg)
		return UBUS_STATUS_UNKNOWN_ERROR;

	blobmsg_parse(service_attrs, __SERVICE_ATTR_MAX, tb, blob_data(msg), blob_len(msg));
	cur = tb[SERVICE_ATTR_NAME];
	if (!cur)
		goto free;

	name = blobmsg_data(cur);

	s = avl_find_element(&services, name, s, avl);
	if (s)
		return service_update(s, msg, tb);

	s = service_alloc(name);
	if (!s)
		return UBUS_STATUS_UNKNOWN_ERROR;

	ret = service_update(s, msg, tb);
	if (ret)
		goto free;

	avl_insert(&services, &s->avl);

	return 0;

free:
	free(msg);
	return ret;
}

static int
service_handle_list(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct service *s;

	blob_buf_init(&b, 0);
	avl_for_each_element(&services, s, avl) {
		void *c;

		c = blobmsg_open_table(&b, s->name);
		blobmsg_close_table(&b, c);
	}

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	SERVICE_DEL_NAME,
	__SERVICE_DEL_MAX,
};

static const struct blobmsg_policy service_del_attrs[__SERVICE_DEL_MAX] = {
	[SERVICE_DEL_NAME] = { "name", BLOBMSG_TYPE_STRING },
};


static int
service_handle_delete(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__SERVICE_DEL_MAX], *cur;
	struct service *s, *tmp;

	blobmsg_parse(service_del_attrs, __SERVICE_DEL_MAX, tb, blob_data(msg), blob_len(msg));

	cur = tb[SERVICE_ATTR_NAME];
	if (!cur) {
		avl_for_each_element_safe(&services, s, avl, tmp)
			service_delete(s);
		return 0;
	}

	s = avl_find_element(&services, blobmsg_data(cur), s, avl);
	if (!s)
		return UBUS_STATUS_NOT_FOUND;

	service_delete(s);
	return 0;
}

static struct ubus_method main_object_methods[] = {
	{ .name = "list", .handler = service_handle_list },
	{ .name = "set", .handler = service_handle_set },
	{ .name = "delete", .handler = service_handle_delete },
};

static struct ubus_object_type main_object_type =
	UBUS_OBJECT_TYPE("service", main_object_methods);

static struct ubus_object main_object = {
	.name = "service",
	.type = &main_object_type,
	.methods = main_object_methods,
	.n_methods = ARRAY_SIZE(main_object_methods),
};

void procd_init_service(struct ubus_context *ctx)
{
	avl_init(&services, avl_strcmp, false, NULL);
	ubus_add_object(ctx, &main_object);
}