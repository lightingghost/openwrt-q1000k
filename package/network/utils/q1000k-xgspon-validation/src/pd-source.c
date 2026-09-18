// SPDX-License-Identifier: GPL-2.0-only
/* Choose one source from an unused, live delegated prefix supplied by netifd.
 * This program only validates addresses; the bench owns address lifetime.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inside(const struct in6_addr *address,
		  const struct in6_addr *network, unsigned int bits)
{
	unsigned int full = bits / 8, partial = bits % 8;
	return !memcmp(address, network, full) &&
		(!partial || !((address->s6_addr[full] ^ network->s6_addr[full]) &
			      (0xff << (8 - partial))));
}

int main(int argc, char **argv)
{
	struct in6_addr network, candidate, existing;
	char text[INET6_ADDRSTRLEN], input[INET6_ADDRSTRLEN + 5], *end;
	unsigned long bits;
	unsigned int i;

	if (argc < 3 || !argv[2][0] || strspn(argv[2], "0123456789") != strlen(argv[2]))
		return 2;
	bits = strtoul(argv[2], &end, 10);
	/* Bounded bench support; longer delegations need a separate address plan. */
	if (*end || bits < 48 || bits > 64 || inet_pton(AF_INET6, argv[1], &network) != 1 ||
	    (network.s6_addr[0] & 0xe0) != 0x20)
		return 2;
	for (i = bits; i < 128; i++)
		if (network.s6_addr[i / 8] & (0x80 >> (i % 8)))
			return 2; /* A network prefix must not contain host bits. */
	for (i = 3; i < (unsigned int)argc; i++) {
		if (strlen(argv[i]) >= sizeof(input))
			return 2;
		strcpy(input, argv[i]);
		end = strchr(input, '/');
		if (end)
			*end = '\0';
		if (inet_pton(AF_INET6, input, &existing) != 1)
			return 2;
		if (inside(&existing, &network, bits))
			return 3; /* Do not borrow a prefix already assigned to another use. */
	}
	candidate = network;
	candidate.s6_addr[15] = 1;
	if (!inet_ntop(AF_INET6, &candidate, text, sizeof(text)))
		return 2;
	puts(text);
	return 0;
}
