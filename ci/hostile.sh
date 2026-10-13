#!/bin/sh
#
# Put the filter behind an upstream that misbehaves and in front of a client
# that walks away.
#
# Everything else here hands the module a finished body from disk: t/ serves
# files, and nginx reads those in pieces of its own choosing.  That covers
# the carry across a *file read* boundary, which is what t/boundary.t nails
# down.  It does not cover the streaming case, where the pieces arrive from
# the network whenever they arrive, a character is split across two TCP
# segments, the length is unknown because the upstream speaks chunked, or
# the upstream dies in the middle of a character.
#
# usage: ci/hostile.sh <nginx binary>
#
# Set HOSTILE_LOAD_MODULE to the path of ngx_http_iconv_module.so to test a
# loadable build.

set -eu

NGINX=${1:?path to the nginx binary missing}
WORK=${CI_WORK:-$(cd "$(dirname "$0")/.." && pwd)/ci-work}/hostile
PORT=${HOSTILE_PORT:-18510}
UPORT=${HOSTILE_UPSTREAM_PORT:-18511}

command -v curl > /dev/null || { echo "curl(1) is needed" >&2; exit 2; }
command -v perl > /dev/null || { echo "perl(1) is needed" >&2; exit 2; }
command -v iconv > /dev/null || { echo "iconv(1) is needed" >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK/conf" "$WORK/logs" "$WORK/out" "$WORK/tmp"

fail=0
note() {
	echo "  $1" >&2
	fail=1
}

# ------------------------------------------------------------ the upstream
#
# Written by hand rather than taken from a library: the whole point is to
# control where the pieces end.  Seven bytes at a time against two byte
# characters guarantees that most pieces end inside one.

cat > "$WORK/upstream.pl" <<'PERL'
use strict;
use warnings;
use IO::Socket::INET;
use IO::Handle;

# A client that walks away mid-body would otherwise kill this process with
# SIGPIPE, and every case after it would answer 502 for the wrong reason.
$SIG{PIPE} = 'IGNORE';

my $port = shift or die "port missing\n";
my $srv = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $port,
                                Listen => 16, ReuseAddr => 1, Proto => 'tcp')
    or die "listen: $!\n";

# "ä" twice per line, so every line carries four bytes of UTF-8 and no
# piece boundary can be relied upon to fall between characters.
my $body = "\xc3\xa4\xc3\xb6" x 500;

