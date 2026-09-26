// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/genetlink.h>
#include <linux/omci.h>
#include <libmnl/libmnl.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define BUFFER_SIZE 16384
#define MAX_MIB_OBJECTS 4096
struct client {
	struct mnl_socket *socket;
	uint32_t port, sequence, device, ifindex;
	uint16_t family;
	bool resolving, seen;
	unsigned char request[BUFFER_SIZE], response[BUFFER_SIZE];
	const struct nlattr *attrs[OMCI_ATTR_MAX + 1];
};

static int number(const char *s, uint32_t limit, uint32_t *value)
{
	char *end;
	unsigned long n;

	if (!s || !isdigit((unsigned char)*s))
		return -EINVAL;
	errno = 0;
	n = strtoul(s, &end, s[0] == '0' && s[1] == 'x' ? 16 : 10);
	if (errno || *end || n > limit)
		return -EINVAL;
	*value = n;
	return 0;
}

static void json_string(FILE *out, const void *data, size_t length)
{
	const unsigned char *s = data;

	fputc('"', out);
	for (size_t i = 0; i < length; i++) {
		if (s[i] == '"' || s[i] == '\\')
			fprintf(out, "\\%c", s[i]);
		else if (s[i] < 32 || s[i] >= 127)
			fprintf(out, "\\u%04x", s[i]);
		else
			fputc(s[i], out);
	}
	fputc('"', out);
}

static void json_hex(FILE *out, const void *data, size_t length)
{
	const unsigned char *p = data;

	fputc('"', out);
	for (size_t i = 0; i < length; i++)
		fprintf(out, "%02x", p[i]);
	fputc('"', out);
}

static int attr_cb(const struct nlattr *attr, void *data)
{
	struct client *c = data;
	unsigned int type = mnl_attr_get_type(attr);

	if (type > OMCI_ATTR_MAX || !type)
		return MNL_CB_OK;
	if (c->attrs[type]) { errno = EPROTO; return MNL_CB_ERROR; }
	c->attrs[type] = attr;
	return MNL_CB_OK;
}

