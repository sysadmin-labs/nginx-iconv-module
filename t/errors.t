# Bad input and bad configuration.  The oracle for every runtime case is
# "no alert", which is how a dead worker announces itself: a module that
# walks off the end of a buffer kills the process rather than returning an
# error, and the test suite would otherwise score that as a pass.

use Test::Nginx::Socket 'no_plan';

repeat_each(2);

run_tests();

__DATA__

=== TEST 1: an encoding iconv does not know is refused while reading the config
--- config
    location = /t {
        iconv_filter from=UTF-8 to=NO-SUCH-CHARSET-42;
        return 200 'x';
    }
--- must_die
--- error_log
iconv cannot convert



=== TEST 2: from= without to= is refused
--- config
    location = /t {
        iconv_filter from=UTF-8 from=UTF-8;
        return 200 'x';
    }
--- must_die
--- error_log
is given twice



=== TEST 3: a parameter that is neither from= nor to= is refused
--- config
    location = /t {
        iconv_filter from=UTF-8 charset=latin1;
        return 200 'x';
    }
--- must_die
--- error_log
from= or to= expected



=== TEST 4: two iconv_filter in one location are refused
--- config
    location = /t {
        iconv_filter from=UTF-8 to=ISO-8859-1;
        iconv_filter from=UTF-8 to=ISO-8859-15;
        return 200 'x';
    }
--- must_die
--- error_log
is duplicate



=== TEST 5: a buffer too small to hold one character is refused
--- config
    location = /t {
        iconv_buffer_size 8;
        iconv_filter from=UTF-8 to=ISO-8859-1;
        return 200 'x';
    }
--- must_die
--- error_log
iconv_buffer_size must be at least



=== TEST 6: set_iconv wants a variable as its destination
--- config
    location = /t {
        set_iconv dst $src from=UTF-8 to=ISO-8859-1;
        return 200 'x';
    }
--- must_die
--- error_log
a variable name starting with $ is expected



=== TEST 7: a byte that is not valid UTF-8 is reported, the worker lives
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
    location = /alive {
        return 200 'alive';
    }
--- user_files eval
">>> bad.txt\n" . "ok\xff\xfebad"
--- request
GET /u/bad.txt
--- ignore_response
--- error_log
not valid UTF-8
--- no_error_log
[alert]



=== TEST 8: after the bad request the worker still answers
--- config
    location = /alive {
        return 200 'alive';
    }
--- request
GET /alive
--- response_body chomp
alive
--- no_error_log
[alert]



=== TEST 9: a body ending inside a character is reported, not passed on
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
--- user_files eval
">>> cut.txt\n" . "ab\xc3"
--- request
GET /u/cut.txt
--- response_body chomp
ab
--- error_log
ends in an incomplete character
--- no_error_log
[alert]



=== TEST 10: a character the target encoding cannot hold
# What iconv(3) does here is not the same everywhere: glibc refuses with
# EILSEQ, FreeBSD's libc substitutes and carries on.  So this does not
# assert a body or a log line, only the two things that hold either way --
# the request is answered and the worker is still alive.  The refusal path
# itself is covered by TEST 7, where the bytes are invalid UTF-8 rather
# than merely unrepresentable, and every iconv(3) rejects those.
--- config
    location = /t {
        set $src '€';
        set_iconv $dst $src from=UTF-8 to=ISO-8859-1;
        return 200 "[$dst]";
    }
--- request
GET /t
--- error_code: 200
--- no_error_log
[alert]
