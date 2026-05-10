/*
 * tcp-proxy-worker.js — Cloudflare Worker that bridges WebSocket
 * frames ↔ raw TCP. Two modes:
 *
 * 1. Outbound TCP (existing): wss://this/?host=…&port=…
 *    Each WS connection opens one TCP socket to (host, port).
 *
 * 2. Inbound TCP (new): wss://this/listen?room=ID
 *    Receivers listen on a "room" id; senders connect to the same
 *    room with /connect?room=ID. Bytes flow bidirectionally between
 *    the two WebSockets — emulates accept() for kernel-side servers
 *    that want to host a service in the browser.
 *
 *    We cannot literally accept() inbound IPv4 in a Worker — instead,
 *    multiple browser-side senders rendezvous via room IDs, which
 *    suffices for OsitoK→OsitoK demos and any client that knows the
 *    URL.
 *
 * Pair with the WASM kernel's `tcp` and `httpd-ws` shell commands:
 *
 *   osito> tcp connect example.com 80 mytcp
 *   osito> ws send mytcp "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n"
 *   osito> ws recv mytcp
 *
 * Deploy:
 *   wrangler deploy tools/tcp-proxy-worker.js \
 *     --name tcp-proxy --route tcp-proxy.naranjositos.tech/*
 *
 * For room mode the worker needs Durable Objects (sticky room state)
 * — see RoomDO below.
 *
 * Security: unauthenticated TCP proxy. In production add an allowlist
 * (only api.anthropic.com:443 etc.) or signed-token auth before
 * deploying publicly.
 */

import { connect } from 'cloudflare:sockets';

/* ── Durable Object: rendezvous room ────────────────────────────
 * Holds 0..2 WebSockets keyed by URL path /listen?room=X or
 * /connect?room=X. When both sides are connected, message events on
 * either side are forwarded to the other. The first to leave closes
 * the pair.
 * ─────────────────────────────────────────────────────────────── */
export class RoomDO {
    constructor(state, env) {
        this.state = state;
        this.env = env;
        this.listener = null;   // first WS to arrive (server)
        this.connector = null;  // second WS (client)
    }

    relay(from, to) {
        from.addEventListener('message', (ev) => {
            try { to.send(ev.data); } catch (e) {}
        });
        from.addEventListener('close', () => {
            try { to.close(1000, 'peer closed'); } catch (e) {}
            if (this.listener === from) this.listener = null;
            if (this.connector === from) this.connector = null;
        });
    }

    async fetch(request) {
        if (request.headers.get('Upgrade') !== 'websocket') {
            return new Response('room: WebSocket required', { status: 426 });
        }
        const url = new URL(request.url);
        const role = url.pathname.endsWith('/connect') ? 'connect' : 'listen';
        const pair = new WebSocketPair();
        const client = pair[0], server = pair[1];
        server.accept();

        if (role === 'listen') {
            if (this.listener) {
                server.close(1008, 'room already has a listener');
                return new Response(null, { status: 101, webSocket: client });
            }
            this.listener = server;
            if (this.connector) {
                /* Connector arrived first — relay both directions now */
                this.relay(this.listener, this.connector);
                this.relay(this.connector, this.listener);
            }
        } else {
            this.connector = server;
            if (this.listener) {
                this.relay(this.listener, this.connector);
                this.relay(this.connector, this.listener);
            }
        }
        return new Response(null, { status: 101, webSocket: client });
    }
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        /* Room rendezvous mode: /listen, /connect — Durable Object
         * pairs two WSs by room id so browser kernels can host
         * server-side services for other browser kernels. */
        if (url.pathname === '/listen' || url.pathname === '/connect') {
            const room = url.searchParams.get('room') || 'default';
            const id = env.ROOMS.idFromName(room);
            const stub = env.ROOMS.get(id);
            return stub.fetch(request);
        }

        if (request.headers.get('Upgrade') !== 'websocket') {
            return new Response(
                'tcp-proxy: WebSocket required.\n' +
                'Outbound: wss://this/?host=<host>&port=<port>\n' +
                'Inbound:  wss://this/listen?room=<id>  (server side)\n' +
                '          wss://this/connect?room=<id> (client side)\n',
                { status: 426 }
            );
        }

        const host = url.searchParams.get('host');
        const port = parseInt(url.searchParams.get('port'), 10);
        if (!host || !port || port < 1 || port > 65535) {
            return new Response('bad host/port', { status: 400 });
        }

        // OPTIONAL — uncomment to restrict targets:
        // const ALLOW = ['api.anthropic.com', 'example.com'];
        // if (!ALLOW.includes(host)) {
        //     return new Response('host not allowed', { status: 403 });
        // }

        const wsPair = new WebSocketPair();
        const client = wsPair[0];
        const server = wsPair[1];

        server.accept();

        let socket;
        try {
            socket = connect({ hostname: host, port });
        } catch (e) {
            server.close(1011, 'connect failed: ' + e.message);
            return new Response(null, { status: 101, webSocket: client });
        }

        // TCP → WS direction
        (async () => {
            const reader = socket.readable.getReader();
            try {
                for (;;) {
                    const { value, done } = await reader.read();
                    if (done) break;
                    server.send(value);
                }
            } catch (e) {
                // Connection died — close the WS so the client notices
            } finally {
                try { server.close(1000, 'tcp eof'); } catch (e) {}
            }
        })();

        // WS → TCP direction
        const writer = socket.writable.getWriter();
        server.addEventListener('message', async (ev) => {
            const data = typeof ev.data === 'string'
                ? new TextEncoder().encode(ev.data)
                : new Uint8Array(ev.data);
            try {
                await writer.write(data);
            } catch (e) {
                try { server.close(1011, 'tcp write failed'); } catch (_) {}
            }
        });

        server.addEventListener('close', () => {
            try { socket.close(); } catch (e) {}
        });

        return new Response(null, { status: 101, webSocket: client });
    },
};
