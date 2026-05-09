/*
 * tcp-proxy-worker.js — Cloudflare Worker that bridges WebSocket
 * frames to a TCP connection. Pair with the WASM kernel's `tcp`
 * shell command:
 *
 *   osito> tcp connect example.com 80 mytcp
 *   osito> ws send mytcp "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n"
 *   osito> ws recv mytcp
 *
 * Deploy:
 *   wrangler deploy tools/tcp-proxy-worker.js \
 *     --name tcp-proxy --route tcp-proxy.naranjositos.tech/*
 *
 * Cloudflare exposes raw TCP via the `connect()` API in Workers
 * (cloudflare:sockets). Documentation:
 *   https://developers.cloudflare.com/workers/runtime-apis/tcp-sockets/
 *
 * Security: this is an unauthenticated TCP proxy. In production you
 * MUST add an allowlist (e.g. only api.anthropic.com:443) or
 * authentication (signed token in the URL) so it can't be turned
 * into an open relay.
 */

import { connect } from 'cloudflare:sockets';

export default {
    async fetch(request, env) {
        if (request.headers.get('Upgrade') !== 'websocket') {
            return new Response(
                'tcp-proxy: WebSocket required.\n' +
                'Usage: wss://this-worker/?host=<host>&port=<port>\n',
                { status: 426 }
            );
        }

        const url = new URL(request.url);
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
