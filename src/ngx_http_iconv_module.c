
/*
 * Copyright (c) 2026, Jochen Neumeister <joneum@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <iconv.h>


/*
 * The longest shift or multibyte sequence we are willing to carry from one
 * buffer to the next.  Four bytes covers UTF-8 and GB18030; the stateful
 * ISO-2022 encodings use longer escape sequences, hence the headroom.
 */
#define NGX_HTTP_ICONV_CARRY_MAX  64


typedef struct {
    ngx_http_complex_value_t   value;   /* source */
    ngx_str_t                  from;
    ngx_str_t                  to;
} ngx_http_iconv_set_t;


typedef struct {
    ngx_str_t                  from;    /* filter: source encoding */
    ngx_str_t                  to;      /* filter: target encoding */
    size_t                     buffer_size;
} ngx_http_iconv_loc_conf_t;


typedef struct {
    iconv_t                    cd;
    ngx_chain_t               *free;
    ngx_chain_t               *busy;
    ngx_chain_t               *out;
    ngx_chain_t              **last_out;
    ngx_chain_t               *last;    /* the link the flags belong on */
    u_char                     carry[NGX_HTTP_ICONV_CARRY_MAX];
    size_t                     carry_len;
    unsigned                   done:1;
} ngx_http_iconv_ctx_t;


static ngx_int_t ngx_http_iconv_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_iconv_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_iconv_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_iconv_feed(ngx_http_request_t *r,
    ngx_http_iconv_ctx_t *ctx, ngx_http_iconv_loc_conf_t *ilcf,
    u_char *data, size_t len);
static ngx_chain_t *ngx_http_iconv_append(ngx_http_request_t *r,
    ngx_http_iconv_ctx_t *ctx);
static void ngx_http_iconv_cleanup(void *data);

