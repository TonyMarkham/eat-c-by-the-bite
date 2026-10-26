/* bite 71 — WebSocket Signaling Channel in C (libwebsockets)
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/71-ws-server/ws_relay.c -o /tmp/b71 \
 *       -lwebsockets
 *   /tmp/b71 &
 *   # then either open http://localhost:8081 in a browser and run in devtools:
 *   #   const ws = new WebSocket("ws://localhost:8081"); ws.onmessage = e => console.log(e.data);
 *   #   ws.send("hello");   -> any OTHER connected tab receives it
 *   # or run the local test double (no browser needed):
 *   #   gcc -Wall -Wextra -Werror -std=c17 -g bites/71-ws-server/ws_probe.c -o /tmp/b71probe -lcrypto
 *   #   timeout 10 /tmp/b71probe 127.0.0.1 8081
 *
 * WHY THIS BITE EXISTS
 *   WebRTC has no transport of its own for setting up calls: offers, answers
 *   and ICE candidates travel over whatever channel YOU provide. For a
 *   backend that channel is almost always a WebSocket. So: a WS server in C
 *   that relays any message from one connected client to all others IS your
 *   signaling plane skeleton (bite 72 adds addressing + JSON on top).
 *
 * THE LEARN LIST, MAPPED TO CODE
 *   - WebSocket = HTTP upgrade then a tiny frame protocol. libwebsockets does
 *     both halves for us; the comments below point at where each piece lives
 *     on the wire so the magic is not magic (you did HTTP in bite 55 and
 *     framing in bite 54).
 *   - Integration shape: one more fd type in the bite-51 event loop — lws
 *     owns its own poll set; we just keep calling lws_service().
 *   - Robustness: fragmented messages, ping/pong keepalive, abrupt
 *     disconnects — see the callback cases.
 *
 * BREAK IT ON PURPOSE (as required by the bite):
 *   1. Kill a client mid-session (Ctrl-C the probe with only one tab open,
 *      or close a tab): the callback must log CLOSED and remove the peer,
 *      and the relay must never write to the dead wsi again.
 *   2. Send a 1 MB message from the browser console:
 *        ws.send("x".repeat(1000000));
 *      Watch the rx-buffer / fragmentation notes below: with the default
 *      small rx buffer lws delivers it in several LWS_CALLBACK_RECEIVE calls
 *      (lws_is_final_fragment() tells you which is last). This server relays
 *      per-received-chunk, which is *correct* for a relay: chunking is the
 *      stream nature of WS frames showing through. If you need whole
 *      messages, accumulate in per-session user data until final.
 *   3. Valgrind: run the server under valgrind, connect+disconnect the probe
 *      twice, SIGTERM — zero leaks expected (lws frees its own state on
 *      lws_context_destroy; we allocate nothing per connection here).
 */

#include <libwebsockets.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELAY_PORT   8081
#define MAX_CLIENTS  8              /* signaling room: two tabs is the demo, 8 is generous */
#define RX_BUF_SIZE  (64 * 1024)    /* per-connection receive buffer lws will grow into */
#define RELAY_MAX_MSG (1 << 20)     /* largest message we will ever put on the wire */

static struct lws *g_clients[MAX_CLIENTS];   /* established websocket handles */
static int g_nclients = 0;
static volatile sig_atomic_t g_stop = 0;
static unsigned long g_msgs_relayed = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* The one thing a relay does: give the message to everyone else.      */
/* ------------------------------------------------------------------ */
static void relay_to_others(struct lws *sender, const unsigned char *in, size_t len)
{
	/*
	 * LWS_PRE is reserved headroom in every outgoing buffer: lws writes the
	 * WS frame header *before* your payload, in place. Forgetting it is the
	 * classic lws bug — the frame header then overwrites your data.
	 */
	static unsigned char buf[RELAY_MAX_MSG + LWS_PRE]; /* 1 MiB ceiling, static: no per-msg malloc */
	size_t copy = len < RELAY_MAX_MSG ? len : RELAY_MAX_MSG;

	memcpy(buf + LWS_PRE, in, copy);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_clients[i] && g_clients[i] != sender) {
			/* LWS_WRITE_TEXT: opcode 0x1 — one text frame. lws handles
			 * fin/opcode bits, masking (server->client is unmasked) and
			 * TCP chunking of the payload internally. */
			if (lws_write(g_clients[i], buf + LWS_PRE, copy, LWS_WRITE_TEXT) < 0)
				lwsl_user("relay: write failed to client %d (peer going away?)\n", i);
			else
				g_msgs_relayed++;
		}
	}
}

