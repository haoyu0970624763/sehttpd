#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "http.h"

/* constant-time string comparison */
#define cst_strcmp(m, c0, c1, c2, c3) \
    *(uint32_t *) m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define CR '\r'
#define LF '\n'
#define CRLFCRLF "\r\n\r\n"


int http_parse_request_line(http_request_t *r)
{
    uint8_t ch, *p, *m;
    size_t pi;

    /* Use enum to give every state different value
     * s_method=1 , s_spaces_before_uri=2 ... etc
     */
    enum {
        s_start = 0,
        s_method,
        s_spaces_before_uri,
        s_after_slash_in_uri,
        s_http,
        s_http_H,
        s_http_HT,
        s_http_HTT,
        s_http_HTTP,
        s_first_major_digit,
        s_major_digit,
        s_first_minor_digit,
        s_minor_digit,
        s_spaces_after_digit,
        s_almost_done
    } state;

    const void *conditions[] = {&&c_start,
                                &&c_method,
                                &&c_spaces_before_uri,
                                &&c_after_slash_in_uri,
                                &&c_http,
                                &&c_http_H,
                                &&c_http_HT,
                                &&c_http_HTT,
                                &&c_http_HTTP,
                                &&c_first_major_digit,
                                &&c_major_digit,
                                &&c_first_minor_digit,
                                &&c_minor_digit,
                                &&c_spaces_after_digit,
                                &&c_almost_done};

    state = r->state;

    for (pi = r->pos; pi < r->last; pi++) {
        p = (uint8_t *) &r->buf[pi % MAX_BUF];
        ch = *p;
        goto *conditions[state];

        /* HTTP methods: GET, HEAD, POST */
    c_start:
        r->request_start = p;

        if (ch == CR || ch == LF)
            continue;

        if ((ch < 'A' || ch > 'Z') && ch != '_')
            return HTTP_PARSER_INVALID_METHOD;

        state = s_method;
        continue;

    c_method:
        if (ch == ' ') {
            m = r->request_start;

            switch (p - m) {
            case 3:
                if (cst_strcmp(m, 'G', 'E', 'T', ' ')) {
                    r->method = HTTP_GET;
                    break;
                }
                break;

            case 4:
                if (cst_strcmp(m, 'P', 'O', 'S', 'T')) {
                    r->method = HTTP_POST;
                    break;
                }

                if (cst_strcmp(m, 'H', 'E', 'A', 'D')) {
                    r->method = HTTP_HEAD;
                    break;
                }
                break;

            default:
                r->method = HTTP_UNKNOWN;
                break;
            }
            state = s_spaces_before_uri;
            continue;
        }

        if ((ch < 'A' || ch > 'Z') && ch != '_')
            return HTTP_PARSER_INVALID_METHOD;
        continue;

    /* space* before URI */
    c_spaces_before_uri:
        if (ch == '/') {
            r->uri_start = p;
            state = s_after_slash_in_uri;
            continue;
        }
        if (ch != ' ') {
            return HTTP_PARSER_INVALID_REQUEST;
        }
        continue;

    c_after_slash_in_uri:
        if (ch == ' ') {
            r->uri_end = p;
            state = s_http;
        }
        continue;

    /* space+ after URI */
    c_http:
        if (ch == 'H') {
            state = s_http_H;
            continue;
        }
        if (ch != ' ') {
            return HTTP_PARSER_INVALID_REQUEST;
        }
        continue;

    c_http_H:
        if (ch == 'T') {
            state = s_http_HT;
            continue;
        } else {
            return HTTP_PARSER_INVALID_REQUEST;
        }

    c_http_HT:
        if (ch == 'T') {
            state = s_http_HTT;
            continue;
        } else {
            return HTTP_PARSER_INVALID_REQUEST;
        }

    c_http_HTT:
        if (ch == 'P') {
            state = s_http_HTTP;
            continue;
        } else {
            return HTTP_PARSER_INVALID_REQUEST;
        }

    c_http_HTTP:
        if (ch == '/') {
            state = s_first_major_digit;
            continue;
        } else {
            return HTTP_PARSER_INVALID_REQUEST;
        }
    /* first digit of major HTTP version */
    c_first_major_digit:
        if (ch < '1' || ch > '9')
            return HTTP_PARSER_INVALID_REQUEST;

        r->http_major = ch - '0';
        state = s_major_digit;
        continue;

    /* major HTTP version or dot */
    c_major_digit:
        if (ch == '.') {
            state = s_first_minor_digit;
            continue;
        }

        if (ch < '0' || ch > '9')
            return HTTP_PARSER_INVALID_REQUEST;

        r->http_major = r->http_major * 10 + ch - '0';
        continue;

    /* first digit of minor HTTP version */
    c_first_minor_digit:
        if (ch < '0' || ch > '9')
            return HTTP_PARSER_INVALID_REQUEST;

        r->http_minor = ch - '0';
        state = s_minor_digit;
        continue;

    /* minor HTTP version or end of request line */
    c_minor_digit:
        if (ch == CR) {
            state = s_almost_done;
            continue;
        }

        if (ch == LF)
            goto done;

        if (ch == ' ') {
            state = s_spaces_after_digit;
            continue;
        }

        if (ch < '0' || ch > '9')
            return HTTP_PARSER_INVALID_REQUEST;

        r->http_minor = r->http_minor * 10 + ch - '0';
        continue;

    c_spaces_after_digit:
        switch (ch) {
        case ' ':
            break;
        case CR:
            state = s_almost_done;
            break;
        case LF:
            goto done;
        default:
            return HTTP_PARSER_INVALID_REQUEST;
        }
        continue;

    /* end of request line */
    c_almost_done:
        r->request_end = p - 1;
        if (ch == LF) {
            goto done;
        } else {
            return HTTP_PARSER_INVALID_REQUEST;
        }
    }

    r->pos = pi;
    r->state = state;

    return EAGAIN;

done:
    r->pos = pi + 1;

    if (!r->request_end)
        r->request_end = p;

    r->state = s_start;

    return 0;
}