static int family_attr_cb(const struct nlattr *attr, void *data)
{
	struct client *c = data;

	if (mnl_attr_get_type(attr) == CTRL_ATTR_FAMILY_ID) {
		if (mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
			return MNL_CB_ERROR;
		c->family = mnl_attr_get_u16(attr);
	}
	if (mnl_attr_get_type(attr) == CTRL_ATTR_VERSION) {
		if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
			return MNL_CB_ERROR;
		if (mnl_attr_get_u32(attr) != OMCI_GENL_VERSION) {
			errno = EPROTONOSUPPORT;
			return MNL_CB_ERROR;
		}
		c->seen = true;
	}
	return MNL_CB_OK;
}

static int reply_cb(const struct nlmsghdr *nlh, void *data)
{
	struct client *c = data;
	const struct genlmsghdr *g;
	const struct genlmsghdr *request = (void *)(c->request + NLMSG_HDRLEN);

	if (mnl_nlmsg_get_payload_len(nlh) < GENL_HDRLEN ||
	    nlh->nlmsg_type != (c->resolving ? GENL_ID_CTRL : c->family)) {
		errno = EPROTO;
		return MNL_CB_ERROR;
	}
	g = mnl_nlmsg_get_payload(nlh);
	if (!c->resolving && (g->cmd != request->cmd || g->version != OMCI_GENL_VERSION)) {
		errno = EPROTO;
		return MNL_CB_ERROR;
	}
	/* libmnl stops iterating on a malformed trailing attribute. Reject such
	 * a reply instead of silently rendering it as missing/null state.
	 */
	const unsigned char *p = (const void *)(g + 1);
	size_t left = mnl_nlmsg_get_payload_len(nlh) - GENL_HDRLEN;
	while (left) {
		const struct nlattr *a = (const void *)p;
		size_t step;

		if (left < NLA_HDRLEN || a->nla_len < NLA_HDRLEN || a->nla_len > left) {
			errno = EPROTO; return MNL_CB_ERROR;
		}
		step = MNL_ALIGN(a->nla_len);
		if (step > left) { errno = EPROTO; return MNL_CB_ERROR; }
		p += step; left -= step;
	}
	if (mnl_attr_parse(nlh, GENL_HDRLEN, c->resolving ? family_attr_cb : attr_cb, c) < 0)
		return MNL_CB_ERROR;
	if (c->resolving && (!c->seen || c->family < GENL_MIN_ID)) {
		errno = EPROTO;
		return MNL_CB_ERROR;
	}
	c->seen = true;
	return MNL_CB_STOP;
}

static struct nlmsghdr *request_start(struct client *c, uint8_t command, bool ack)
{
	struct nlmsghdr *nlh;
	struct genlmsghdr *g;

	memset(c->request, 0, sizeof(c->request));
	memset(c->attrs, 0, sizeof(c->attrs));
	c->seen = false;
	nlh = mnl_nlmsg_put_header(c->request);
	nlh->nlmsg_type = c->resolving ? GENL_ID_CTRL : c->family;
	nlh->nlmsg_flags = NLM_F_REQUEST | (ack ? NLM_F_ACK : 0);
	nlh->nlmsg_seq = ++c->sequence;
	g = mnl_nlmsg_put_extra_header(nlh, GENL_HDRLEN);
	g->cmd = command;
	g->version = c->resolving ? 1 : OMCI_GENL_VERSION;
	if (!c->resolving) {
		if (c->ifindex)
			mnl_attr_put_u32(nlh, OMCI_ATTR_IFINDEX, c->ifindex);
		else
			mnl_attr_put_u32(nlh, OMCI_ATTR_DEV_ID, c->device);
	}
	return nlh;
}

static int exchange(struct client *c)
{
	struct nlmsghdr *request = (void *)c->request;
	struct sockaddr_nl source;
	struct iovec iov = { c->response, sizeof(c->response) };
	struct msghdr msg = { .msg_name = &source, .msg_namelen = sizeof(source),
		.msg_iov = &iov, .msg_iovlen = 1 };
	int ret;
	ssize_t bytes;

	if (mnl_socket_sendto(c->socket, request, request->nlmsg_len) < 0)
		return -errno;
	do {
		memset(&source, 0, sizeof(source));
		msg.msg_namelen = sizeof(source);
		msg.msg_flags = 0;
		bytes = recvmsg(mnl_socket_get_fd(c->socket), &msg, 0);
		if (bytes < 0)
			return -errno;
		if (!bytes || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
		    source.nl_family != AF_NETLINK || source.nl_pid != 0)
			return -EPROTO;
		ret = mnl_cb_run(c->response, bytes, c->sequence, c->port, reply_cb, c);
	} while (ret > 0);
	if (ret < 0)
		return -errno;
	return c->seen || (request->nlmsg_flags & NLM_F_ACK) ? 0 : -EPROTO;
}

static int connect_core(struct client *c)
{
	struct timeval timeout = { .tv_sec = 3 };
	struct nlmsghdr *nlh;
	int ret;

	c->socket = mnl_socket_open2(NETLINK_GENERIC, SOCK_CLOEXEC);
	if (!c->socket)
		return -errno;
	if (mnl_socket_bind(c->socket, 0, MNL_SOCKET_AUTOPID) < 0 ||
	    setsockopt(mnl_socket_get_fd(c->socket), SOL_SOCKET, SO_RCVTIMEO,
		       &timeout, sizeof(timeout)) < 0)
		return -errno;
	c->port = mnl_socket_get_portid(c->socket);
	c->resolving = true;
	nlh = request_start(c, CTRL_CMD_GETFAMILY, false);
	mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, OMCI_GENL_NAME);
	ret = exchange(c);
	c->resolving = false;
	return ret;
}

static int scalar(const struct client *c, unsigned int id, unsigned int size, uint64_t *value)
{
	const struct nlattr *a = c->attrs[id];

	if (!a)
		return -ENODATA;
	if (mnl_attr_get_payload_len(a) != size)
		return -EPROTO;
	*value = 0;
	/* Netlink scalars use host byte order; never dereference unaligned u64. */
	switch (size) {
	case 1: *value = mnl_attr_get_u8(a); break;
	case 2: *value = mnl_attr_get_u16(a); break;
	case 4: *value = mnl_attr_get_u32(a); break;
	case 8: memcpy(value, mnl_attr_get_payload(a), sizeof(*value)); break;
	default: return -EINVAL;
	}
	return 0;
}

