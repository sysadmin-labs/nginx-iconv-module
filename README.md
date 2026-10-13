# nginx-iconv-module

[![Build & Test][build-test-badge]][build-test-link]
[![FreeBSD][freebsd-badge]][freebsd-link]
[![Hostile][hostile-badge]][hostile-link]
[![A/UBSan][sanitizers-badge]][sanitizers-link]
[![Valgrind][valgrind-badge]][valgrind-link]
[![Reload][reload-badge]][reload-link]
[![CodeQL][codeql-badge]][codeql-link]
[![Lint][lint-badge]][lint-link]

[build-test-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/build-test.yml/badge.svg
[build-test-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/build-test.yml
[freebsd-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/freebsd.yml/badge.svg
[freebsd-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/freebsd.yml
[hostile-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/hostile.yml/badge.svg
[hostile-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/hostile.yml
[sanitizers-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/sanitizers.yml/badge.svg
[sanitizers-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/sanitizers.yml
[valgrind-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/valgrind.yml/badge.svg
[valgrind-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/valgrind.yml
[reload-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/reload.yml/badge.svg
[reload-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/reload.yml
[codeql-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/codeql.yml/badge.svg
[codeql-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/codeql.yml
[lint-badge]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/lint.yml/badge.svg
[lint-link]: https://github.com/sysadmin-labs/nginx-iconv-module/actions/workflows/lint.yml

Convert between character encodings in nginx, through `iconv(3)`: a single
string into a variable, or a whole response body on its way out.

nginx can recode a response with `charset` and `charset_map`, but only
between a pair it has a table for, written out by hand in the
configuration.  This module hands the work to `iconv(3)` and reaches every
encoding the system knows.

## Directives

### set\_iconv

**syntax:** `set_iconv $destination $source from=<encoding> to=<encoding>`

**context:** http, server, location

Converts `$source` and puts the result in `$destination`.  The source may
be any value nginx can build, a variable or a string with variables in it.

```nginx
location /price {
    set $utf8 'Grüße';
    set_iconv $latin1 $utf8 from=UTF-8 to=ISO-8859-1;

    add_header X-Greeting $latin1;
    return 204;
}
```

The conversion runs when `$destination` is read, so it costs nothing in a
request that never looks at it.  A conversion that fails leaves the
variable unset, which nginx renders as empty, and says why in the error
log.

### iconv\_filter

**syntax:** `iconv_filter from=<encoding> to=<encoding>`

**context:** http, server, location

Converts the response body.  `Content-Length`, `ETag` and `Accept-Ranges`
are dropped, because the body that goes out is no longer the one they
describe.

```nginx
location /legacy {
    proxy_pass http://backend;

    iconv_filter from=ISO-8859-1 to=UTF-8;
}
```

A character split across two buffers is carried into the next one, so a
body arriving in pieces converts the same as one arriving whole.  A byte
sequence that is not what `from=` claims ends the response and is reported
in the error log.

### iconv\_buffer\_size

**syntax:** `iconv_buffer_size <size>`

**default:** one page

**context:** http, server, location

The size of each buffer the filter converts into.  It has to hold at least
64 bytes.

## Building

The module needs `iconv(3)`: in libc on glibc systems, in libiconv
elsewhere.  `config` finds either.

```sh
./configure --add-module=/path/to/nginx-iconv-module
```

or as a loadable module:

```sh
./configure --with-compat --add-dynamic-module=/path/to/nginx-iconv-module
```

```nginx
load_module modules/ngx_http_iconv_module.so;
```

## Testing

```sh
ci/build.sh 1.30.5 /tmp/nginx-iconv
TEST_NGINX_BINARY=/tmp/nginx-iconv/sbin/nginx prove -r t/
ci/hostile.sh /tmp/nginx-iconv/sbin/nginx
```

`t/boundary.t` is the one that matters: it serves files laid out so that
every buffer nginx reads ends inside a character.  `ci/hostile.sh` does the
same from the network, where the pieces are smaller and the length is
sometimes unknown.

## License

BSD 2-Clause, see [LICENSE](LICENSE).
