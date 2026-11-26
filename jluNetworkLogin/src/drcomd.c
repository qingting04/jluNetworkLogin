#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <net/if.h>

#include <libubox/blobmsg.h>
#include <libubox/ulog.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>
#include <libubus.h>
#include <uci.h>

#include "drcom-packet.h"

#define DRCOM_DEFAULT_PORT 61440
#define DRCOM_DEFAULT_SERVER "10.100.61.3"
#define DRCOM_RECV_BUFSZ 1024

#define CHALLENGE_TIMEOUT_MS 2000
#define LOGIN_TIMEOUT_MS 3000
#define KEEPALIVE_TIMEOUT_MS 3000

typedef enum {
	PHASE_IDLE = 0,
	PHASE_CHALLENGE,
	PHASE_LOGIN,
	PHASE_KEEPALIVE,
} drcom_phase_t;

struct drcom_ctx {
	bool enabled;
	char server[64];
	char username[37];
	char password[33];
	char interface[16];
	in_addr_t bind_ip;
	bool bind_ip_ok;
	char hostname[33];
	char dns[16];
	int client_port;
	int retry_interval_s;

	int sock;
	struct sockaddr_in server_sa;
	in_addr_t local_ip;
	uint8_t mac[6];
	bool mac_ok;

	uint8_t salt[4];
	uint8_t md5a[16];
	uint8_t tail[16];
	uint8_t flux[4];
	uint16_t rand16;
	uint32_t alivesum;

	drcom_phase_t phase;
	int keepalive_stage; /* 0,1,2 */
	int attempt;

	bool online;
	time_t last_login;
	time_t last_rx;
	time_t last_tx;
	char last_err[128];

	uint8_t rxbuf[DRCOM_RECV_BUFSZ];

	struct uloop_fd ufd;
	struct uloop_timeout tmo;
	struct uloop_timeout retry_tmo;
	struct uloop_timeout keepalive_tmo;

	struct ubus_context *ubus;
	struct ubus_object ubus_obj;
};

static struct blob_buf bb;
static struct drcom_ctx g;

static void set_err(struct drcom_ctx *c, const char *msg) {
	snprintf(c->last_err, sizeof(c->last_err), "%s", msg ? msg : "");
	if (msg && *msg)
		ULOG_WARN("%s\n", msg);
}

static void fmt_mac(const uint8_t mac[6], char out[18]) {
	snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void fmt_ip(in_addr_t ip, char out[INET_ADDRSTRLEN]) {
	struct in_addr a = { .s_addr = ip };
	const char *p = inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
	if (!p)
		snprintf(out, INET_ADDRSTRLEN, "0.0.0.0");
}

static bool parse_mac(const char *s, uint8_t mac[6]) {
	unsigned int b[6];
	if (!s || !*s)
		return false;
	if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return false;
	for (int i = 0; i < 6; i++)
		mac[i] = (uint8_t)b[i];
	return true;
}

static int resolve_server(struct drcom_ctx *c) {
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	char portstr[16];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	/* Destination is the DrCOM server port (fixed 61440); the configurable
	 * `client_port` is the local source port bound in open_socket(). */
	snprintf(portstr, sizeof(portstr), "%d", DRCOM_DEFAULT_PORT);
	int r = getaddrinfo(c->server, portstr, &hints, &res);
	if (r != 0)
		return -1;

	memset(&c->server_sa, 0, sizeof(c->server_sa));
	memcpy(&c->server_sa, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return 0;
}

static int open_socket(struct drcom_ctx *c) {
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons((uint16_t)c->client_port);
	local.sin_addr.s_addr = c->bind_ip_ok ? c->bind_ip : INADDR_ANY;

	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&c->server_sa, sizeof(c->server_sa)) != 0) {
		close(fd);
		return -1;
	}

	struct sockaddr_in name;
	socklen_t namelen = sizeof(name);
	if (getsockname(fd, (struct sockaddr *)&name, &namelen) != 0) {
		close(fd);
		return -1;
	}

	c->local_ip = name.sin_addr.s_addr;
	return fd;
}