struct field { const char *name; uint16_t attr; uint8_t size; bool signed_value; };
#define FIELD(name, id, size) { name, OMCI_ATTR_##id, size, false }
static const struct field status_fields[] = {
	FIELD("device_id", DEV_ID, 4), FIELD("ifindex", IFINDEX, 4),
	FIELD("onu_id", ONU_ID, 2), FIELD("gem_port_id", GEM_PORT_ID, 2),
	FIELD("state", STATE, 1), FIELD("flags", FLAGS, 4), FIELD("capabilities", CAPABILITIES, 4),
	FIELD("agent_enabled", AGENT_ENABLED, 1), FIELD("agent_operational", AGENT_OPERATIONAL, 1),
	FIELD("authenticated", AUTHENTICATED, 1), FIELD("service_rules", SERVICE_RULES, 4),
	{ "service_error", OMCI_ATTR_SERVICE_ERROR, 4, true },
	FIELD("mib_sync", MIB_SYNC, 2), FIELD("mib_objects", MIB_OBJECTS, 4),
	FIELD("rx_packets", RX_PACKETS, 8), FIELD("rx_dropped", RX_DROPPED, 8),
	FIELD("tx_packets", TX_PACKETS, 8), FIELD("tx_errors", TX_ERRORS, 8),
	FIELD("responses", AGENT_RESPONSES, 8), FIELD("unsupported", AGENT_UNSUPPORTED, 8),
	FIELD("olt_profile", OLT_PROFILE_EFFECTIVE, 1), FIELD("onu_type", ONU_TYPE, 1),
	FIELD("telemetry_valid", TELEMETRY_VALID, 4),
	{ "temperature_mc", OMCI_ATTR_BOSA_TEMPERATURE_MC, 4, true },
	FIELD("voltage_uv", BOSA_VOLTAGE_UV, 4), FIELD("bias_ua", BOSA_BIAS_UA, 4),
	FIELD("tx_power_nw", BOSA_TX_POWER_NW, 4), FIELD("rx_power_nw", BOSA_RX_POWER_NW, 4),
};
static int print_status(FILE *out, struct client *c)
{
	uint64_t device, ifindex;

	if (scalar(c, OMCI_ATTR_DEV_ID, 4, &device) ||
	    scalar(c, OMCI_ATTR_IFINDEX, 4, &ifindex))
		return -EPROTO;
	if (c->ifindex ? ifindex != c->ifindex : device != c->device)
		return -ESTALE;
	fputs("{\"schema_version\":1", out);
	for (size_t i = 0; i < sizeof(status_fields) / sizeof(status_fields[0]); i++) {
		const struct field *f = &status_fields[i];
		uint64_t value;
		int ret = scalar(c, f->attr, f->size, &value);

		fprintf(out, ",\"%s\":", f->name);
		if (ret == -ENODATA) { fputs("null", out); continue; }
		if (ret)
			return ret;
		if (f->signed_value)
			fprintf(out, "%" PRId32, (int32_t)value);
		else if (f->size == 8)
			fprintf(out, "\"%" PRIu64 "\"", value);
		else
			fprintf(out, "%" PRIu64, value);
	}
	uint64_t power;
	int ret = scalar(c, OMCI_ATTR_BOSA_RX_POWER_NW, 4, &power);
	if (ret && ret != -ENODATA)
		return ret;
	fputs(",\"rx_power_dbm\":", out);
	if (ret || !power)
		fputs("null", out);
	else
		fprintf(out, "%.2f", 10.0 * log10((double)power / 1000000.0));
	fputs("}\n", out);
	return 0;
}

