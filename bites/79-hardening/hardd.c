/* bite 79 — hardd.c: a hardened signaling-style relay server. This is the
 * bite-72 relay rebuilt the way it must survive production:
 *
 *   - poll()-driven event loop (bite 51's pattern), single thread, no forks
 *   - every peer owns: one fd, one in-buffer, one out-buffer, one name —
 *     and ONE teardown path (peer_close) that frees them exactly once
 *   - input caps: a peer that sends more than MAX_LINE without a newline,
 *     or floods faster than we drain, is disconnected — never malloc'd at
 *   - idle timeout: 15 s of silence = dead connection, close it
 *   - graceful shutdown: SIGTERM/SIGINT via signalfd -> stop accepting,
 *     close every peer, print final stats, exit 0
 *   - stats: uptime, peers served, messages relayed, errors, bytes
 *
 * PROTOCOL (line-based, bite 53 discipline):
 *   IDENT <name>            register; duplicate -> error
 *   SEND <name> <text...>   route text to that peer
 *   STATS                   server stats to the requester
 *   QUIT                    clean disconnect
 *   anything else / overlong line / non-registered sender -> error line
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/79-hardening/hardd.c \
 *       -o /tmp/b79 -pthread
 *   /tmp/b79 [port]            # default 8090
 *   # churn/flood test double:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/79-hardening/test_churn.c \
 *       -o /tmp/b79t
 *   timeout 30 /tmp/b79t 127.0.0.1 8090
 *   # and the valgrind gate (this is the bite's "done when"):
 *   valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
 *       --error-exitcode=9 /tmp/b79 8091 &
 *   # ... run test_churn against 8091, then pkill -TERM -x b79  -> rc must be 0
 *
 * BREAK IT ON PURPOSE (all three from the bite):
 *   1. Kill a client mid-session (test_churn does abrupt closes) — the
 *      server logs the disconnect, frees the slot, keeps serving.
 *   2. Flood: test_churn --flood sprays 500 garbage lines/sec for 2 s.
 *      CPU stays sane, memory flat (fixed buffers), legit peers unaffected.
 *   3. /proc/<pid>/fd count is stable across churn — count with:
 *      ls /proc/$(pgrep -x b79)/fd | wc -l   (idle, after 20 connections)
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT   8090
#define MAX_PEERS      32
#define MAX_LINE       1024        /* a signaling message; beyond this = abuse */
#define OUTBUF_MAX     (64 * 1024) /* undrained output: kick, don't grow */
#define IDLE_TIMEOUT_S 15

typedef struct {
	int   fd;
	int   active;
	char  name[32];
	char  inbuf[MAX_LINE + 1];
	size_t inlen;
	char  outbuf[OUTBUF_MAX];
	size_t outlen;
	long long last_active_ms;
} peer_t;

static peer_t g_peers[MAX_PEERS];
static int g_listen_fd = -1;
static int g_sig_fd = -1;
static volatile sig_atomic_t g_shutdown = 0;

/* stats */
static long long g_start_ms;
static unsigned long g_peers_served, g_msgs_relayed, g_proto_errors, g_kicked;
static unsigned long long g_bytes_in, g_bytes_out;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logline(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char ts[32];
	time_t t = time(NULL);
	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	printf("[%s] ", ts);
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static int set_nonblocking(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ----------------------------- peer lifecycle ----------------------------- */
/* ONE close path. Bite 26's ownership rule: whoever opened it closes it,
 * and everything a peer owns dies here — in order, exactly once. */
static void peer_close(int idx, const char *why)
{
	peer_t *p = &g_peers[idx];
	if (!p->active)
		return;
	logline("peer '%s' disconnected (%s)", p->name[0] ? p->name : "(unnamed)", why);
	close(p->fd);
	p->fd = -1;
	p->active = 0;
	p->inlen = 0;
	p->outlen = 0;
	p->name[0] = '\0';
}

static int peer_alloc(void)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (!g_peers[i].active)
			return i;
	return -1;
}

static int peer_by_name(const char *name)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active && strcmp(g_peers[i].name, name) == 0)
			return i;
	return -1;
}