int http_parse_request_body(http_request_t *r)
{
    uint8_t ch, *p;
    size_t pi;

    enum {
        s_start = 0,
        s_key,
        s_spaces_before_colon,
        s_spaces_after_colon,
        s_value,
        s_cr,
        s_crlf,
        s_crlfcr
    } state;

    const void *conditions[] = {
        &&c_start,
        &&c_key,
        &&c_spaces_before_colon,
        &&c_spaces_after_colon,
        &&c_value,
        &&c_cr,
        &&c_crlf,
        &&c_crlfcr,
    };

    state = r->state;
    assert(state == 0 && "state should be 0");

    http_header_t *hd;

    for (pi = r->pos; pi < r->last; pi++) {
        p = (uint8_t *) &r->buf[pi % MAX_BUF];
        ch = *p;
        goto *conditions[state];

    c_start:
        if (ch == CR || ch == LF)
            continue;

        r->cur_header_key_start = p;
        state = s_key;
        continue;

    c_key:
        if (ch == ' ') {
            r->cur_header_key_end = p;
            state = s_spaces_before_colon;
            continue;
        }

        if (ch == ':') {
            r->cur_header_key_end = p;
            state = s_spaces_after_colon;
            continue;
        }
        continue;

    c_spaces_before_colon:
        if (ch == ' ')
            continue;
        if (ch == ':') {
            state = s_spaces_after_colon;
            continue;
        }
        return HTTP_PARSER_INVALID_HEADER;

    c_spaces_after_colon:
        if (ch == ' ')
            continue;

        state = s_value;
        r->cur_header_value_start = p;
        continue;

    c_value:
        if (ch == CR) {
            r->cur_header_value_end = p;
            state = s_cr;
        }

        if (ch == LF) {
            r->cur_header_value_end = p;
            state = s_crlf;
        }
        continue;

    c_cr:
        if (ch == LF) {
            state = s_crlf;
            /* save the current HTTP header */
            hd = malloc(sizeof(http_header_t));
            hd->key_start = r->cur_header_key_start;
            hd->key_end = r->cur_header_key_end;
            hd->value_start = r->cur_header_value_start;
            hd->value_end = r->cur_header_value_end;

            list_add(&(hd->list), &(r->list));
            continue;
        }
        return HTTP_PARSER_INVALID_HEADER;

    c_crlf:
        if (ch == CR) {
            state = s_crlfcr;
        } else {
            r->cur_header_key_start = p;
            state = s_key;
        }
        continue;

    c_crlfcr:
        switch (ch) {
        case LF:
            goto done;
        default:
            return HTTP_PARSER_INVALID_HEADER;
        }
        continue;
    }

    r->pos = pi;
    r->state = state;

    return EAGAIN;

done:
    r->pos = pi + 1;
    r->state = s_start;

    return 0;
}