static int print_mib(FILE *out, struct client *c, uint32_t *next)
{
	uint64_t class, entity, mask, index;
	const struct nlattr *data = c->attrs[OMCI_ATTR_ATTR_DATA];
	const struct nlattr *name = c->attrs[OMCI_ATTR_NAME];
	int ret = scalar(c, OMCI_ATTR_CLASS_ID, 2, &class);

	if (!ret) ret = scalar(c, OMCI_ATTR_ENTITY_ID, 2, &entity);
	if (!ret) ret = scalar(c, OMCI_ATTR_ATTR_MASK, 2, &mask);
	if (!ret) ret = scalar(c, OMCI_ATTR_INDEX, 4, &index);
	if (ret)
		return ret;
	if (!data || mnl_attr_get_payload_len(data) > OMCI_MAX_ATTR_DATA ||
	    (name && mnl_attr_validate(name, MNL_TYPE_NUL_STRING) < 0))
		return -EPROTO;
	fprintf(out, "{\"class_id\":%" PRIu64 ",\"entity_id\":%" PRIu64 ",\"mask\":%" PRIu64 ",\"name\":", class, entity, mask);
	if (name)
		json_string(out, mnl_attr_get_payload(name), mnl_attr_get_payload_len(name) - 1);
	else
		fputs("null", out);
	if (class == 256) fputs(",\"credentials_redacted\":true", out);
	fputs(",\"data_hex\":", out);
	json_hex(out, mnl_attr_get_payload(data), mnl_attr_get_payload_len(data));
	fputc('}', out);
	*next = index;
	return 0;
}

struct config { const char *name; uint16_t key; uint8_t limit; bool text, writable; };
static const char *const profiles[] = {
	"none", "generic", "auto", "nokia", "dasan", "huawei", "fiberhome", "zte",
};
static const char *const onu_types[] = { "other", "sfu", "hgu", "mdu", "sbu", "mtu", "cbu" };
static const struct config configs[] = {
	{ "serial", OMCI_CONFIG_SERIAL_NUMBER, 8, false, false },
	{ "vendor", OMCI_CONFIG_VENDOR_ID, 4, true, true },
	{ "version", OMCI_CONFIG_VERSION, 14, true, true },
	{ "hardware-version", OMCI_CONFIG_HARDWARE_VERSION, 14, true, true },
	{ "sync-circuit-pack", OMCI_CONFIG_SYNC_CIRCUIT_PACK, 1, false, true },
	{ "active-bank", OMCI_CONFIG_ACTIVE_BANK, 1, false, true },
	{ "committed-bank", OMCI_CONFIG_COMMITTED_BANK, 1, false, true },
	{ "logical-onu-id", OMCI_CONFIG_LOGICAL_ONU_ID, 24, true, true },
	{ "logical-password", OMCI_CONFIG_LOGICAL_PASSWORD, 12, true, true },
	{ "equipment", OMCI_CONFIG_EQUIPMENT_ID, 20, true, true },
	{ "software0", OMCI_CONFIG_SOFTWARE_VERSION_0, 14, true, true },
	{ "software1", OMCI_CONFIG_SOFTWARE_VERSION_1, 14, true, true },
	{ "enabled", OMCI_CONFIG_AGENT_ENABLED, 1, false, true },
	{ "onu-type", OMCI_CONFIG_ONU_TYPE, 6, false, true },
	{ "uni-count", OMCI_CONFIG_UNI_COUNT, 4, false, true },
	{ "olt-profile", OMCI_CONFIG_OLT_PROFILE, OMCI_OLT_PROFILE_ZTE, false, true },
	{ "olt-profile-force", OMCI_CONFIG_OLT_PROFILE_FORCE, OMCI_OLT_PROFILE_ZTE, false, true },
	{ "omcc-version", OMCI_CONFIG_OMCC_VERSION, 255, false, true },
};

static const struct config *config_find(const char *name)
{
	for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
		if (!strcmp(name, configs[i].name))
			return &configs[i];
	return NULL;
}

