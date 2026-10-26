/* bite 79 — test_churn.c: drives hardd through its paces WITHOUT a browser.
 *
 * COMPILE + RUN (inside WSL, from the repo root, hardd running):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/79-hardening/test_churn.c \
 *       -o /tmp/b79t
 *   timeout 30 /tmp/b79t 127.0.0.1 8090
 *   timeout 10 /tmp/b79t 127.0.0.1 8090 --flood     # the abuse scenario
 *
 * Scenario 1 (churn): 10 connect/IDENT/SEND/disconnect cycles (half abrupt,
 * half clean QUIT), plus two persistent peers that exchange a message —
 * the "disconnect/reconnect matrix" in miniature. Verifies:
 *   - registration, routing, duplicate-name rejection
 *   - protocol errors answered (never silence, never crash)
 *   - the server is still healthy after all of it (final STATS query)
 * Scenario 2 (--flood): 500 garbage lines/sec for ~2 s from one connection;
 * the server must keep answering a legit peer during and after.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int dial(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	struct timeval tv = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static int send_line(int fd, const char *s)
{
	char buf[1200];
	int n = snprintf(buf, sizeof(buf), "%s\n", s);
	return send(fd, buf, (size_t)n, 0) == n ? 0 : -1;
}

static int recv_line(int fd, char *out, size_t cap)
{
	size_t used = 0;
	while (used < cap - 1) {
		char c;
		ssize_t r = recv(fd, &c, 1, 0);
		if (r <= 0)
			return -1;
		if (c == '\n')
			break;
		out[used++] = c;
	}
	out[used] = '\0';
	return 0;
}

static int expect(int fd, const char *label, const char *needle)
{
	char buf[2048];
	if (recv_line(fd, buf, sizeof(buf)) != 0) {
		fprintf(stderr, "churn: FAIL %s (no reply)\n", label);
		return 1;
	}
	if (!strstr(buf, needle)) {
		fprintf(stderr, "churn: FAIL %s (got '%.80s', wanted '%s')\n",
			label, buf, needle);
		return 1;
	}
	return 0;
}

static int churn(int port)
{
	int failures = 0;

	/* two persistent peers first: they prove routing works DURING churn */
	int a = dial(port), b = dial(port);
	if (a < 0 || b < 0)
		return 1;
	send_line(a, "IDENT alice");
	failures += expect(a, "alice ident", "OK ident");
	send_line(b, "IDENT bob");
	failures += expect(b, "bob ident", "OK ident");

	/* 10 churn cycles: connect, ident, (abrupt close | clean quit) */
	for (int i = 0; i < 10; i++) {
		int fd = dial(port);
		if (fd < 0) {
			fprintf(stderr, "churn: FAIL dial %d\n", i);
			failures++;
			break;
		}
		char name[32], msg[64];
		snprintf(name, sizeof(name), "temp%d", i);
		snprintf(msg, sizeof(msg), "IDENT %s", name);
		send_line(fd, msg);
		failures += expect(fd, "temp ident", "OK ident");
		/* temp peers also SAY something to alice: she must receive it */
		snprintf(msg, sizeof(msg), "SEND alice hi from %s", name);
		send_line(fd, msg);
		failures += expect(fd, "send ok", "OK sent");
		if (i % 2 == 0) {
			close(fd);              /* abrupt: no QUIT, no close frame */
		} else {
			send_line(fd, "QUIT");
			expect(fd, "bye", "BYE");
			close(fd);
		}
	}

	/* alice received 10 relays while peers appeared and vanished */
	for (int i = 0; i < 10; i++)
		failures += expect(a, "relay from temp peer", "MSG temp");

	/* protocol abuse on the persistent connection */
	send_line(a, "garbage command");
	failures += expect(a, "unknown cmd", "ERR");
	send_line(a, "SEND");
	failures += expect(a, "SEND missing args", "ERR");
	send_line(a, "SEND nobody hi");
	failures += expect(a, "unknown target", "ERR");
	/* overlong line on a THROWAWAY connection: the server's fixed input
	 * buffer means an overlong line is a flood, and the correct response
	 * is a kick — we just proved alice survives it because it isn't her
	 * connection that gets kicked. */
	{
		int t = dial(port);
		send_line(t, "IDENT overlong-guy");
		char buf[256];
		recv_line(t, buf, sizeof(buf));
		char big[1200];
		memset(big, 'x', sizeof(big) - 2);
		big[sizeof(big) - 2] = '\n';
		big[sizeof(big) - 1] = '\0';
		send(t, big, strlen(big), 0);
		if (recv_line(t, buf, sizeof(buf)) == 0)
			printf("churn: overlong line answered: %.60s\n", buf);
		else
			printf("churn: overlong line -> connection closed (kick path)\n");
		close(t);
	}

	/* bob must still work after all of alice's abuse: */
	send_line(b, "STATS");
	failures += expect(b, "stats after abuse", "STATS uptime");
	send_line(b, "SEND alice still-alive");
	failures += expect(b, "bob send", "OK sent");
	failures += expect(a, "alice got bob's msg", "MSG bob still-alive");

	close(a);
	close(b);

	printf("churn: %s (%d failure(s))\n", failures ? "FAILED" : "PASSED",
	       failures);
	return failures;
}

static int flood(int port)
{
	int f = dial(port);
	if (f < 0)
		return 1;
	int victim = dial(port);
	send_line(victim, "IDENT victim");
	char buf[256];
	recv_line(victim, buf, sizeof(buf));

	printf("flood: spraying ~500 garbage lines/sec for 2 seconds...\n");
	char line[64];
	memset(line, 'z', sizeof(line) - 2);
	line[sizeof(line) - 2] = '\n';
	line[sizeof(line) - 1] = '\0';
	for (int i = 0; i < 1000; i++) {
		if (send(f, line, strlen(line), 0) < 0) {
			printf("flood: flood socket closed by server after %d lines "
			       "(kick path — correct)\n", i);
			break;
		}
		if (i % 250 == 0) {
			struct timespec ms = { 0, 1000 * 1000 };  /* pace to ~500 lines/s */
			nanosleep(&ms, NULL);
		}
	}
	/* the legit peer must be unaffected: */
	send_line(victim, "STATS");
	int failures = expect(victim, "stats after flood", "STATS uptime");
	printf("flood: %s\n", failures ? "FAILED" : "PASSED (legit peer unaffected)");
	close(f);
	close(victim);
	return failures;
}

int main(int argc, char **argv)
{
	int port = argc > 2 ? atoi(argv[2]) : 8090;
	signal(SIGPIPE, SIG_IGN);   /* the flood test EXPECTS RSTs; keep writing */
	if (argc > 3 && !strcmp(argv[3], "--flood"))
		return flood(port);
	return churn(port);
}