static void schedule_retry(struct drcom_ctx *c);

static void stop_io(struct drcom_ctx *c) {
	uloop_timeout_cancel(&c->tmo);
	uloop_timeout_cancel(&c->keepalive_tmo);
	if (c->ufd.registered)
		uloop_fd_delete(&c->ufd);
	if (c->sock >= 0) {
		close(c->sock);
		c->sock = -1;
	}
	c->phase = PHASE_IDLE;
}

static void go_offline(struct drcom_ctx *c, const char *reason) {
	c->online = false;
	set_err(c, reason);
	stop_io(c);
	schedule_retry(c);
}

static void start_challenge(struct drcom_ctx *c);

static void retry_cb(struct uloop_timeout *t) {
	struct drcom_ctx *c = container_of(t, struct drcom_ctx, retry_tmo);
	start_challenge(c);
}

static void schedule_retry(struct drcom_ctx *c) {
	int ms = (c->retry_interval_s > 0 ? c->retry_interval_s : 28) * 1000;
	uloop_timeout_set(&c->retry_tmo, ms);
}

/* 统一的发包收尾：记发送时间、切换阶段、武装响应超时。
 * 原来「send + last_tx + phase + uloop_timeout_set」这段在 5 处各抄了一遍。 */
static void drcom_send(struct drcom_ctx *c, const void *pkt, size_t len,
		       drcom_phase_t phase, int timeout_ms) {
	send(c->sock, pkt, len, 0);
	c->last_tx = time(NULL);
	c->phase = phase;
	uloop_timeout_set(&c->tmo, timeout_ms);
}

static void timeout_cb(struct uloop_timeout *t) {
	struct drcom_ctx *c = container_of(t, struct drcom_ctx, tmo);
	switch (c->phase) {
	case PHASE_CHALLENGE:
		go_offline(c, "challenge timeout");
		break;
	case PHASE_LOGIN:
		go_offline(c, "login timeout");
		break;
	case PHASE_KEEPALIVE:
		go_offline(c, "keepalive timeout");
		break;
	default:
		break;
	}
}

static void keepalive_cycle_cb(struct uloop_timeout *t) {
	struct drcom_ctx *c = container_of(t, struct drcom_ctx, keepalive_tmo);
	c->keepalive_stage = 0;
	c->rand16 = (uint16_t)(rand() & 0xffff);
	uint8_t pkt[DRCOM_KEEPALIVE0_LEN];
	drcom_build_keepalive0(c->md5a, c->tail, c->rand16, pkt);
	drcom_send(c, pkt, sizeof(pkt), PHASE_KEEPALIVE, KEEPALIVE_TIMEOUT_MS);
}

static void start_keepalive(struct drcom_ctx *c) {
	memset(c->flux, 0, sizeof(c->flux));
	uloop_timeout_cancel(&c->keepalive_tmo);
	uloop_timeout_set(&c->keepalive_tmo, 10);
}

static void start_login(struct drcom_ctx *c) {
	uint8_t pkt[512];
	struct drcom_login_fields fields = {
		.username = c->username,
		.password = c->password,
		.hostname = c->hostname,
		.dns = c->dns,
		.local_ip = c->local_ip,
	};
	memcpy(fields.mac, c->mac, sizeof(fields.mac));
	memcpy(fields.salt, c->salt, sizeof(fields.salt));
	ssize_t len = drcom_build_login(&fields, pkt, sizeof(pkt), c->md5a);
	if (len < 0) {
		go_offline(c, "invalid login config");
		return;
	}
	drcom_send(c, pkt, (size_t)len, PHASE_LOGIN, LOGIN_TIMEOUT_MS);
}

static void sock_cb(struct uloop_fd *u, unsigned int events);