static int config_value(const struct config *config, const char *input, unsigned char value[64], size_t *length)
{
	uint32_t n;

	if (!config || !config->writable)
		return -EOPNOTSUPP;
	if (config->text) {
		*length = strlen(input);
		if ((!*length && config->key != OMCI_CONFIG_LOGICAL_ONU_ID &&
		     config->key != OMCI_CONFIG_LOGICAL_PASSWORD) || *length > config->limit)
			return -EINVAL;
		for (size_t i = 0; i < *length; i++)
			if ((unsigned char)input[i] < 32 || (unsigned char)input[i] >= 127)
				return -EINVAL;
		if (config->key == OMCI_CONFIG_VENDOR_ID) {
			if (*length != 4) return -EINVAL;
			for (size_t i = 0; i < 4; i++)
				if (!((input[i] >= 'A' && input[i] <= 'Z') ||
				      (input[i] >= 'a' && input[i] <= 'z') ||
				      (input[i] >= '0' && input[i] <= '9'))) return -EINVAL;
		}
		memcpy(value, input, *length);
	} else {
		const char *const *names = NULL;
		size_t count = 0;
		int ret = number(input, config->limit, &n);

		if (config->key == OMCI_CONFIG_OLT_PROFILE || config->key == OMCI_CONFIG_OLT_PROFILE_FORCE) {
			names = profiles; count = sizeof(profiles) / sizeof(profiles[0]);
		} else if (config->key == OMCI_CONFIG_ONU_TYPE) {
			names = onu_types; count = sizeof(onu_types) / sizeof(onu_types[0]);
		}
		for (size_t i = 0; ret && i < count; i++)
			if (!strcmp(input, names[i])) { n = i; ret = 0; }
		if (ret ||
		    (config->key == OMCI_CONFIG_OLT_PROFILE && !n) ||
		    (config->key == OMCI_CONFIG_UNI_COUNT && !n))
			return -EINVAL;
		value[0] = n;
		*length = 1;
	}
	return 0;
}

static void usage(FILE *out)
{
	fputs("Usage: omci [-d DEVICE_ID | -i INTERFACE] [--json] COMMAND\n"
	      "  status                       Read kernel OMCI state and counters\n"
	      "  mib                          Read all managed entities\n"
	      "  mib CLASS_ID ENTITY_ID       Read one managed entity\n"
	      "  get KEY                      Read configuration and its source\n"
	      "  set KEY VALUE                Apply validated runtime configuration\n"
	      "Keys: serial, vendor, version, equipment, software0, software1, enabled,\n"
	      "      onu-type, uni-count, olt-profile, olt-profile-force, omcc-version\n"
	      "      hardware-version, sync-circuit-pack, active-bank, committed-bank,\n"
	      "      logical-onu-id, logical-password (banks: 0=A, 1=B).\n"
	      "  config list|get|set|clear|validate   Staged UCI identity (no module needed)\n"
	      "Serial and registration are configured with config set; applied at next startup.\n"
	      "Default device ID: 0. Output is JSON; 64-bit counters are decimal strings.\n"
	      "IDs accept decimal or 0x-prefixed hex. No module loading or MIB reset.\n", out);
	fputs("Profiles: generic, auto, nokia, dasan, huawei, fiberhome, zte; force also accepts none.\n"
	      "ONU types: other, sfu, hgu, mdu, sbu, mtu, cbu. Numeric values are also accepted.\n", out);
}