static void peer_want_write(int idx, const char *data, size_t len)
{
	peer_t *p = &g_peers[idx];
	if (p->outlen + len > OUTBUF_MAX) {
		/* undrained output = dead or malicious peer: kick, don't grow */
		g_kicked++;
		logline("kicking peer '%s': output buffer full", p->name);
		peer_close(idx, "output buffer overflow");
		return;
	}
	memcpy(p->outbuf + p->outlen, data, len);
	p->outlen += len;
}

/* Push pending output NOW (used before a deliberate close, e.g. QUIT —
 * otherwise the queued "BYE" dies with the socket). */
static void peer_flush(int idx)
{
	peer_t *p = &g_peers[idx];
	while (p->outlen > 0) {
		ssize_t w = send(p->fd, p->outbuf, p->outlen, MSG_NOSIGNAL);
		if (w > 0) {
			memmove(p->outbuf, p->outbuf + w, p->outlen - (size_t)w);
			p->outlen -= (size_t)w;
			continue;
		}
		break;   /* EAGAIN or error: drop the tail; close follows anyway */
	}
}

static void send_err(int idx, const char *reason)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf), "ERR %s\n", reason);
	peer_want_write(idx, buf, (size_t)n);
	g_proto_errors++;
}

/* ------------------------------ line handling ----------------------------- */
static void handle_line(int idx, char *line)
{
	peer_t *p = &g_peers[idx];
	g_bytes_in += strlen(line) + 1;

	if (!strncmp(line, "IDENT ", 6)) {
		const char *name = line + 6;
		if (!*name || strlen(name) > 15) {
			send_err(idx, "bad name");
			return;
		}
		if (peer_by_name(name) >= 0) {
			send_err(idx, "name taken");
			return;
		}
		snprintf(p->name, sizeof(p->name), "%s", name);
		peer_want_write(idx, "OK ident\n", 9);
		logline("peer '%s' registered", name);
		return;
	}
	if (!p->name[0]) {
		send_err(idx, "IDENT first");
		return;
	}
	if (!strncmp(line, "SEND ", 5)) {
		char target[32] = { 0 };
		const char *rest = line + 5;
		const char *sp = strchr(rest, ' ');
		if (!sp) {
			send_err(idx, "SEND needs target and text");
			return;
		}
		size_t tl = (size_t)(sp - rest);
		if (tl >= sizeof(target)) {
			send_err(idx, "bad target");
			return;
		}
		memcpy(target, rest, tl);
		int dst = peer_by_name(target);
		if (dst < 0) {
			send_err(idx, "unknown peer");
			return;
		}
		char out[MAX_LINE + 64];
		int n = snprintf(out, sizeof(out), "MSG %s %.*s\n",
				 p->name, (int)(strlen(sp + 1)), sp + 1);
		peer_want_write(dst, out, (size_t)n);
		peer_want_write(idx, "OK sent\n", 8);
		g_msgs_relayed++;
		g_bytes_out += (unsigned long)n;
		return;
	}
	if (!strcmp(line, "STATS")) {
		char out[256];
		int n = snprintf(out, sizeof(out),
				 "STATS uptime_s=%lld peers=%ld relayed=%lu "
				 "errors=%lu kicked=%lu bytes_in=%llu bytes_out=%llu\n",
				 (now_ms() - g_start_ms) / 1000, g_peers_served,
				 g_msgs_relayed, g_proto_errors, g_kicked,
				 g_bytes_in, g_bytes_out);
		peer_want_write(idx, out, (size_t)n);
		return;
	}
	if (!strcmp(line, "QUIT")) {
		peer_want_write(idx, "BYE\n", 4);
		peer_flush(idx);       /* deliver BYE before the socket dies */
		peer_close(idx, "quit");
		return;
	}
	send_err(idx, "unknown command");
}

