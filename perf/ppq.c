#include "module.h"
#include "common.h"
#include <time.h>

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luaconf.h>

#define PPQ_QUEUE_SIZE 1024

struct ppq_t {
	struct xtm_queue *tx2net;
	struct xtm_queue *net2tx;

	pthread_t net_thread;
	lua_State *L;

	int is_running;
};

struct ppq_message_t {
	struct ppq_t *q;
	uint64_t id;
	uint64_t otime;
	uint64_t ctime;
	int delayed;
};

static uint64_t
now()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec*1e9 + ts.tv_nsec;
}

static struct ppq_message_t*
newmsg(uint64_t id)
{
	struct ppq_message_t *msg = malloc(sizeof(struct ppq_message_t));
	msg->id = id;
	msg->otime = msg->ctime = now();
	return msg;
}

static void tx_consumer_func(void *);

static void net_consumer_func(void *arg) {
	struct ppq_message_t *msg = (struct ppq_message_t*) arg;
	struct ppq_t *q = msg->q;
	msg->ctime = now();
	while (xtm_msg_probe(q->net2tx) != 0)
		;
	xtm_fail_unless(xtm_fun_dispatch(q->net2tx, tx_consumer_func, msg, msg->delayed) == 0);
}


static void tx_consumer_func(void *arg) {
	struct ppq_message_t *msg = (struct ppq_message_t*) arg;
	struct ppq_t *q = msg->q;
	double received_at = now();

	if (lua_gettop(q->L) > 1) {
		lua_pop(q->L, lua_gettop(q->L)-1);
	}
	xtm_fail_unless(lua_gettop(q->L) == 1);

	/** module table */
	luaL_checktype(q->L, 1, LUA_TTABLE);
	lua_pushstring(q->L, "on_ctx");
	/** t.on_ctx */
	lua_gettable(q->L, -2);

	if (lua_isfunction(q->L, -1)) {
		/** arg[1] = L[1] (module table) */
		lua_pushvalue(q->L, 1);
		/** arg[2] = x = {} (message table) */
		lua_createtable(q->L, 0, 7);

		lua_pushstring(q->L, "id");
		lua_pushinteger(q->L, msg->id);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "ack");
		lua_pushnumber(q->L, (received_at-msg->ctime)/1e3);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "syn");
		lua_pushnumber(q->L, (msg->ctime-msg->otime)/1e3);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "rtt");
		lua_pushnumber(q->L, (received_at-msg->otime)/1e3);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "otime");
		lua_pushnumber(q->L, msg->otime);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "ftime");
		lua_pushnumber(q->L, received_at);
		lua_settable(q->L, -3);

		lua_pushstring(q->L, "rtime");
		lua_pushnumber(q->L, msg->ctime);
		lua_settable(q->L, -3);

		xtm_fail_unless(lua_pcall(q->L, 2, 0, 0) == 0);
	}

	free(msg);
}

static void *
net_worker_f(void *arg)
{
	struct ppq_t *q = (struct ppq_t *) arg;
	q->tx2net = xtm_create(PPQ_QUEUE_SIZE);
	xtm_fail_unless(q->tx2net != NULL);
	int tx2net_fd = xtm_fd(q->tx2net);

	__atomic_store_n(&q->is_running, 1, __ATOMIC_RELEASE);

	while (1) {
		fd_set readset;
		FD_ZERO(&readset);
		FD_SET(tx2net_fd, &readset);

		int rc = select(tx2net_fd+1, &readset, NULL, NULL, NULL);
		xtm_fail_unless(!(rc <= 0 && errno != EINTR));
		if (rc <= 0)
			continue;

		if (FD_ISSET(tx2net_fd, &readset))
			xtm_fail_unless(xtm_fun_invoke_all(q->tx2net) == 0);
	}

	return NULL;
}

static int
send(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	double msg_id = luaL_checknumber(L, 2);

	lua_pushstring(L, "q");
	lua_gettable(L, 1);

	if (!lua_isuserdata(L, -1)) {
		luaL_error(L, "ppq object is not found at index 'q', got: %s",
			   lua_typename(L, lua_type(L, -1)));
	}

	int delayed = 0;
	if (lua_isnumber(L, -3))
		delayed = lua_tonumber(L, -2);

	struct ppq_t *q = (struct ppq_t *)lua_touserdata(L, -1);
	if (q == NULL)
		luaL_error(L, "ppq:run() lost q");

	double ctime = now();
	struct ppq_message_t *msg = newmsg((uint64_t) msg_id);
	xtm_fail_unless(msg != NULL);
	msg->q = q;
	msg->delayed = delayed;
	while(xtm_msg_probe(q->tx2net) != 0)
		;
	xtm_fail_unless(xtm_fun_dispatch(q->tx2net, net_consumer_func, msg, delayed) == 0);
	lua_pushnumber(L, now() - ctime);
	return 1;
}

static int
run(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	lua_pushstring(L, "q");
	lua_gettable(L, 1); // get t[q];
	struct ppq_t *q = (struct ppq_t *)lua_touserdata(L, -1);
	if (q == NULL)
		luaL_error(L, "ppq:run() lost q");
	q->L = L;
	q->net2tx = xtm_create(PPQ_QUEUE_SIZE);
	xtm_fail_unless(q->net2tx != NULL);
	xtm_fail_unless(pthread_create(&q->net_thread, NULL, net_worker_f, q) == 0);

	/** Wait until net thread initializes itself */
	while (__atomic_load_n(&q->is_running, __ATOMIC_ACQUIRE) != 1)
		;

	int net2tx_fd = xtm_fd(q->net2tx);
	while (1) {
		xtm_fail_unless((coio_wait(net2tx_fd, COIO_READ, DBL_MAX) & COIO_READ) != 0);
		xtm_fail_unless(xtm_fun_invoke_all(q->net2tx) == 0);
	}

	return 0;
}

static int
new(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	lua_pushstring(L, "on_ctx");
	lua_gettable(L, -2);

	if (!lua_isfunction(L, -1))
		luaL_error(L, "ppq.new() requires function for 'on_ctx' got: %s",
			   lua_typename(L, lua_type(L, -1)));

	lua_pop(L, 1);

	lua_pushstring(L, "q");
	struct ppq_t *ppq = (struct ppq_t *) lua_newuserdata(L, sizeof(struct ppq_t));
	if (ppq == NULL)
		luaL_error(L, "not enough memory: %s", strerror(errno));
	memset(ppq, 0, sizeof(*ppq));

	lua_settable(L, -3);

	luaL_getmetatable(L, "ppq.metatable");
	lua_setmetatable(L, -2);
	return 1;
}

static const struct luaL_Reg ppqlib_f[] = {
	{"new", new},
	{NULL, NULL},
};

static const struct luaL_Reg ppqlib_m [] = {
	{"send", send},
	{"run", run},
	{NULL, NULL},
};

int luaopen_libppq(lua_State *L) {
	luaL_newmetatable(L, "ppq.metatable");

	lua_pushstring(L, "__index");
	lua_pushvalue(L, -2);
	/**  metatable.__index = metatable */
	lua_settable(L, -3);

	luaL_openlib(L, NULL, ppqlib_m, 0);
	luaL_openlib(L, "ppq", ppqlib_f, 0);
	return 1;
}