static int callback_relay(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	(void)user;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		lwsl_user("relay: protocol init (serving ws://0.0.0.0:%d)\n", RELAY_PORT);
		break;

	case LWS_CALLBACK_ESTABLISHED:
		/* The HTTP upgrade (bite 55 territory: request line + headers) has
		 * succeeded — lws already answered the Sec-WebSocket-Key challenge
		 * with SHA1(key + GUID), RFC 6455 s1.3. We now hold a live fd. */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (!g_clients[i]) {
				g_clients[i] = wsi;
				break;
			}
		}
		g_nclients++;
		lwsl_user("relay: client connected (%d now online)\n", g_nclients);
		break;

	case LWS_CALLBACK_RECEIVE: {
		/* One message (or one *fragment* of a jumbo message) arrived.
		 * lws already: stripped the frame header, unmasked the payload
		 * (client->server frames are always masked, RFC 6455 s5.3) and
		 * reassembled if it fit the rx buffer. */
		unsigned char op = (unsigned char)lws_frame_is_binary(wsi) ? 2 : 1;
		lwsl_user("relay: recv opcode=%u (%s) len=%zu final=%d from %p\n",
			  op, op == 1 ? "text" : "binary", len,
			  lws_is_final_fragment(wsi), (void *)wsi);
		relay_to_others(wsi, in, len);
		break;
	}

	case LWS_CALLBACK_CLOSED:
		/* Abrupt disconnect, clean close, or protocol error all funnel here.
		 * Remove the dead wsi so relay_to_others can never touch it again —
		 * this is the bite-79 lifecycle discipline in miniature: every
		 * connection has exactly one owner (this slot) and one exit path. */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (g_clients[i] == wsi) {
				g_clients[i] = NULL;
				break;
			}
		}
		g_nclients--;
		if (g_nclients < 0)
			g_nclients = 0;
		lwsl_user("relay: client gone (%d now online)\n", g_nclients);
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		lwsl_user("relay: shutdown after %lu messages relayed\n", g_msgs_relayed);
		break;

	default:
		break;
	}
	return 0;
}

static const struct lws_protocols protocols[] = {
	{ "relay", callback_relay, 0, RX_BUF_SIZE, 0, NULL, 0 },
	LWS_PROTOCOL_LIST_TERM
};

int main(void)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	/* Silence lws's own chatter so our logs are readable; raise it to
	 * LLL_INFO | LLL_WARN while debugging the handshake. */
	lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN, NULL);

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port    = RELAY_PORT;
	info.protocols = protocols;
	info.gid = info.uid = -1;

	struct lws_context *ctx = lws_create_context(&info);
	if (!ctx) {
		lwsl_err("lws_create_context failed (port %d in use?)\n", RELAY_PORT);
		return 1;
	}

	/* The event loop: bite 51's poll loop, delegated. lws_service() runs
	 * timers, keepalive pings (it answers pings, and sends pings when
	 * idle), reads and writes. Timeout in ms. */
	while (!g_stop && lws_service(ctx, 200) >= 0)
		;

	lwsl_user("relay: shutting down gracefully\n");
	lws_context_destroy(ctx);   /* closes every socket, frees all lws state */
	return 0;
}