/* Split the in-buffer into complete lines; partial line stays buffered. */
static void peer_process_input(int idx)
{
	peer_t *p = &g_peers[idx];
	char *nl;
	while ((nl = memchr(p->inbuf, '\n', p->inlen)) != NULL) {
		size_t linelen = (size_t)(nl - p->inbuf);
		p->inbuf[linelen] = '\0';
		if (linelen && p->inbuf[linelen - 1] == '\r')
			p->inbuf[linelen - 1] = '\0';
		handle_line(idx, p->inbuf);
		if (!g_peers[idx].active)          /* QUIT closed us mid-loop */
			return;
		size_t rest = p->inlen - linelen - 1;
		memmove(p->inbuf, nl + 1, rest);
		p->inlen = rest;
	}
}

static void peer_readable(int idx)
{
	peer_t *p = &g_peers[idx];
	char tmp[4096];
	ssize_t r;
	while ((r = recv(p->fd, tmp, sizeof(tmp), 0)) > 0) {
		p->last_active_ms = now_ms();
		size_t space = sizeof(p->inbuf) - 1 - p->inlen;
		if ((size_t)r > space) {
			/* flood guard: input would overflow the fixed buffer.
			 * Drop the peer — cheaper and safer than growing. */
			g_kicked++;
			logline("kicking peer '%s': input flood", p->name);
			peer_close(idx, "input flood");
			return;
		}
		memcpy(p->inbuf + p->inlen, tmp, (size_t)r);
		p->inlen += (size_t)r;
		peer_process_input(idx);
		if (!g_peers[idx].active)
			return;
		if ((size_t)r < sizeof(tmp))
			break;                     /* drained for now (level-triggered poll) */
	}
	if (r == 0) {
		peer_close(idx, "closed by peer");
		return;
	}
	if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		peer_close(idx, "read error");
		return;
	}
}

