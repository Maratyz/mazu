#!/usr/bin/env python3
"""TLS relay proxy for Mazu kernel.

Accepts plaintext HTTP from the Mazu guest, terminates TLS on behalf of
the client, and forwards the request to the upstream HTTPS API.  The relay
rewrites the Host header and pipes the response back in plaintext.

Usage:
    python3 tools/tls_relay.py [--listen HOST:PORT] [--allow HOST ...]

    --listen  Local address to bind (default 0.0.0.0:8443).
    --allow   Upstream hostnames permitted for relay (default: allow all).
              Example: --allow api.anthropic.com api.openai.com

The guest kernel sends a normal HTTP request to this relay with an
X-Upstream-Host header indicating the real destination:

    GET /v1/messages HTTP/1.1
    Host: 10.0.2.2:8443
    X-Upstream-Host: api.anthropic.com

The relay connects to https://api.anthropic.com/v1/messages, copies all
request headers (replacing Host), forwards the request body, and streams
the response back to the guest in plaintext HTTP.
"""

import argparse
import http.server
import ssl
import socket
import sys
import threading
import http.client


class RelayHandler(http.server.BaseHTTPRequestHandler):
    """Relay incoming plaintext HTTP to upstream HTTPS."""

    server_version = "MazuTLSRelay/1.0"
    protocol_version = "HTTP/1.1"
    allowed_hosts = None  # Set by main()

    def do_request(self):
        upstream_host = self.headers.get("X-Upstream-Host")
        if not upstream_host:
            self.send_error(400, "Missing X-Upstream-Host header")
            return

        # Enforce allowlist if configured.
        if self.allowed_hosts and upstream_host not in self.allowed_hosts:
            self.send_error(403, "Host not in allowlist: %s" % upstream_host)
            return

        # Read request body if present.
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else None

        # Build upstream headers: copy everything except hop-by-hop and relay
        # headers, replace Host.
        skip = {"host", "x-upstream-host", "connection", "transfer-encoding"}
        upstream_headers = {}
        for key, val in self.headers.items():
            if key.lower() not in skip:
                upstream_headers[key] = val
        upstream_headers["Host"] = upstream_host

        try:
            ctx = ssl.create_default_context()
            conn = http.client.HTTPSConnection(upstream_host, context=ctx,
                                               timeout=60)
            conn.request(self.command, self.path, body=body,
                         headers=upstream_headers)
            resp = conn.getresponse()
        except Exception as e:
            self.send_error(502, "Upstream connection failed: %s" % e)
            return

        # Relay upstream response back to the guest.
        self.send_response_only(resp.status, resp.reason)

        # Forward response headers.
        chunked = False
        for key, val in resp.getheaders():
            low = key.lower()
            if low == "transfer-encoding" and "chunked" in val.lower():
                chunked = True
            # Skip hop-by-hop headers that don't apply to our plaintext link.
            if low in ("connection",):
                continue
            self.send_header(key, val)
        self.send_header("Connection", "close")
        self.end_headers()

        # Stream body.
        try:
            while True:
                chunk = resp.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            conn.close()

    do_GET = do_request
    do_POST = do_request
    do_PUT = do_request
    do_DELETE = do_request
    do_PATCH = do_request

    def log_message(self, fmt, *args):
        sys.stderr.write("[relay] %s %s\n" % (self.client_address[0],
                                               fmt % args))


class ThreadedHTTPServer(http.server.HTTPServer):
    """Handle each request in a new thread for concurrent relay."""
    allow_reuse_address = True

    def process_request(self, request, client_address):
        t = threading.Thread(target=self.process_request_thread,
                             args=(request, client_address))
        t.daemon = True
        t.start()

    def process_request_thread(self, request, client_address):
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen", default="0.0.0.0:8443",
                        help="Local address to bind (default: 0.0.0.0:8443)")
    parser.add_argument("--allow", nargs="*", default=None,
                        help="Allowed upstream hostnames (default: allow all)")
    args = parser.parse_args()

    host, port = args.listen.rsplit(":", 1)
    port = int(port)

    if args.allow:
        RelayHandler.allowed_hosts = set(args.allow)
        sys.stderr.write("[relay] Allowlist: %s\n" % ", ".join(args.allow))
    else:
        sys.stderr.write("[relay] WARNING: no allowlist, all hosts permitted\n")

    server = ThreadedHTTPServer((host, port), RelayHandler)
    sys.stderr.write("[relay] Listening on %s:%d\n" % (host, port))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        sys.stderr.write("\n[relay] Shutting down\n")
        server.shutdown()


if __name__ == "__main__":
    main()
