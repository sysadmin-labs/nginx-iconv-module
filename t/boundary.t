# The reason this module needs a carry buffer at all: nginx hands the body
# to a filter in pieces of its own choosing, and a character does not care
# where those pieces end.  Every file here starts with one ASCII byte, so
# each 1024 byte boundary nginx reads at falls *inside* a multibyte
# character.  Without carrying the unfinished tail into the next buffer the
# conversion either loses bytes or fails outright.

use Test::Nginx::Socket 'no_plan';

repeat_each(2);

run_tests();

__DATA__

=== TEST 1: a two byte character straddles every input boundary
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        output_buffers 2 1k;
        iconv_buffer_size 64;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
--- user_files eval
">>> u.txt\n" . "x" . ("\xc3\xa4" x 3000)
--- request
GET /u/u.txt
--- response_body eval
"x" . ("\xe4" x 3000)
--- no_error_log
[alert]
[error]



=== TEST 2: a three byte character, so the carry is sometimes two bytes
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        output_buffers 2 1k;
        iconv_buffer_size 64;
        iconv_filter from=UTF-8 to=ISO-8859-15;
    }
--- user_files eval
">>> e.txt\n" . "x" . ("\xe2\x82\xac" x 2000)
--- request
GET /u/e.txt
--- response_body eval
"x" . ("\xa4" x 2000)
--- no_error_log
[alert]
[error]



=== TEST 3: the output buffer is smaller than the input, E2BIG every round
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        output_buffers 4 8k;
        iconv_buffer_size 64;
        iconv_filter from=ISO-8859-1 to=UTF-8;
    }
--- user_files eval
">>> g.txt\n" . ("\xe4" x 20000)
--- request
GET /u/g.txt
--- response_body eval
"\xc3\xa4" x 20000
--- no_error_log
[alert]
[error]



=== TEST 4: a body that is exactly one buffer long
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        output_buffers 2 1k;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
--- user_files eval
">>> k.txt\n" . ("\xc3\xa4" x 512)
--- request
GET /u/k.txt
--- response_body eval
"\xe4" x 512
--- no_error_log
[alert]
[error]



=== TEST 5: one single byte of body
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
--- user_files eval
">>> s.txt\n" . "a"
--- request
GET /u/s.txt
--- response_body chomp
a
--- no_error_log
[alert]
[error]



=== TEST 6: one single multibyte character and nothing else
--- config
    location /u/ {
        alias $TEST_NGINX_SERVER_ROOT/html/;
        iconv_buffer_size 64;
        iconv_filter from=UTF-8 to=ISO-8859-1;
    }
--- user_files eval
">>> m.txt\n" . "\xc3\xa4"
--- request
GET /u/m.txt
--- response_body eval
"\xe4"
--- no_error_log
[alert]
[error]