static void peer_writable(int idx)
{
	peer_t *p = &g_peers[idx];
	while (p->outlen > 0) {
		ssize_t w = send(p->fd, p->outbuf, p->outlen, MSG_NOSIGNAL);
		if (w > 0) {
			memmove(p->outbuf, p->outbuf + w, p->outlen - (size_t)w);
			p->outlen -= (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;                    /* try again next poll round */
		peer_close(idx, "write error");
		return;
	}
}

/* --------------------------- graceful shutdown ----------------------------- */
static void do_shutdown(const char *trigger)
{
	logline("shutdown (%s): closing %d peer(s)...", trigger, MAX_PEERS);
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active)
			peer_close(i, "server shutdown");
	long long up = (now_ms() - g_start_ms) / 1000;
	logline("final stats: uptime=%llds served=%lu relayed=%lu errors=%lu "
		"kicked=%lu in=%llu out=%llu",
		up, g_peers_served, g_msgs_relayed, g_proto_errors, g_kicked,
		g_bytes_in, g_bytes_out);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
	setvbuf(stdout, NULL, _IOLBF, 0);

	g_start_ms = now_ms();

	/* Signals via signalfd: signals become poll()-able bytes instead of
	 * async interruptions — no signal-handler-unsafe work ever runs. */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	g_sig_fd = signalfd(-1, &mask, 0);
	if (g_sig_fd < 0) {
		perror("signalfd");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);              /* send() uses MSG_NOSIGNAL too */

	g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_listen_fd < 0) {
		perror("socket");
		return 1;
	}
	int one = 1;
	setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((unsigned short)port);
	if (bind(g_listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
	    listen(g_listen_fd, 16) != 0) {
		perror("bind/listen");
		return 1;
	}
	set_nonblocking(g_listen_fd);
	memset(g_peers, 0, sizeof(g_peers));
	for (int i = 0; i < MAX_PEERS; i++)
		g_peers[i].fd = -1;
	logline("hardd: listening on tcp/%d (idle timeout %ds, line cap %d)",
		port, IDLE_TIMEOUT_S, MAX_LINE);

	/* ------------------------------ event loop ------------------------------ */
	long long last_stats = now_ms();
	while (!g_shutdown) {
		/* poll set: [0]=listener, then one entry per active peer,
		 * last=signalfd. idxs[] maps each slot back to its peer index
		 * (or -1 listener / -2 signalfd) — index into g_peers directly,
		 * no pointer aliasing games. */
		struct pollfd pfds[2 + MAX_PEERS];
		int idxs[2 + MAX_PEERS];
		int n = 0;
		pfds[n].fd = g_listen_fd;
		pfds[n].events = POLLIN;
		pfds[n].revents = 0;
		idxs[n] = -1;                      /* listener */
		n++;
		for (int i = 0; i < MAX_PEERS; i++) {
			if (!g_peers[i].active)
				continue;
			pfds[n].fd = g_peers[i].fd;
			pfds[n].events = POLLIN | (g_peers[i].outlen ? POLLOUT : 0);
			pfds[n].revents = 0;
			idxs[n] = i;
			n++;
		}
		pfds[n].fd = g_sig_fd;
		pfds[n].events = POLLIN;
		pfds[n].revents = 0;
		idxs[n] = -2;                      /* signalfd */
		n++;

		int rc = poll(pfds, (nfds_t)n, 1000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		for (int k = 0; k < n; k++) {
			short rev = pfds[k].revents;
			if (!rev)
				continue;
			if (idxs[k] == -2) {           /* signalfd */
				/* signalfd footgun: do NOT drain in a while-loop —
				 * a signalfd with an empty queue BLOCKS forever
				 * (no EOF, unlike a pipe). One read per event. */
				struct signalfd_siginfo si;
				if (read(g_sig_fd, &si, sizeof(si)) == sizeof(si)) {
					logline("signal %u received", si.ssi_signo);
					g_shutdown = 1;
				}
				continue;
			}
			if (idxs[k] == -1) {           /* listener */
				if (rev & POLLIN) {
					int c = accept(g_listen_fd, NULL, NULL);
					if (c >= 0) {
						int slot = peer_alloc();
						if (slot < 0) {
							close(c);   /* full: refuse */
							g_kicked++;
						} else {
							set_nonblocking(c);
							g_peers[slot].fd = c;
							g_peers[slot].active = 1;
							g_peers[slot].last_active_ms = now_ms();
							g_peers_served++;
							logline("peer connected (slot %d, %ld served)",
								slot, g_peers_served);
						}
					}
				}
				continue;
			}
			int i = idxs[k];
			if (rev & (POLLHUP | POLLERR | POLLNVAL)) {
				peer_close(i, "hangup/error");
				continue;
			}
			if (rev & POLLIN)
				peer_readable(i);
			if (g_peers[i].active && (rev & POLLOUT))
				peer_writable(i);
		}

		/* idle sweep: silence beyond IDLE_TIMEOUT_S is a dead peer */
		long long tnow = now_ms();
		for (int i = 0; i < MAX_PEERS; i++)
			if (g_peers[i].active &&
			    tnow - g_peers[i].last_active_ms > IDLE_TIMEOUT_S * 1000)
				peer_close(i, "idle timeout");
		/* periodic stats heartbeat — "a server you can't see into..." */
		if (tnow - last_stats > 30000) {
			logline("stats: served=%lu relayed=%lu errors=%lu kicked=%lu",
				g_peers_served, g_msgs_relayed, g_proto_errors, g_kicked);
			last_stats = tnow;
		}
	}

	do_shutdown("requested");
	if (g_listen_fd >= 0)
		close(g_listen_fd);
	close(g_sig_fd);
	return 0;
}
