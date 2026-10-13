use Test::Nginx::Socket 'no_plan';

repeat_each(2);

run_tests();

__DATA__

=== TEST 1: set_iconv turns UTF-8 into ISO-8859-1
--- config
    location = /t {
        set $src 'äöü';
        set_iconv $dst $src from=UTF-8 to=ISO-8859-1;
        return 200 $dst;
    }
--- request
GET /t
--- response_body eval
"\xe4\xf6\xfc"
--- no_error_log
[alert]



=== TEST 2: set_iconv the other way round
--- config
    location = /t {
        set_iconv $dst $src from=ISO-8859-1 to=UTF-8;
        return 200 $dst;
    }
--- request
GET /t?src=%e4
--- more_headers
--- response_body eval
"\xc3\xa4"
--- SKIP



=== TEST 3: from= and to= in either order
--- config
    location = /t {
        set $src 'ä';
        set_iconv $dst $src to=ISO-8859-1 from=UTF-8;
        return 200 $dst;
    }
--- request
GET /t
--- response_body eval
"\xe4"
--- no_error_log
[alert]



=== TEST 4: ASCII passes through untouched
--- config
    location = /t {
        set $src 'plain ascii, 1234';
        set_iconv $dst $src from=UTF-8 to=ISO-8859-1;
        return 200 $dst;
    }
--- request
GET /t
--- response_body chomp
plain ascii, 1234
--- no_error_log
[alert]



=== TEST 5: an empty source gives an empty result
--- config
    location = /t {
        set $src '';
        set_iconv $dst $src from=UTF-8 to=ISO-8859-1;
        return 200 "[$dst]";
    }
--- request
GET /t
--- response_body chomp
[]
--- no_error_log
[alert]



=== TEST 6: the filter converts a response body
--- config
    location = /t {
        iconv_filter from=UTF-8 to=ISO-8859-1;
        default_type text/plain;
        return 200 'Grüße';
    }
--- request
GET /t
--- response_body eval
"Gr\xfc\xdfe"
--- no_error_log
[alert]



=== TEST 7: the filter drops Content-Length, the length changes
--- config
    location = /t {
        iconv_filter from=UTF-8 to=ISO-8859-1;
        default_type text/plain;
        return 200 'äää';
    }
--- request
GET /t
--- response_headers
!Content-Length
--- response_body eval
"\xe4\xe4\xe4"
--- no_error_log
[alert]



=== TEST 8: an empty body with the filter on still answers
--- config
    location = /t {
        iconv_filter from=UTF-8 to=ISO-8859-1;
        default_type text/plain;
        return 200 '';
    }
--- request
GET /t
--- response_body chomp
--- error_code: 200
--- no_error_log
[alert]



=== TEST 9: a location without the filter is untouched
--- config
    location = /t {
        default_type text/plain;
        return 200 'äöü';
    }
--- request
GET /t
--- response_body eval
"\xc3\xa4\xc3\xb6\xc3\xbc"
--- no_error_log
[alert]



=== TEST 10: the filter is inherited into a nested location
--- config
    iconv_filter from=UTF-8 to=ISO-8859-1;
    location = /t {
        default_type text/plain;
        return 200 'ä';
    }
--- request
GET /t
--- response_body eval
"\xe4"
--- no_error_log
[alert]
