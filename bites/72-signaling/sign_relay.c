/* bite 72 — The Signaling Protocol (JSON relay with peer registry, cJSON)
 *
 * COMPILE + RUN (inside WSL, from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/72-signaling/sign_relay.c \
 *       -o /tmp/b72 -lwebsockets -lcjson
 *   /tmp/b72 &
 *   # browser path: two tabs on http://localhost:8082, devtools console:
 *   #   const ws = new WebSocket("ws://localhost:8082");
 *   #   ws.onmessage = e => console.log("got:", e.data);
 *   #   ws.send('{"type":"join","id":"a"}');
 *   #   ws.send('{"type":"offer","to":"b","sdp":"v=0..."}');
 *   # test double (no browser): gcc ... bites/72-signaling/sign_probe.c -o /tmp/b72probe -lcrypto
 *   #                            timeout 15 /tmp/b72probe
 *
 * THE PROTOCOL (bite-53 discipline: spec before code, strict parse)
 * ----------------------------------------------------------------
 * Transport: WebSocket text frames, one JSON object per frame.
 * Envelope:  {"type":"join","id":"<peer id>"}
 *            {"type":"offer","to":"<peer id>","sdp":"<sdp body>"}
 *            {"type":"answer","to":"<peer id>","sdp":"<sdp body>"}
 *            {"type":"candidate","to":"<peer id>","candidate":"<ice line>"}
 * Server behavior:
 *   - join registers the connection under that id; duplicate id -> error.
 *   - offer/answer/candidate are RELAYED VERBATIM (the server is a router,
 *     never a participant — it generates no SDP, that is media-server
 *     territory, bite 77).
 *   - unknown "type"          -> {"type":"error","reason":"unknown type"}
 *   - missing type / bad JSON -> {"type":"error","reason":...}
 *   - "to" that never joined  -> {"type":"error","reason":"unknown peer"}
 *   Errors go back to the SENDER; garbage is never relayed to a third party.
 *
 * WHY cJSON
 *   WebRTC does not mandate a wire format for signaling — only that both
 *   peers agree. But the de-facto format is JSON (Socket.IO, websocket
 *   examples, pion demos all speak it), so a C backend needs a JSON parser.
 *   cJSON is ~1k lines, no dependencies, and models JSON as a tree you walk
 *   with cJSON_GetObjectItemCaseSensitive. We parse EVERY message and treat
 *   any parse failure as a protocol error: a signaling server that forwards
 *   garbage is a DoS amplifier.
 *
 * BREAK IT ON PURPOSE (probe exercises all of these):
 *   malformed JSON, missing type, unknown type, unknown "to", duplicate
 *   join id, a 10 KB message (should route fine — size limit check).
 *   Each must produce a clean error to the sender, never a crash, never a
 *   relay of garbage. Run the probe and watch each case handled.
 */

#define _POSIX_C_SOURCE 200809L
#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELAY_PORT   8082
#define MAX_PEERS    8
/* Deliberately SMALL rx buffer: forces lws to deliver jumbo messages in
 * several LWS_CALLBACK_RECEIVE chunks, which exercises our reassembler —
 * the same reason bite 48 made you read-exactly N bytes. */
#define RX_BUF_SIZE  (4 * 1024)
#define MAX_MSG      (16 * 1024)   /* 10 KB abuse message must fit; a deliberate cap */

/* The peer registry: connection (wsi) -> peer id. Two parallel slots keep the
 * lookup O(n) over a tiny n — a hash map here would be bite-30 cosplay. */
static struct {
	struct lws *wsi;
	char        id[64];
	int         active;
} g_peers[MAX_PEERS];

/* Per-connection state, one per live wsi (lws gives us a slot via the
 * per_session_data_size field in the protocol struct). This is where message
 * reassembly lives: WebSocket messages larger than the rx buffer arrive as
 * several LWS_CALLBACK_RECEIVE calls, and a JSON message only makes sense
 * once it is complete. Ownership: exactly one accumulator per connection,
 * reset after every final fragment — the bite-79 lifecycle in miniature. */
typedef struct {
	char    acc[MAX_MSG + 1];
	size_t  len;
	int     overflow;   /* set when a message blew the cap; drop until final */
} sess_t;

static volatile sig_atomic_t g_stop = 0;
static unsigned long g_relayed = 0, g_errors = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int peer_find_by_wsi(struct lws *wsi)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active && g_peers[i].wsi == wsi)
			return i;
	return -1;
}

static int peer_find_by_id(const char *id)
{
	for (int i = 0; i < MAX_PEERS; i++)
		if (g_peers[i].active && strcmp(g_peers[i].id, id) == 0)
			return i;
	return -1;
}

static void send_json(struct lws *to, const char *json)
{
	static unsigned char buf[MAX_MSG + LWS_PRE];
	size_t len = strlen(json);
	if (len > MAX_MSG)
		len = MAX_MSG;
	memcpy(buf + LWS_PRE, json, len);
	if (lws_write(to, buf + LWS_PRE, len, LWS_WRITE_TEXT) < 0)
		lwsl_user("relay: write failed\n");
}

static void send_error(struct lws *to, const char *reason)
{
	char buf[192];
	snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"reason\":\"%s\"}", reason);
	send_json(to, buf);
	g_errors++;
}