static void start_challenge(struct drcom_ctx *c) {
	uloop_timeout_cancel(&c->retry_tmo);
	stop_io(c);

	if (!c->enabled) {
		set_err(c, "disabled");
		return;
	}

	if (!c->username[0]) {
		set_err(c, "username required");
		return;
	}

	if (!c->password[0]) {
		set_err(c, "password required");
		return;
	}

	if (!c->bind_ip_ok) {
		set_err(c, "ip required");
		return;
	}

	if (!c->mac_ok) {
		set_err(c, "mac required");
		return;
	}

	if (resolve_server(c) != 0) {
		go_offline(c, "resolve server failed");
		return;
	}

	c->sock = open_socket(c);
	if (c->sock < 0) {
		go_offline(c, "open socket failed");
		return;
	}

	c->ufd.fd = c->sock;
	c->ufd.cb = sock_cb;
	uloop_fd_add(&c->ufd, ULOOP_READ);

	uint8_t pkt[20];
	memset(pkt, 0, sizeof(pkt));
	c->attempt++;
	pkt[0] = 0x01;
	pkt[1] = (uint8_t)(0x02 + (c->attempt & 0xff));
	pkt[2] = (uint8_t)(rand() & 0xff);
	pkt[3] = (uint8_t)(rand() & 0xff);
	pkt[4] = 0x68;
	drcom_send(c, pkt, sizeof(pkt), PHASE_CHALLENGE, CHALLENGE_TIMEOUT_MS);
}

static void sock_cb(struct uloop_fd *u, unsigned int events) {
	(void)events;
	struct drcom_ctx *c = container_of(u, struct drcom_ctx, ufd);

	for (;;) {
		size_t n = sizeof(c->rxbuf);
		ssize_t r = recv(c->sock, c->rxbuf, n, 0);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			go_offline(c, "recv failed");
			return;
		}
		if (r == 0)
			break;

		c->last_rx = time(NULL);

		if (c->rxbuf[0] == 0x4d && r >= 2) {
			if (c->rxbuf[1] == 0x15) {
				go_offline(c, "kicked: other device logged in");
				return;
			}
			continue;
		}

		switch (c->phase) {
		case PHASE_CHALLENGE:
			if (c->rxbuf[0] == 0x02 && r >= 8) {
				memcpy(c->salt, c->rxbuf + 4, 4);
				uloop_timeout_cancel(&c->tmo);
				start_login(c);
			}
			break;
		case PHASE_LOGIN:
			if (c->rxbuf[0] == 0x04 && r >= 0x17 + 16) {
				memcpy(c->tail, c->rxbuf + 0x17, 16);
				c->online = true;
				c->last_login = time(NULL);
				set_err(c, "");
				uloop_timeout_cancel(&c->tmo);
				start_keepalive(c);
			} else if (c->rxbuf[0] == 0x05) {
				go_offline(c, "login failed: wrong credentials");
			}
			break;
		case PHASE_KEEPALIVE:
			if (c->rxbuf[0] == 0x07) {
				uloop_timeout_cancel(&c->tmo);
				if (c->keepalive_stage > 0 && r >= 20)
					memcpy(c->flux, c->rxbuf + 16, 4);

				/* 两个 stage 只差 build_keepalive12 的 type（= 新的 stage 值），合并成一段 */
				if (c->keepalive_stage < 2) {
					uint8_t pkt[DRCOM_KEEPALIVE12_LEN];

					c->keepalive_stage++;
					drcom_build_keepalive12(c->keepalive_stage, c->alivesum++,
						c->rand16, c->flux, c->local_ip, pkt);
					drcom_send(c, pkt, sizeof(pkt), PHASE_KEEPALIVE, KEEPALIVE_TIMEOUT_MS);
				} else {
					c->keepalive_stage = 0;
					c->phase = PHASE_IDLE;
					uloop_timeout_set(&c->keepalive_tmo, 20000);
				}
			}
			break;
		default:
			break;
		}
	}
}

enum {
	RELOAD_FORCE,
	__RELOAD_MAX,
};

static const struct blobmsg_policy reload_policy[__RELOAD_MAX] = {
	[RELOAD_FORCE] = { .name = "force", .type = BLOBMSG_TYPE_BOOL },
};

/* 取一个字符串选项；空值或缺失时用 def。等价于原来的
 * uci_lookup_option_string() + snprintf(out, sz, "%s", v ? v : def) 组合。 */
