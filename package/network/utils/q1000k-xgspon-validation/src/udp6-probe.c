// SPDX-License-Identifier: GPL-2.0-only
/* Finite IPv6 UDP echo control for the two owned bench namespaces. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_in6 a = { .sin6_family = AF_INET6 };
	struct pollfd p = { .events = POLLIN };
	char payload[96], reply[96], *end;
	long port;
	int fd, n, server;
	if (argc != 4 || (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) return 2;
	server = !strcmp(argv[1], "server");
	errno = 0;
	port = strtol(argv[3], &end, 10);
	if (errno || *end || port < 1024 || port > 65535 || inet_pton(AF_INET6, argv[2], &a.sin6_addr) != 1) return 2;
	a.sin6_port = htons((unsigned short)port);
	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) return 2;
	p.fd = fd;
	if (server) {
		time_t until = time(NULL) + 30;
		if (bind(fd, (void *)&a, sizeof(a))) { close(fd); return 2; }
		puts("udp6_server ready"); fflush(stdout);
		while (time(NULL) < until) {
			socklen_t len = sizeof(a);
			if (poll(&p, 1, 1000) <= 0) continue;
			n = recvfrom(fd, reply, sizeof(reply), 0, (void *)&a, &len);
			if (n > 0) (void)sendto(fd, reply, (size_t)n, 0, (void *)&a, len);
		}
		close(fd); return 0;
	}
	n = snprintf(payload, sizeof(payload), "q1000k-udp6-%ld-%ld", (long)getpid(), (long)time(NULL));
	if (connect(fd, (void *)&a, sizeof(a)) || send(fd, payload, (size_t)n, 0) != n) { close(fd); return 2; }
	if (poll(&p, 1, 2500) > 0) {
		int got = recv(fd, reply, sizeof(reply), 0);
		if (got == n && !memcmp(payload, reply, (size_t)n)) { close(fd); return 0; }
	}
	close(fd); return 1;
}