static int run(int argc, char **argv, FILE *out, struct client *c)
{
	const struct config *config = NULL;
	unsigned char value[64] = {};
	size_t length = 0;
	uint32_t class = 0, entity = 0, cursor = 0;
	const char *command;
	struct nlmsghdr *nlh;
	int ret;
	bool selected = false, one = false, write = false;

	while (argc && argv[0][0] == '-') {
		if (!strcmp(argv[0], "--json")) { argc--; argv++; continue; }
		if (argc < 2 || selected)
			return -EINVAL;
		if (!strcmp(argv[0], "-d")) {
			if (number(argv[1], UINT32_MAX, &c->device)) return -EINVAL;
		} else if (!strcmp(argv[0], "-i")) {
			c->ifindex = if_nametoindex(argv[1]);
			if (!c->ifindex) return -ENODEV;
		} else return -EINVAL;
		selected = true; argc -= 2; argv += 2;
	}
	if (!argc)
		return -EINVAL;
	command = *argv++; argc--;
	if (!strcmp(command, "status")) {
		if (argc) return -EINVAL;
	} else if (!strcmp(command, "mib")) {
		if (argc != 0 && argc != 2) return -EINVAL;
		one = argc == 2;
		if (one && (number(argv[0], UINT16_MAX, &class) || number(argv[1], UINT16_MAX, &entity)))
			return -EINVAL;
	} else if (!strcmp(command, "get") || !strcmp(command, "set")) {
		write = !strcmp(command, "set");
		if (argc != (write ? 2 : 1) || !(config = config_find(argv[0]))) return -EINVAL;
		if (write && (ret = config_value(config, argv[1], value, &length))) return ret;
	} else return -EINVAL;

	ret = connect_core(c);
	if (ret)
		return ret;
	if (!strcmp(command, "status")) {
		request_start(c, OMCI_CMD_GET, false);
		ret = exchange(c);
		return ret ? ret : print_status(out, c);
	}
	if (config) {
		nlh = request_start(c, write ? OMCI_CMD_CONFIG_SET : OMCI_CMD_CONFIG_GET, write);
		mnl_attr_put_u16(nlh, OMCI_ATTR_CONFIG_KEY, config->key);
		if (write) mnl_attr_put(nlh, OMCI_ATTR_CONFIG_VALUE, length, value);
		ret = exchange(c);
		if (ret) return ret;
		if (write) { fputs("{\"ok\":true}\n", out); return 0; }
		const struct nlattr *a = c->attrs[OMCI_ATTR_CONFIG_VALUE];
		uint64_t key, source;
		if (scalar(c, OMCI_ATTR_CONFIG_KEY, 2, &key) || key != config->key ||
		    scalar(c, OMCI_ATTR_CONFIG_SOURCE, 1, &source) || !a)
			return -EPROTO;
		length = mnl_attr_get_payload_len(a);
		if (length > sizeof(value)) return -EPROTO;
		fprintf(out, "{\"key\":\"%s\",\"source\":%" PRIu64 ",\"value\":", config->name, source);
		if (config->text)
			json_string(out, mnl_attr_get_payload(a), length);
		else if (config->key == OMCI_CONFIG_SERIAL_NUMBER && length == 8)
			json_hex(out, mnl_attr_get_payload(a), length);
		else if (length == 1)
			fprintf(out, "%u", mnl_attr_get_u8(a));
		else return -EPROTO;
		fputs("}\n", out);
		return 0;
	}
	if (!one) fputc('[', out);
	for (unsigned int i = 0; i < MAX_MIB_OBJECTS; i++) {
		uint32_t next;
		nlh = request_start(c, one ? OMCI_CMD_MIB_GET : OMCI_CMD_MIB_NEXT, false);
		if (one) {
			mnl_attr_put_u16(nlh, OMCI_ATTR_CLASS_ID, class);
			mnl_attr_put_u16(nlh, OMCI_ATTR_ENTITY_ID, entity);
		} else mnl_attr_put_u32(nlh, OMCI_ATTR_INDEX, cursor);
		ret = exchange(c);
		if (ret == -ENOENT && !one) { fputs("]\n", out); return 0; }
		if (ret) return ret;
		if (i) fputc(',', out);
		ret = print_mib(out, c, &next);
		if (ret) return ret;
		if (one) { fputc('\n', out); return 0; }
		if (next <= cursor) return -EPROTO;
		cursor = next;
	}
	return -E2BIG;
}

int main(int argc, char **argv)
{
	struct client c = {};
	char *json = NULL;
	size_t length = 0;
	FILE *out;
	int ret;

	if (argc >= 2 && !strcmp(argv[1], "config")) {
		execv("/usr/libexec/q1000k-omci-config", argv + 1);
		perror("omci config");
		return 1;
	}
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage(stdout); return 0;
	}
	out = open_memstream(&json, &length);
	if (!out) { perror("omci"); return 1; }
	ret = run(argc - 1, argv + 1, out, &c);
	if (fclose(out) && !ret) ret = -errno;
	if (c.socket) mnl_socket_close(c.socket);
	if (!ret) {
		if (fwrite(json, 1, length, stdout) != length) ret = -EIO;
	} else {
		fprintf(stderr, "omci: %s\n", strerror(-ret));
		fprintf(stdout, "{\"error\":%d,\"message\":", ret);
		json_string(stdout, strerror(-ret), strlen(strerror(-ret)));
		fputs("}\n", stdout);
	}
	free(json);
	return ret ? 1 : 0;
}