static char *ngx_http_iconv_set(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_iconv_filter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_iconv_arg(ngx_conf_t *cf, ngx_str_t *arg,
    ngx_str_t *from, ngx_str_t *to);
static ngx_int_t ngx_http_iconv_check(ngx_conf_t *cf, ngx_str_t *from,
    ngx_str_t *to);

static void *ngx_http_iconv_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_iconv_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_iconv_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_iconv_commands[] = {

    { ngx_string("set_iconv"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE4,
      ngx_http_iconv_set,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("iconv_filter"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_iconv_filter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("iconv_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_iconv_loc_conf_t, buffer_size),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_iconv_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_iconv_init,                   /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_iconv_create_loc_conf,        /* create location configuration */
    ngx_http_iconv_merge_loc_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_iconv_module = {
    NGX_MODULE_V1,
    &ngx_http_iconv_module_ctx,            /* module context */
    ngx_http_iconv_commands,               /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/* ------------------------------------------------------------- set_iconv */

/*
 * The conversion happens when the variable is read, not in a phase handler.
 * A handler in the rewrite phase would run after ngx_http_rewrite_module's
 * own, so a "return 200 $dst" in the same location would answer before the
 * value existed -- which is exactly what the first version of this module
 * got wrong.  Reading it lazily also costs nothing when nobody asks.
 */
static ngx_int_t
ngx_http_iconv_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_iconv_set_t *set = (ngx_http_iconv_set_t *) data;

    size_t      inleft, outleft, outlen;
    u_char     *out, *inp, *outp;
    iconv_t     cd;
    ngx_str_t   src;

    if (ngx_http_complex_value(r, &set->value, &src) != NGX_OK) {
        return NGX_ERROR;
    }

    cd = iconv_open((char *) set->to.data, (char *) set->from.data);

    if (cd == (iconv_t) -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "iconv_open(\"%V\", \"%V\") failed",
                      &set->to, &set->from);
        return NGX_ERROR;
    }

    /*
     * Four bytes of output per input byte is the worst case across the
     * encodings iconv(3) offers, and it spares a resize loop for the short
     * strings this directive deals with.
     */
    outlen = src.len * 4 + 1;

    out = ngx_pnalloc(r->pool, outlen);
    if (out == NULL) {
        iconv_close(cd);
        return NGX_ERROR;
    }

    inp = src.data;
    inleft = src.len;
    outp = out;
    outleft = outlen;

    if (iconv(cd, (char **) &inp, &inleft, (char **) &outp, &outleft)
        == (size_t) -1)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "iconv() failed converting \"%V\" from %V to %V",
                      &src, &set->from, &set->to);
        iconv_close(cd);
        return NGX_ERROR;
    }

    iconv_close(cd);

    v->len = outlen - outleft;
    v->data = out;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


/* ---------------------------------------------------------- iconv_filter */

static ngx_int_t
ngx_http_iconv_header_filter(ngx_http_request_t *r)
{
    ngx_pool_cleanup_t         *cln;
    ngx_http_iconv_ctx_t       *ctx;
    ngx_http_iconv_loc_conf_t  *ilcf;

    ilcf = ngx_http_get_module_loc_conf(r, ngx_http_iconv_module);

    if (ilcf->from.len == 0
        || r->header_only
        || r->headers_out.content_length_n == 0)
    {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_iconv_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->cd = iconv_open((char *) ilcf->to.data, (char *) ilcf->from.data);

    if (ctx->cd == (iconv_t) -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "iconv_open(\"%V\", \"%V\") failed",
                      &ilcf->to, &ilcf->from);
        return NGX_ERROR;
    }

    /*
     * iconv_close() belongs to the pool rather than to the end of the body
     * filter: a request aborted mid-stream never reaches the last buffer,
     * and a leaked descriptor per request is the kind of thing that only
     * shows up weeks later.
     */
    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        iconv_close(ctx->cd);
        return NGX_ERROR;
    }

    cln->handler = ngx_http_iconv_cleanup;
    cln->data = ctx;

    ctx->last_out = &ctx->out;

    ngx_http_set_ctx(r, ctx, ngx_http_iconv_module);

    /*
     * The converted body has a different length, and neither a range of it
     * nor an entity tag of the source still describes what we send.
     */
    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_clear_etag(r);

    r->filter_need_in_memory = 1;

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_iconv_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                   rc;
    ngx_buf_t                  *b;
    ngx_chain_t                *cl, *nl, *out;
    ngx_http_iconv_ctx_t       *ctx;
    ngx_http_iconv_loc_conf_t  *ilcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_iconv_module);

    if (ctx == NULL || ctx->done) {
        return ngx_http_next_body_filter(r, in);
    }

    ilcf = ngx_http_get_module_loc_conf(r, ngx_http_iconv_module);

    for (cl = in; cl; cl = cl->next) {

        b = cl->buf;

        if (ngx_buf_size(b) > 0) {
            rc = ngx_http_iconv_feed(r, ctx, ilcf, b->pos, b->last - b->pos);
            if (rc != NGX_OK) {
                return NGX_ERROR;
            }

            b->pos = b->last;
        }

        if (b->last_buf || b->flush || b->sync) {

            if (b->last_buf && ctx->carry_len) {
                /*
                 * Bytes left over at the end of the stream are an incomplete
                 * character: the body was cut short or is not in the encoding
                 * the configuration claims.  Dropping them is the only honest
                 * choice, the headers went out long ago.
                 */
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "iconv dropped %uz trailing byte(s), the body "
                              "ends in an incomplete character",
                              ctx->carry_len);
                ctx->carry_len = 0;
            }

            /*
             * The flags need a buffer to sit on.  When the conversion
             * produced nothing -- an empty response, or a flush between two
             * characters -- an empty one is appended, otherwise a client
             * waiting on a server-sent event sits there until the response
             * ends, and last_buf would never arrive at all.
             */
            if (ctx->last == NULL) {
                nl = ngx_http_iconv_append(r, ctx);
                if (nl == NULL) {
                    return NGX_ERROR;
                }

                nl->buf->sync = 1;
            }

            ctx->last->buf->last_buf = b->last_buf;
            ctx->last->buf->flush = b->flush;

            if (b->sync) {
                ctx->last->buf->sync = 1;
            }

            if (b->last_buf) {
                ctx->done = 1;
            }
        }
    }

    if (ctx->out == NULL) {
        return NGX_OK;
    }

    out = ctx->out;
    ctx->out = NULL;
    ctx->last_out = &ctx->out;
    ctx->last = NULL;

    rc = ngx_http_next_body_filter(r, out);

    ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &out,
                            (ngx_buf_tag_t) &ngx_http_iconv_module);

    return rc;
}


/*
 * Convert one buffer's worth of input, prepending whatever the previous
 * buffer left unfinished.  The three errno cases iconv(3) raises are the
 * whole difficulty of this module:
 *
 *   E2BIG   the output buffer is full; emit it and continue with a fresh one
 *   EINVAL  the input ends inside a character; keep the tail for next time
 *   EILSEQ  the input is not what the configuration says it is
 */