static void uci_get_str(struct uci_context *uc, struct uci_section *s, const char *name,
			char *out, size_t out_sz, const char *def) {
	const char *v = uci_lookup_option_string(uc, s, name);

	snprintf(out, out_sz, "%s", (v && *v) ? v : (def ? def : ""));
}

static int load_uci_config(struct drcom_ctx *c) {
	struct uci_context *uc = uci_alloc_context();
	if (!uc)
		return -1;

	struct uci_package *pkg = NULL;
	if (uci_load(uc, "jlu-network-login", &pkg) != UCI_OK) {
		uci_free_context(uc);
		return -1;
	}

	struct uci_section *s = uci_lookup_section(uc, pkg, "main");
	if (!s) {
		uci_unload(uc, pkg);
		uci_free_context(uc);
		return -1;
	}

	const char *v;

	v = uci_lookup_option_string(uc, s, "enabled");
	c->enabled = (v && (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")));

	snprintf(c->server, sizeof(c->server), "%s", DRCOM_DEFAULT_SERVER);

	uci_get_str(uc, s, "username", c->username, sizeof(c->username), "");
	uci_get_str(uc, s, "password", c->password, sizeof(c->password), "");
	uci_get_str(uc, s, "interface", c->interface, sizeof(c->interface), "wan");

	c->bind_ip_ok = false;
	c->bind_ip = INADDR_ANY;
	v = uci_lookup_option_string(uc, s, "ip");
	if (v && *v) {
		struct in_addr a;
		if (inet_pton(AF_INET, v, &a) == 1) {
			c->bind_ip = a.s_addr;
			c->bind_ip_ok = true;
		} else {
			ULOG_WARN("invalid ip: %s\n", v);
		}
	}

	{
		char hn[sizeof(c->hostname)] = {0};
		if (gethostname(hn, sizeof(hn) - 1) == 0)
			snprintf(c->hostname, sizeof(c->hostname), "%s", hn);
		else
			snprintf(c->hostname, sizeof(c->hostname), "OpenWrt");
	}

	snprintf(c->dns, sizeof(c->dns), "%s", "10.10.10.10");

	c->client_port = DRCOM_DEFAULT_PORT;

	c->retry_interval_s = 28;

	v = uci_lookup_option_string(uc, s, "mac");
	c->mac_ok = parse_mac(v, c->mac);

	uci_unload(uc, pkg);
	uci_free_context(uc);
	return 0;
}

static int ubus_status(struct ubus_context *ubus, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg) {
	(void)ubus; (void)obj; (void)req; (void)method; (void)msg;
	struct drcom_ctx *c = &g;

	blob_buf_init(&bb, 0);

	char ipbuf[INET_ADDRSTRLEN];
	fmt_ip(c->local_ip, ipbuf);
	char macbuf[18];
	fmt_mac(c->mac, macbuf);

	const char *state = "idle";
	if (c->online)
		state = "online";
	else if (c->phase == PHASE_CHALLENGE)
		state = "challenge";
	else if (c->phase == PHASE_LOGIN)
		state = "login";
	else if (c->phase == PHASE_KEEPALIVE)
		state = "keepalive";

	blobmsg_add_string(&bb, "state", state);
	blobmsg_add_u8(&bb, "enabled", c->enabled);
	blobmsg_add_u8(&bb, "online", c->online);
	blobmsg_add_string(&bb, "server", c->server);
	blobmsg_add_string(&bb, "username", c->username);
	blobmsg_add_string(&bb, "ip", ipbuf);
	blobmsg_add_string(&bb, "mac", macbuf);
	blobmsg_add_u32(&bb, "client_port", (uint32_t)c->client_port);
	blobmsg_add_u32(&bb, "retry_interval", (uint32_t)c->retry_interval_s);
	blobmsg_add_u32(&bb, "keepalive_stage", (uint32_t)c->keepalive_stage);
	blobmsg_add_u32(&bb, "last_login", (uint32_t)c->last_login);
	blobmsg_add_u32(&bb, "last_rx", (uint32_t)c->last_rx);
	blobmsg_add_u32(&bb, "last_tx", (uint32_t)c->last_tx);
	blobmsg_add_string(&bb, "last_error", c->last_err);

	ubus_send_reply(ubus, req, bb.head);
	return 0;
}

static int drcom_ubus_reconnect(struct ubus_context *ubus, struct ubus_object *obj,
				      struct ubus_request_data *req, const char *method, struct blob_attr *msg) {
	(void)ubus; (void)obj; (void)method; (void)msg;
	stop_io(&g);
	g.online = false;
	set_err(&g, "manual reconnect");
	start_challenge(&g);
	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "result", "ok");
	ubus_send_reply(g.ubus, req, bb.head);
	return 0;
}

static int ubus_reload(struct ubus_context *ubus, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg) {
	(void)ubus; (void)obj; (void)method;
	bool force = false;
	if (msg) {
		struct blob_attr *tb[__RELOAD_MAX];
		blobmsg_parse(reload_policy, __RELOAD_MAX, tb, blob_data(msg), blob_len(msg));
		force = tb[RELOAD_FORCE] ? blobmsg_get_bool(tb[RELOAD_FORCE]) : false;
	}

	if (load_uci_config(&g) != 0 && !force) {
		blob_buf_init(&bb, 0);
		blobmsg_add_string(&bb, "result", "error");
		blobmsg_add_string(&bb, "message", "uci load failed");
		ubus_send_reply(g.ubus, req, bb.head);
		return 0;
	}

	stop_io(&g);
	g.online = false;
	set_err(&g, "reloaded");
	start_challenge(&g);

	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "result", "ok");
	ubus_send_reply(g.ubus, req, bb.head);
	return 0;
}

