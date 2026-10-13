# Changelog

Newest first.  Dates are release dates.

## v0.1.0 (2026-10-13)

### Added

- First release.  Written from scratch rather than taken over: the module
  this replaces in the FreeBSD nginx ports had its last change to the
  module code in May 2016, carried a BSD 3-Clause notice that lives only in
  its README, and required `ngx_devel_kit` for nothing but a hook into the
  rewrite phase.

- `set_iconv $dst $src from= to=` converts a value into a variable, and
  `iconv_filter from= to=` converts a response body.  Both keep the
  spelling the ports have always offered, so a configuration that used the
  old module needs no edit.

- No `ngx_devel_kit`.  The destination variable computes itself when it is
  read, which removes the dependency and, with it, a phase ordering
  problem: a handler in the rewrite phase runs *after* the one belonging to
  `ngx_http_rewrite_module`, so `return 200 $dst` in the same location
  answered before the value existed.

- `iconv_filter` drops `Content-Length`, `ETag` and `Accept-Ranges`.  The
  body that leaves is not the one they describe, and a range of it is not a
  range of what the client would get.

- A conversion whose encodings `iconv_open()` rejects is refused while the
  configuration is read, not at the first request.  Finding out later means
  a 500 for whoever asks first and a line in a log nobody is watching.

- `t/boundary.t`: every file in it starts with one ASCII byte, so each
  boundary nginx reads at falls *inside* a multibyte character.  Without
  carrying the unfinished tail into the next buffer the conversion loses
  bytes, and the test says so -- proven against a planted defect before it
  was written down: with the carry removed `t/sanity.t` stays green while
  `boundary.t` fails 12 of its 48 checks.

- `ci/hostile.sh`: the same question from the network instead of from
  disk.  An upstream that dribbles seven bytes at a time, one that speaks
  chunked so the wire bytes are not the body, one that announces 100000
  bytes and sends three, a client that hangs up mid-stream, an empty body
  and a HEAD request.

- The conversion descriptor is closed from a pool cleanup rather than at
  the end of the body filter.  A request aborted mid-stream never reaches
  the last buffer, and one leaked descriptor per request is the kind of
  thing that surfaces weeks later.

### Fixed

Two defects that only the streaming cases could show, both found by
`ci/hostile.sh` before the first release and neither reachable from a suite
that serves files:

- An output buffer went into the chain before the conversion ran, so a
  piece that was nothing but the start of a character left an empty buffer
  behind.

- The buffer that carries `last_buf` and `flush` kept `temporary` set from
  its previous use.  `ngx_buf_special()` only counts a buffer as special
  when it is not in memory, so nginx logged every one of them as "zero size
  buf in writer" -- an alert on each streamed request.

### Portability

- `config` probes with the same flags the build will use.  On FreeBSD
  `iconv_open` is in libc, but the ports tree also installs GNU libiconv
  under `/usr/local` and nginx puts that directory on the compile line by
  itself.  GNU's header then renames `iconv_open` to `libiconv_open` behind
  a `#define`, so a probe run without that include path answers a question
  the build never asks: it finds libc, and the link fails against names
  that are not there.  `LIBICONV_PLUG` tells the header to leave the names
  alone, and the probe carries it.

- The define goes into `CFLAGS`, not into the include list.  nginx writes
  `-I` in front of every entry of `CORE_INCS`, so a `-D` placed there comes
  out as `-I -DLIBICONV_PLUG` and is swallowed as the include's argument,
  silently.

- For a module built into the binary, `auto/module` does not keep
  `ngx_module_incs` and `ngx_module_libs` per module; those are for the
  dynamic case.  The static branch adds them to the global lists itself.

- What `iconv(3)` does with a character the target encoding cannot hold is
  not the same everywhere: glibc refuses with EILSEQ, FreeBSD's libc
  substitutes.  The suite therefore asserts the refusal only where the
  input is invalid rather than merely unrepresentable, which every
  `iconv(3)` rejects.