static void relay_envelope(int from_idx, cJSON *msg, const char *raw, size_t rawlen)
{
	(void)rawlen;  /* kept for symmetry with send_json's eventual length param */
	/* Relay the ORIGINAL text (not a re-serialization): a relay that
	 * round-trips through the parser can silently drop or reorder fields.
	 * We only validated; we don't rewrite. */
	cJSON *to = cJSON_GetObjectItemCaseSensitive(msg, "to");
	if (!cJSON_IsString(to) || to->valuestring[0] == '\0') {
		send_error(g_peers[from_idx].wsi, "missing 'to'");
		return;
	}
	int dst = peer_find_by_id(to->valuestring);
	if (dst < 0) {
		send_error(g_peers[from_idx].wsi, "unknown peer");
		return;
	}
	if (dst == from_idx) {
		send_error(g_peers[from_idx].wsi, "cannot signal yourself");
		return;
	}
	send_json(g_peers[dst].wsi, raw);
	g_relayed++;
}

static void handle_message(struct lws *wsi, const char *raw, size_t rawlen)
{
	int from = peer_find_by_wsi(wsi);
	/* Parse first, always. cJSON returns NULL on any structural error. */
	cJSON *msg = cJSON_ParseWithLength(raw, rawlen);
	if (!msg) {
		send_error(wsi, "malformed json");
		return;
	}

	cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
	if (!cJSON_IsString(type) || type->valuestring[0] == '\0') {
		send_error(wsi, "missing 'type'");
		cJSON_Delete(msg);
		return;
	}

	if (strcmp(type->valuestring, "join") == 0) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
		if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
			send_error(wsi, "join needs 'id'");
		} else if (peer_find_by_id(id->valuestring) >= 0) {
			send_error(wsi, "id already taken");
		} else if (from < 0) {
			int slot = -1;
			for (int i = 0; i < MAX_PEERS; i++)
				if (!g_peers[i].active) {
					slot = i;
					break;
				}
			if (slot < 0) {
				send_error(wsi, "peer table full");
			} else {
				/* Bound the id: an id longer than the registry entry
				 * is a memory-exhaustion trick, so clamp it. */
				snprintf(g_peers[slot].id, sizeof(g_peers[slot].id),
					 "%.63s", id->valuestring);
				g_peers[slot].wsi = wsi;
				g_peers[slot].active = 1;
				lwsl_user("relay: joined as '%s'\n", g_peers[slot].id);
				char ok[96];
				snprintf(ok, sizeof(ok),
					 "{\"type\":\"joined\",\"id\":\"%.63s\"}", g_peers[slot].id);
				send_json(wsi, ok);
			}
		} else {
			send_error(wsi, "already joined");
		}
	} else if (strcmp(type->valuestring, "offer") == 0 ||
		   strcmp(type->valuestring, "answer") == 0 ||
		   strcmp(type->valuestring, "candidate") == 0) {
		if (from < 0)
			send_error(wsi, "join first");
		else
			relay_envelope(from, msg, raw, rawlen);
	} else {
		send_error(wsi, "unknown type");
	}

	cJSON_Delete(msg);   /* every parse gets exactly one delete — bite 26 ownership */
}

static int callback_relay(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	sess_t *s = (sess_t *)user;
	switch (reason) {
	case LWS_CALLBACK_RECEIVE: {
		/* lws delivers a logical message in chunks when it exceeds the
		 * rx buffer (it deliberately keeps the rx buffer small to cap
		 * per-connection memory). Accumulate until the final fragment,
		 * then parse ONCE. This is bite 48's "TCP is a byte stream"
		 * lesson reappearing one layer up. */
		size_t avail = (size_t)(MAX_MSG - s->len);
		if (s->overflow || len > avail) {
			if (!s->overflow)
				send_error(wsi, "message too large");
			s->overflow = 1;   /* keep swallowing until the final fragment */
		} else {
			memcpy(s->acc + s->len, in, len);
			s->len += len;
		}
		if (lws_is_final_fragment(wsi)) {
			s->acc[s->len] = '\0';
			if (!s->overflow) {
				lwsl_user("relay: recv %zu bytes from %p\n", s->len, (void *)wsi);
				handle_message(wsi, s->acc, s->len);
			}
			s->len = 0;
			s->overflow = 0;
		}
		break;
	}
	case LWS_CALLBACK_ESTABLISHED:
		lwsl_user("relay: ws connected (unregistered until 'join')\n");
		break;
	case LWS_CALLBACK_CLOSED: {
		int i = peer_find_by_wsi(wsi);
		if (i >= 0) {
			lwsl_user("relay: peer '%s' disconnected\n", g_peers[i].id);
			g_peers[i].active = 0;   /* the id is now reusable */
			g_peers[i].wsi = NULL;
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static const struct lws_protocols protocols[] = {
	/* name, callback, per_session_data_size, rx_buffer_size, ... */
	{ "signal", callback_relay, sizeof(sess_t), RX_BUF_SIZE, 0, NULL, 0 },
	LWS_PROTOCOL_LIST_TERM
};

int main(void)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN, NULL);

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = RELAY_PORT;
	info.protocols = protocols;
	info.gid = info.uid = -1;

	struct lws_context *ctx = lws_create_context(&info);
	if (!ctx) {
		lwsl_err("cannot create context on port %d\n", RELAY_PORT);
		return 1;
	}
	lwsl_user("signaling relay on ws://0.0.0.0:%d (protocol: see file header)\n", RELAY_PORT);

	while (!g_stop && lws_service(ctx, 200) >= 0)
		;

	lwsl_user("relay: %lu messages relayed, %lu protocol errors; shutting down\n",
		  g_relayed, g_errors);
	lws_context_destroy(ctx);
	return 0;
}