static const struct ubus_method drcom_methods[] = {
	UBUS_METHOD_NOARG("status", ubus_status),
	UBUS_METHOD_NOARG("reconnect", drcom_ubus_reconnect),
	UBUS_METHOD("reload", ubus_reload, reload_policy),
};

static struct ubus_object_type drcom_obj_type =
	UBUS_OBJECT_TYPE("jlu-network-login", drcom_methods);

static void sig_handler(int signo) {
	(void)signo;
	uloop_end();
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	memset(&g, 0, sizeof(g));
	g.sock = -1;
	g.client_port = DRCOM_DEFAULT_PORT;
	g.retry_interval_s = 28;

	srand((unsigned int)(time(NULL) ^ getpid()));

	/* OpenWrt-native logging: goes to logd (logread) */
	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "jlu-network-login");
	ulog_threshold(LOG_INFO);
	ULOG_INFO("starting\n");

	if (uloop_init() != 0) {
		ULOG_ERR("uloop_init failed\n");
		return 1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	g.ubus = ubus_connect(NULL);
	if (!g.ubus) {
		ULOG_ERR("ubus_connect failed\n");
		return 1;
	}
	ubus_add_uloop(g.ubus);

	g.ubus_obj.name = "jlu-network-login";
	g.ubus_obj.type = &drcom_obj_type;
	g.ubus_obj.methods = drcom_methods;
	g.ubus_obj.n_methods = ARRAY_SIZE(drcom_methods);

	if (ubus_add_object(g.ubus, &g.ubus_obj) != 0) {
		ULOG_ERR("ubus_add_object failed\n");
		return 1;
	}

	g.tmo.cb = timeout_cb;
	g.retry_tmo.cb = retry_cb;
	g.keepalive_tmo.cb = keepalive_cycle_cb;

	if (load_uci_config(&g) != 0) {
		set_err(&g, "uci load failed");
	} else {
		ULOG_INFO("config loaded (enabled=%d, server=%s, user=%s)\n", g.enabled, g.server, g.username);
	}

	start_challenge(&g);

	uloop_run();

	stop_io(&g);
	if (g.ubus)
		ubus_free(g.ubus);
	ULOG_INFO("stopped\n");
	ulog_close();
	uloop_done();
	return 0;
}