static ngx_int_t
ngx_http_iconv_feed(ngx_http_request_t *r, ngx_http_iconv_ctx_t *ctx,
    ngx_http_iconv_loc_conf_t *ilcf, u_char *data, size_t len)
{
    size_t        inleft, outleft, n;
    u_char       *inp, *outp, *in;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (ctx->carry_len) {
        /*
         * Joining the carried tail with the new buffer costs one copy of at
         * most NGX_HTTP_ICONV_CARRY_MAX bytes plus the buffer itself.  The
         * alternative, feeding the tail separately, would stall whenever a
         * character straddles the boundary, which is exactly the case this
         * code exists for.
         */
        in = ngx_pnalloc(r->pool, ctx->carry_len + len);
        if (in == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(in, ctx->carry, ctx->carry_len);
        ngx_memcpy(in + ctx->carry_len, data, len);

        inleft = ctx->carry_len + len;
        ctx->carry_len = 0;

    } else {
        in = data;
        inleft = len;
    }

    inp = in;

    while (inleft) {

        cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        b = cl->buf;
        b->tag = (ngx_buf_tag_t) &ngx_http_iconv_module;
        b->temporary = 1;
        b->memory = 0;
        b->flush = 0;
        b->last_buf = 0;
        b->sync = 0;

        if (b->start == NULL || (size_t) (b->end - b->start)
                                < ilcf->buffer_size)
        {
            b->start = ngx_palloc(r->pool, ilcf->buffer_size);
            if (b->start == NULL) {
                return NGX_ERROR;
            }

            b->end = b->start + ilcf->buffer_size;
        }

        b->pos = b->start;
        b->last = b->start;

        outp = b->start;
        outleft = b->end - b->start;

        n = iconv(ctx->cd, (char **) &inp, &inleft, (char **) &outp, &outleft);

        b->last = outp;

        if (n == (size_t) -1) {

            switch (ngx_errno) {

            case E2BIG:
                /* buffer full, the rest goes into the next one */
                break;

            case EINVAL:
                if (inleft > NGX_HTTP_ICONV_CARRY_MAX) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                  "iconv left %uz byte(s) unconverted, more "
                                  "than the %d this module carries over",
                                  inleft, NGX_HTTP_ICONV_CARRY_MAX);
                    return NGX_ERROR;
                }

                ngx_memcpy(ctx->carry, inp, inleft);
                ctx->carry_len = inleft;
                inleft = 0;
                break;

            case EILSEQ:
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "iconv hit a byte sequence that is not valid "
                              "%V, %uz byte(s) into the buffer",
                              &ilcf->from, (size_t) (inp - in));
                return NGX_ERROR;

            default:
                ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                              "iconv() failed");
                return NGX_ERROR;
            }
        }

        if (b->last > b->pos) {
            *ctx->last_out = cl;
            ctx->last_out = &cl->next;
            ctx->last = cl;
            cl->next = NULL;

        } else {
            /*
             * Nothing came out of this round: the whole piece was the start
             * of a character and went into the carry.  A buffer with no
             * bytes and no flags is what nginx reports as "zero size buf in
             * writer", so it goes back on the free list instead of into the
             * chain.  Only a body arriving in small pieces from the network
             * gets here, which is why ci/hostile.sh exists.
             */
            cl->next = ctx->free;
            ctx->free = cl;
        }
    }

    return NGX_OK;
}


/*
 * Take a buffer from the free list and put it at the end of the outgoing
 * chain.  ctx->last keeps pointing at the link itself, not at the slot
 * behind it, because last_buf and flush have to be set on a buffer that
 * exists.
 */
static ngx_chain_t *
ngx_http_iconv_append(ngx_http_request_t *r, ngx_http_iconv_ctx_t *ctx)
{
    ngx_chain_t  *cl;

    cl = ngx_chain_get_free_buf(r->pool, &ctx->free);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf->tag = (ngx_buf_tag_t) &ngx_http_iconv_module;
    cl->buf->flush = 0;
    cl->buf->last_buf = 0;
    cl->buf->sync = 0;

    /*
     * This one carries flags, not bytes.  ngx_buf_special() only counts a
     * buffer as special when it is not in memory, and a buffer taken off
     * the free list still has temporary set from its last use -- leave it
     * and nginx logs every one of them as "zero size buf in writer".
     */
    cl->buf->temporary = 0;
    cl->buf->memory = 0;
    cl->buf->mmap = 0;
    cl->buf->in_file = 0;
    cl->buf->pos = NULL;
    cl->buf->last = NULL;

    *ctx->last_out = cl;
    ctx->last_out = &cl->next;
    ctx->last = cl;
    cl->next = NULL;

    return cl;
}


static void
ngx_http_iconv_cleanup(void *data)
{
    ngx_http_iconv_ctx_t  *ctx = data;

    if (ctx->cd != (iconv_t) -1) {
        iconv_close(ctx->cd);
        ctx->cd = (iconv_t) -1;
    }
}