while (my $c = $srv->accept) {
    $c->autoflush(1);
    my $req = '';
    while (my $line = <$c>) {
        $req .= $line;
        last if $line =~ /^\r?\n$/;
    }
    my ($path) = $req =~ m{^\w+\s+(\S+)};
    $path = '/' unless defined $path;

    if ($path =~ m{^/dribble}) {
        print $c "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               . "Content-Length: " . length($body) . "\r\n\r\n";
        for (my $i = 0; $i < length($body); $i += 7) {
            print $c substr($body, $i, 7);
            select undef, undef, undef, 0.001;
        }

    } elsif ($path =~ m{^/chunked}) {
        print $c "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               . "Transfer-Encoding: chunked\r\n\r\n";
        for (my $i = 0; $i < length($body); $i += 7) {
            my $p = substr($body, $i, 7);
            printf $c "%x\r\n%s\r\n", length($p), $p;
        }
        print $c "0\r\n\r\n";

    } elsif ($path =~ m{^/truncate}) {
        # Announces far more than it sends and cuts off inside a character.
        print $c "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               . "Content-Length: 100000\r\n\r\n";
        print $c "ab\xc3";

    } elsif ($path =~ m{^/empty}) {
        print $c "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               . "Content-Length: 0\r\n\r\n";

    } else {
        print $c "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    }
    close $c;
}
PERL

perl "$WORK/upstream.pl" "$UPORT" &
upstream_pid=$!
sleep 1

cleanup() {
	kill "$upstream_pid" 2>/dev/null || true
	[ -f "$WORK/logs/nginx.pid" ] &&
		kill -QUIT "$(cat "$WORK/logs/nginx.pid")" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --------------------------------------------------------------- the nginx

ngx_user=
if [ "$(id -u)" = 0 ]; then
	ngx_user="user root $(id -gn);"
fi

ngx_load=
if [ -n "${HOSTILE_LOAD_MODULE:-}" ]; then
	ngx_load="load_module $HOSTILE_LOAD_MODULE;"
fi

cat > "$WORK/conf/nginx.conf" <<EOF
$ngx_load
$ngx_user
daemon on;
master_process on;
worker_processes 1;
pid $WORK/logs/nginx.pid;
error_log $WORK/logs/error.log info;
events { worker_connections 64; }
http {
    access_log off;
    client_body_temp_path $WORK/tmp/cb;
    proxy_temp_path $WORK/tmp/px;
    fastcgi_temp_path $WORK/tmp/fc;
    uwsgi_temp_path $WORK/tmp/uw;
    scgi_temp_path $WORK/tmp/sc;
    default_type text/plain;

    server {
        listen 127.0.0.1:$PORT;

        location / {
            proxy_pass http://127.0.0.1:$UPORT;
            proxy_buffering off;

            iconv_buffer_size 64;
            iconv_filter from=UTF-8 to=ISO-8859-1;
        }

        location = /alive {
            return 200 'alive';
        }
    }
}
EOF

"$NGINX" -t -c "$WORK/conf/nginx.conf" -p "$WORK" > "$WORK/out/t.log" 2>&1 ||
	{ cat "$WORK/out/t.log"; exit 1; }
"$NGINX" -c "$WORK/conf/nginx.conf" -p "$WORK"
sleep 1

# What the body has to look like once converted: the upstream sends "äö" in
# UTF-8 five hundred times, so one byte each on the way out.
perl -e 'print "\xe4\xf6" x 500' > "$WORK/out/want.bin"

# ---------------------------------------------------------------- the cases

echo "--- a body that arrives seven bytes at a time ---"
curl -s --max-time 30 "http://127.0.0.1:$PORT/dribble" -o "$WORK/out/dribble.bin" || true
if cmp -s "$WORK/out/dribble.bin" "$WORK/out/want.bin"; then
	echo "  $(wc -c < "$WORK/out/dribble.bin" | tr -d ' ') bytes, byte for byte what it should be"
else
	note "the converted body differs from the expected one"
	note "got $(wc -c < "$WORK/out/dribble.bin" | tr -d ' ') bytes, wanted $(wc -c < "$WORK/out/want.bin" | tr -d ' ')"
fi

echo "--- the same body, chunked, so the wire bytes are not the body ---"
curl -s --max-time 30 "http://127.0.0.1:$PORT/chunked" -o "$WORK/out/chunked.bin" || true
if cmp -s "$WORK/out/chunked.bin" "$WORK/out/want.bin"; then
	echo "  $(wc -c < "$WORK/out/chunked.bin" | tr -d ' ') bytes, byte for byte what it should be"
else
	note "the chunked body came out wrong"
fi

echo "--- an upstream that promises 100000 bytes and sends three ---"
curl -s --max-time 10 "http://127.0.0.1:$PORT/truncate" -o "$WORK/out/cut.bin" || true
got=$(cat "$WORK/out/cut.bin" 2>/dev/null || true)
if [ "$got" = "ab" ]; then
	echo "  the two complete bytes came through, the half character did not"
else
	note "expected \"ab\", got \"$got\""
fi
# No log line is demanded here.  nginx notices the short upstream response
# by itself and ends the request, so the filter never sees a last buffer
# and never gets to say anything.  What matters is that the half character
# did not travel, and that is what the body check above shows.

echo "--- a client that hangs up in the middle ---"
curl -s --max-time 0.05 "http://127.0.0.1:$PORT/dribble" -o /dev/null 2>/dev/null || true
sleep 1
echo "  the worker is supposed to survive this, see the log check below"

echo "--- an empty body with the filter on ---"
code=$(curl -s -o "$WORK/out/empty.bin" -w '%{http_code}' --max-time 10 \
	"http://127.0.0.1:$PORT/empty" || true)
if [ "$code" = 200 ] && [ ! -s "$WORK/out/empty.bin" ]; then
	echo "  200 and nothing in the body"
else
	note "empty body: code $code, $(wc -c < "$WORK/out/empty.bin" | tr -d ' ') bytes"
fi

echo "--- a HEAD request ---"
code=$(curl -s -I -o /dev/null -w '%{http_code}' --max-time 10 \
	"http://127.0.0.1:$PORT/dribble" || true)
if [ "$code" = 200 ]; then
	echo "  200, and no body to convert"
else
	note "HEAD answered $code"
fi

echo "--- the worker is still there ---"
alive=$(curl -s --max-time 5 "http://127.0.0.1:$PORT/alive" || true)
if [ "$alive" = alive ]; then
	echo "  it answers"
else
	note "the worker did not answer after all that"
fi

echo "--- nothing in the error log that should not be there ---"
if grep -E '\[alert\]|\[emerg\]|exited on signal' "$WORK/logs/error.log"; then
	note "the log carries an alert, an emerg or a worker that died"
else
	echo "  no alert, no emerg, no signal"
fi

exit $fail