/* ------------------------------------------------------------ directives */

static char *
ngx_http_iconv_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                          *value, name;
    ngx_http_variable_t                *v;
    ngx_http_iconv_set_t               *set;
    ngx_http_compile_complex_value_t    ccv;

    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid destination \"%V\" in \"set_iconv\", "
                           "a variable name starting with $ is expected",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    name = value[1];
    name.len--;
    name.data++;

    /*
     * Each one stands on its own and is reached through the variable, so
     * there is no array to grow and no pointer that moves when the next
     * set_iconv is parsed.
     */
    set = ngx_pcalloc(cf->pool, sizeof(ngx_http_iconv_set_t));
    if (set == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_iconv_arg(cf, &value[3], &set->from, &set->to) != NGX_OK
        || ngx_http_iconv_arg(cf, &value[4], &set->from, &set->to) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_iconv_check(cf, &set->from, &set->to) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    v = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    v->get_handler = ngx_http_iconv_variable;
    v->data = (uintptr_t) set;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &set->value;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_iconv_filter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_iconv_loc_conf_t *ilcf = conf;

    ngx_str_t  *value;

    if (ilcf->from.len) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (ngx_http_iconv_arg(cf, &value[1], &ilcf->from, &ilcf->to) != NGX_OK
        || ngx_http_iconv_arg(cf, &value[2], &ilcf->from, &ilcf->to) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_iconv_check(cf, &ilcf->from, &ilcf->to) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/*
 * Both directives take their encodings as from=X and to=Y in either order.
 */
static ngx_int_t
ngx_http_iconv_arg(ngx_conf_t *cf, ngx_str_t *arg, ngx_str_t *from,
    ngx_str_t *to)
{
    ngx_str_t  *dst;

    if (ngx_strncmp(arg->data, "from=", 5) == 0) {
        dst = from;
        arg->data += 5;
        arg->len -= 5;

    } else if (ngx_strncmp(arg->data, "to=", 3) == 0) {
        dst = to;
        arg->data += 3;
        arg->len -= 3;

    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\", from= or to= expected",
                           arg);
        return NGX_ERROR;
    }

    if (dst->len) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "parameter \"%V\" is given twice",
                           arg);
        return NGX_ERROR;
    }

    if (arg->len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "empty encoding name");
        return NGX_ERROR;
    }

    /* iconv_open() wants C strings */
    dst->data = ngx_pnalloc(cf->pool, arg->len + 1);
    if (dst->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(dst->data, arg->data, arg->len);
    dst->data[arg->len] = '\0';
    dst->len = arg->len;

    return NGX_OK;
}


/*
 * Reject an unusable pair of encodings while the configuration is read.
 * Finding out at the first request means a 500 for whoever happens to ask
 * first, and a message in the error log nobody is watching.
 */
static ngx_int_t
ngx_http_iconv_check(ngx_conf_t *cf, ngx_str_t *from, ngx_str_t *to)
{
    iconv_t  cd;

    if (from->len == 0 || to->len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "both from= and to= are required");
        return NGX_ERROR;
    }

    cd = iconv_open((char *) to->data, (char *) from->data);

    if (cd == (iconv_t) -1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "iconv cannot convert from \"%V\" to \"%V\"",
                           from, to);
        return NGX_ERROR;
    }

    iconv_close(cd);

    return NGX_OK;
}


/* ----------------------------------------------------------- boilerplate */

static void *
ngx_http_iconv_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_iconv_loc_conf_t  *ilcf;

    ilcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_iconv_loc_conf_t));
    if (ilcf == NULL) {
        return NULL;
    }

    ilcf->buffer_size = NGX_CONF_UNSET_SIZE;

    return ilcf;
}


static char *
ngx_http_iconv_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_iconv_loc_conf_t *prev = parent;
    ngx_http_iconv_loc_conf_t *conf = child;

    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size,
                              (size_t) ngx_pagesize);

    /*
     * A buffer too small to hold a single converted character would make
     * the conversion loop ask for a new one forever, so the floor is not
     * cosmetic.
     */
    if (conf->buffer_size < NGX_HTTP_ICONV_CARRY_MAX) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "iconv_buffer_size must be at least %d bytes",
                           NGX_HTTP_ICONV_CARRY_MAX);
        return NGX_CONF_ERROR;
    }

    if (conf->from.len == 0) {
        conf->from = prev->from;
        conf->to = prev->to;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_iconv_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_iconv_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_iconv_body_filter;

    return NGX_OK;
}
