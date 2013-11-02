/*
 * zling:
 *  light-weight lossless data compression utility.
 *
 * Copyright (C) 2012-2013 by Zhang Li <zhangli10 at baidu.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* git-tag = 20131103
 *
 * reference:
 *  ROLZ:
 *      http://www.ezcodesample.com/rolz/rolz_article.html
 *  POLAR-CODE:
 *      http://www.ezcodesample.com/prefixer/prefixer_article.html
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef _DEBUG /* enable debug? */
#define _DEBUG 0
#endif

#if _DEBUG
#define __DEBUG_LINE__(s) s
#define __DEBUG_INFO__(...) \
    (fprintf(stderr, "__debug: "), \
     fprintf(stderr, __VA_ARGS__), \
     fprintf(stderr, "\n"))

static int count_match;
static int count_literal;

static int count_match_calls;
static int count_match_items;
static int count_memaccess;
static int count_update_match_len;

static void __debug_on_destroy__() __attribute__((__destructor__));
static void __debug_on_destroy__() {
    __DEBUG_INFO__("");
    __DEBUG_INFO__("count_match:            %d", count_match);
    __DEBUG_INFO__("count_literal:          %d", count_literal);
    __DEBUG_INFO__("");
    __DEBUG_INFO__("count_match_calls:      %d", count_match_calls);
    __DEBUG_INFO__("count_match_items:      %d", count_match_items);
    __DEBUG_INFO__("count_memaccess:        %d", count_memaccess);
    __DEBUG_INFO__("count_update_match_len: %d", count_update_match_len);
    return;
}

#else /* _DEBUG */
#define __DEBUG_LINE__(s)
#define __DEBUG_INFO__(...)
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
#include <fcntl.h>
#include <io.h>

int ma_n(int argc, char** argv);
int main(int argc, char** argv) { /* set stdio to binary mode for windows */
    setmode(fileno(stdin),  O_BINARY);
    setmode(fileno(stdout), O_BINARY);
    return ma_n(argc, argv);
}
#define main ma_n
#endif

#define __cache_aligned(name) __attribute__((aligned(64)))(name)

/*******************************************************************************
 * POLAR Coder
 ******************************************************************************/
#define POLAR_SYMBOLS   384 /* should be even */
#define POLAR_MAXLEN    15  /* should be < 16 -- for packing two length values into a byte */

#define m_round_down(x)     while((x)&(-(x)^(x))) { (x) &= (-(x)^(x)); }
#define m_round_up(x)       while((x)&(-(x)^(x))) { (x) &= (-(x)^(x)); } (x) <<= 1;
#define m_int_swap(x, y)    {int (_)=(x); (x)=(y); (y)=(_);}

static int polar_make_leng_table(const unsigned int* freq_table, unsigned int* leng_table) {
    int symbols[POLAR_SYMBOLS];
    int i;
    int s;
    int total;
    int shift = 0;

    memcpy(leng_table, freq_table, POLAR_SYMBOLS * sizeof(int));

MakeTablePass:
    /* sort symbols */
    for (i = 0; i < POLAR_SYMBOLS; i++) {
        symbols[i] = i;
    }
    for (i = 0; i < POLAR_SYMBOLS; i++) { /* simple gnome sort */
        if (i > 0 && leng_table[symbols[i - 1]] < leng_table[symbols[i]]) {
            m_int_swap(symbols[i - 1], symbols[i]);
            i -= 2;
        }
    }

    /* calculate total frequency */
    total = 0;
    for (i = 0; i < POLAR_SYMBOLS; i++) {
        total += leng_table[i];
    }

    /* run */
    m_round_up(total);
    s = 0;
    for (i = 0; i < POLAR_SYMBOLS; i++) {
        m_round_down(leng_table[i]);
        s += leng_table[i];
    }
    while (s < total) {
        for (i = 0; i < POLAR_SYMBOLS; i++) {
            if (s + leng_table[symbols[i]] <= total) {
                s += leng_table[symbols[i]];
                leng_table[symbols[i]] *= 2;
            }
        }
    }

    /* get code length */
    for (i = 0; i < POLAR_SYMBOLS; i++) {
        s = 2;
        if (leng_table[i] > 0) {
            while ((total / leng_table[i]) >> s != 0) {
                s += 1;
            }
            leng_table[i] = s - 1;
        } else {
            leng_table[i] = 0;
        }

        /* code length too long -- scale and rebuild table */
        if (leng_table[i] > POLAR_MAXLEN) {
            shift += 1;
            for (i = 0; i < POLAR_SYMBOLS; i++) {
                if ((leng_table[i] = freq_table[i] >> shift) == 0 && freq_table[i] > 0) {
                    leng_table[i] = 1;
                }
            }
            goto MakeTablePass;
        }
    }
    return 0;
}

static int polar_make_code_table(const unsigned int* leng_table, unsigned int* code_table) {
    int i;
    int s;
    int code = 0;

    memset(code_table, 0, POLAR_SYMBOLS * sizeof(int));

    /* make code for each symbol */
    for (s = 1; s <= POLAR_MAXLEN; s++) {
        for (i = 0; i < POLAR_SYMBOLS; i++) {
            if (leng_table[i] == s) {
                code_table[i] = code;
                code += 1;
            }
        }
        code *= 2;
    }

    /* reverse each code */
    for (i = 0; i < POLAR_SYMBOLS; i++) {
        code_table[i] = ((code_table[i] & 0xff00) >> 8 | (code_table[i] & 0x00ff) << 8);
        code_table[i] = ((code_table[i] & 0xf0f0) >> 4 | (code_table[i] & 0x0f0f) << 4);
        code_table[i] = ((code_table[i] & 0xcccc) >> 2 | (code_table[i] & 0x3333) << 2);
        code_table[i] = ((code_table[i] & 0xaaaa) >> 1 | (code_table[i] & 0x5555) << 1);
        code_table[i] >>= 16 - leng_table[i];
    }
    return 0;
}

static int polar_make_decode_table(
    const unsigned int* leng_table,
    const unsigned int* code_table, unsigned short* decode_table) {
    int i;
    int c;

    memset(decode_table, -1, sizeof(unsigned short) << POLAR_MAXLEN);

    for (c = 0; c < POLAR_SYMBOLS; c++) {
        if (leng_table[c] > 0) {
            for (i = 0; i + code_table[c] < (1 << POLAR_MAXLEN); i += (1 << leng_table[c])) {
                decode_table[i + code_table[c]] = leng_table[c] * POLAR_SYMBOLS + c;
            }
        }
    }
    return 0;
}

/*******************************************************************************
 * ROLZ
 ******************************************************************************/
#define BUCKET_ITEM_SIZE        3600
#define BUCKET_ITEM_HASH        1024
#define MATCH_DISCARD_MINLEN    1300
#define MATCH_MAXTRY            8
#define MATCH_MINLEN            4
#define MATCH_MAXLEN            (MATCH_MINLEN + (POLAR_SYMBOLS - 256) - 1)

struct rolz_bucket_dec_st {
    unsigned int   m_offset[BUCKET_ITEM_SIZE];
    unsigned short m_head;
};
struct rolz_bucket_st {
    unsigned short m_suffix[BUCKET_ITEM_SIZE];
    unsigned int   m_offset[BUCKET_ITEM_SIZE];
    unsigned short m_head;
    unsigned short m_hash[BUCKET_ITEM_HASH];
};

#define m_hash_context(ptr) (((ptr)[0] * 31337 + (ptr)[1] * 3337 + (ptr)[2] * 337 + (ptr)[3]) % BUCKET_ITEM_HASH)
#define m_hash_check(ptr)   (((ptr)[0] * 11337 + (ptr)[1] * 1337 + (ptr)[2]) & 0xff)

static int find_common_length(unsigned char* buf1, unsigned char* buf2, int maxlen) {
    unsigned char* p1 = buf1;
    unsigned char* p2 = buf2;

    while ((maxlen--) > 0 && *p1 == *p2) {
        p1++;
        p2++;
    }
    return p1 - buf1;
}

static int rolz_dec_update(struct rolz_bucket_dec_st* rolz_table, unsigned char* buf, int pos) {
    struct rolz_bucket_dec_st* bucket = &rolz_table[buf[pos - 1]];

    bucket->m_head = bucket->m_head + 1 - (-(bucket->m_head + 1 == BUCKET_ITEM_SIZE) & BUCKET_ITEM_SIZE);
    bucket->m_offset[bucket->m_head] = pos;
    return 0;
}

static int rolz_dec_get_offset(struct rolz_bucket_dec_st* rolz_table, unsigned char* buf, int pos, int idx) {
    struct rolz_bucket_dec_st* bucket = &rolz_table[buf[pos - 1]];
    int head = bucket->m_head;
    int node = head - idx + (-(head < idx) & BUCKET_ITEM_SIZE);
    return bucket->m_offset[node];
}

static int rolz_update(struct rolz_bucket_st* rolz_table, unsigned char* buf, int pos) {
    int hash = m_hash_context(buf + pos);
    struct rolz_bucket_st* bucket = &rolz_table[buf[pos - 1]];

    bucket->m_head = bucket->m_head + 1 - (-(bucket->m_head + 1 == BUCKET_ITEM_SIZE) & BUCKET_ITEM_SIZE);
    bucket->m_suffix[bucket->m_head] = bucket->m_hash[hash];
    bucket->m_offset[bucket->m_head] = pos | m_hash_check(buf + pos) << 24;
    bucket->m_hash[hash] = bucket->m_head;
    return 0;
}

static int rolz_match(struct rolz_bucket_st* rolz_table, unsigned char* buf, int pos, int* match_idx, int* match_len) {
    int maxlen = MATCH_MINLEN - 1;
    int maxidx = 0;
    int hash = m_hash_context(buf + pos);
    int node;
    int i;
    int len;
    struct rolz_bucket_st* bucket = &rolz_table[buf[pos - 1]];

    __DEBUG_LINE__(
        count_match_calls += 1;
    );
    node = bucket->m_hash[hash];

    for (i = 0; i < MATCH_MAXTRY; i++) {
        unsigned offset = bucket->m_offset[node] & 0xffffff;
        unsigned check = bucket->m_offset[node] >> 24;

        __DEBUG_LINE__(
            count_match_items += 1;
        );
        if (check == m_hash_check(buf + pos)) {
            __DEBUG_LINE__(
                count_memaccess += 1;
            );
            if (buf[pos + maxlen] == buf[offset + maxlen]) {
                if ((len = find_common_length(buf + pos, buf + offset, MATCH_MAXLEN)) > maxlen) {
                    maxlen = len;
                    maxidx = bucket->m_head - node + (-(bucket->m_head < node) & BUCKET_ITEM_SIZE);
                    __DEBUG_LINE__(
                        count_update_match_len += 1;
                    );
                    if (maxlen == MATCH_MAXLEN) {
                        break;
                    }
                }
            }
        }
        if (offset <= (bucket->m_offset[bucket->m_suffix[node]] & 0xffffff)) {
            break;
        }
        node = bucket->m_suffix[node];
    }
    if (maxlen >= MATCH_MINLEN + (maxidx >= MATCH_DISCARD_MINLEN)) {
        *match_len = maxlen;
        *match_idx = maxidx;
        return 1;
    }
    return 0;
}

static int rolz_encode(unsigned char* ibuf, unsigned short* obuf, int ilen) {
    int olen = 0;
    int pos = 0;
    int match_idx;
    int match_len;
    struct rolz_bucket_st* rolz_table = calloc(sizeof(*rolz_table), 256);

    /* first byte */
    if (pos < ilen) {
        obuf[olen++] = ibuf[pos];
        pos++;
    }

    while (pos + MATCH_MAXLEN < ilen) {
        if (!rolz_match(rolz_table, ibuf, pos, &match_idx, &match_len)) {
            __DEBUG_LINE__(
                count_literal += 1;
            );
            obuf[olen++] = ibuf[pos]; /* encode as literal */
            rolz_update(rolz_table, ibuf, pos);
            pos += 1;

        } else {
            __DEBUG_LINE__(
                count_match += 1;
            );
            obuf[olen++] = 256 + match_len - MATCH_MINLEN; /* encode as match */
            obuf[olen++] = match_idx;
            rolz_update(rolz_table, ibuf, pos);
            pos += match_len;
        }
    }

    /* rest byte */
    while (pos < ilen) {
        obuf[olen++] = ibuf[pos];
        pos++;
    }
    free(rolz_table);

    return olen;
}

static int rolz_decode(unsigned short* ibuf, unsigned char* obuf, int ilen) {
    int olen = 0;
    int pos = 0;
    int match_idx;
    int match_len;
    int match_offset;
    struct rolz_bucket_dec_st* rolz_table = calloc(sizeof(*rolz_table), 256);

    for (pos = 0; pos < ilen; pos++) {
        if (ibuf[pos] < 256) { /* process a literal byte */
            obuf[olen] = ibuf[pos];
            rolz_dec_update(rolz_table, obuf, olen);
            olen++;

        } else { /* process a match */
            match_len = ibuf[pos++] - 256 + MATCH_MINLEN;
            match_idx = ibuf[pos];
            match_offset = olen - rolz_dec_get_offset(rolz_table, obuf, olen, match_idx);
            rolz_dec_update(rolz_table, obuf, olen);

            /* update context at current pos with rolz-table updating */
            while (match_len > 0) {
                obuf[olen] = obuf[olen - match_offset];
                match_len -= 1;
                olen++;
            }
        }
    }
    free(rolz_table);

    return olen;
}

/*******************************************************************************
 * MAIN
 ******************************************************************************/
static clock_t clock_start;
static clock_t clock_during_rolz = 0;
static clock_t clock_during_polar = 0;

static void print_result(size_t size_src, size_t size_dst, int encode) {
    fprintf(stderr, (encode ?
                     "encode: %u => %u, time=%.3f sec\n" :
                     "decode: %u <= %u, time=%.3f sec\n"),
            size_src,
            size_dst,
            (clock() - clock_start) / (double)CLOCKS_PER_SEC);

    fprintf(stderr, "\ttime_rolz:  %.3f sec\n", clock_during_rolz  / (double)CLOCKS_PER_SEC);
    fprintf(stderr, "\ttime_polar: %.3f sec\n", clock_during_polar / (double)CLOCKS_PER_SEC);
    return;
}

/*******************************************************************************
 * MAIN
 ******************************************************************************/
#define BLOCK_SIZE_IN       16777216
#define BLOCK_SIZE_OUT      18000000

#define MATCHIDX_EXBIT      4 /* (BUCKET_ITEM_SIZE >> MATCHIDX_EXBIT) < POLAR_SYMBOLS */
#define MATCHIDX_EXBIT_MASK 0x0f

int main(int argc, char** argv) {
    static unsigned char  cbuf[BLOCK_SIZE_OUT];
    static unsigned short tbuf[BLOCK_SIZE_IN];
    size_t size_src = 0;
    size_t size_dst = 0;
    int ilen;
    int rlen;
    int olen;
    int rpos;
    int opos;
    int i;
    unsigned int freq_table1[POLAR_SYMBOLS];
    unsigned int freq_table2[POLAR_SYMBOLS];
    unsigned int leng_table1[POLAR_SYMBOLS];
    unsigned int leng_table2[POLAR_SYMBOLS];
    unsigned int code_table1[POLAR_SYMBOLS];
    unsigned int code_table2[POLAR_SYMBOLS];
    unsigned short decode_table1[1 << POLAR_MAXLEN];
    unsigned short decode_table2[1 << POLAR_MAXLEN];
    clock_t checkpoint;

    unsigned long long code_buf;
    int code_len;

    clock_start = clock();

    /* welcome message */
    fprintf(stderr, "zling:\n");
    fprintf(stderr, "   light-weight lossless data compression utility\n");
    fprintf(stderr, "   by Zhang Li <zhangli10 at baidu.com>\n");
    fprintf(stderr, "\n");

    /* zling <e/d> __argv2__ __argv3__ */
    if (argc == 4) {
        if (freopen(argv[2], "rb", stdin) == NULL) {
            fprintf(stderr, "error: cannot open file '%s' for read.\n", argv[2]);
            return -1;
        }
        if (freopen(argv[3], "wb", stdout) == NULL) {
            fprintf(stderr, "error: cannot open file '%s' for write.\n", argv[3]);
            return -1;
        }
        argc = 2;
    }

    /* zling <e/d> __argv2__ (stdout) */
    if (argc == 3) {
        if (freopen(argv[2], "rb", stdin) == NULL) {
            fprintf(stderr, "error: cannot open file '%s' for read.\n", argv[2]);
            return -1;
        }
        argc = 2;
    }

    /* zling <e/d> (stdin) (stdout) */
    if (argc == 2 && strcmp(argv[1], "e") == 0) {
        while ((ilen = fread(cbuf, 1, BLOCK_SIZE_IN, stdin)) > 0) {

            /* rolz-encode */
            checkpoint = clock();
            {
                rlen = rolz_encode(cbuf, tbuf, ilen);
                olen = 0;
            }
            clock_during_rolz += clock() - checkpoint;

            /* polar-encode */
            checkpoint = clock();
            {
                memset(freq_table1, 0, sizeof(freq_table1));
                memset(freq_table2, 0, sizeof(freq_table2));
                code_buf = 0;
                code_len = 0;

                for (i = 0; i < rlen; i++) {
                    freq_table1[tbuf[i]] += 1;

                    if (tbuf[i] >= 256) {
                        i++;
                        freq_table2[tbuf[i] >> MATCHIDX_EXBIT] += 1;
                    }
                }
                polar_make_leng_table(freq_table1, leng_table1);
                polar_make_leng_table(freq_table2, leng_table2);
                polar_make_code_table(leng_table1, code_table1);
                polar_make_code_table(leng_table2, code_table2);

                /* write length table */
                for (i = 0; i < POLAR_SYMBOLS; i += 2) {
                    cbuf[olen++] = leng_table1[i] * 16 + leng_table1[i + 1];
                }
                for (i = 0; i < POLAR_SYMBOLS; i += 2) {
                    cbuf[olen++] = leng_table2[i] * 16 + leng_table2[i + 1];
                }

                /* encode */
                for (i = 0; i < rlen; i++) {
                    code_buf += (unsigned long long)code_table1[tbuf[i]] << code_len;
                    code_len += leng_table1[tbuf[i]];

                    if (tbuf[i] >= 256) {
                        i++;
                        code_buf += (unsigned long long)code_table2[tbuf[i] >> MATCHIDX_EXBIT] << code_len;
                        code_len += leng_table2[tbuf[i] >> MATCHIDX_EXBIT];

                        code_buf += (unsigned long long)(tbuf[i] & MATCHIDX_EXBIT_MASK) << code_len;
                        code_len += MATCHIDX_EXBIT;
                    }
                    while (code_len >= 8) {
                        cbuf[olen++] = code_buf % 256;
                        code_buf /= 256;
                        code_len -= 8;
                    }
                }

                while (code_len > 0) {
                    cbuf[olen++] = code_buf % 256;
                    code_buf /= 256;
                    code_len -= 8;
                }
            }
            clock_during_polar += clock() - checkpoint;

            /* output */
            fputc(rlen % 256, stdout);
            fputc(olen % 256, stdout);
            fputc(rlen / 256 % 256, stdout);
            fputc(olen / 256 % 256, stdout);
            fputc(rlen / 256 / 256 % 256, stdout);
            fputc(olen / 256 / 256 % 256, stdout);
            fputc(rlen / 256 / 256 / 256 % 256, stdout);
            fputc(olen / 256 / 256 / 256 % 256, stdout);
            fwrite(cbuf, 1, olen, stdout);

            size_src += ilen;
            size_dst += olen + sizeof(rlen) + sizeof(olen);
        }
        print_result(size_src, size_dst, 1);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "d") == 0) {
        while (
            rlen  = fgetc(stdin),
            olen  = fgetc(stdin),
            rlen += fgetc(stdin) * 256,
            olen += fgetc(stdin) * 256,
            rlen += fgetc(stdin) * 65536,
            olen += fgetc(stdin) * 65536,
            rlen += fgetc(stdin) * 16777216,
            olen += fgetc(stdin) * 16777216,
            !  feof(stdin) &&
            !ferror(stdin) &&
            olen < BLOCK_SIZE_OUT && fread(cbuf, 1, olen, stdin) == olen) {

            rpos = 0;
            opos = 0;
            code_buf = 0;
            code_len = 0;

            /* polar-decode */
            checkpoint = clock();
            {
                /* read length table */
                for (i = 0; i < POLAR_SYMBOLS; i += 2) {
                    leng_table1[i] =     cbuf[opos] / 16;
                    leng_table1[i + 1] = cbuf[opos] % 16;
                    opos++;
                }
                for (i = 0; i < POLAR_SYMBOLS; i += 2) {
                    leng_table2[i] =     cbuf[opos] / 16;
                    leng_table2[i + 1] = cbuf[opos] % 16;
                    opos++;
                }

                /* decode */
                polar_make_code_table(leng_table1, code_table1);
                polar_make_code_table(leng_table2, code_table2);
                polar_make_decode_table(leng_table1, code_table1, decode_table1);
                polar_make_decode_table(leng_table2, code_table2, decode_table2);

                while (rpos < rlen) {
                    while (opos < olen && code_len < 56) {
                        code_buf += (unsigned long long)cbuf[opos++] << code_len;
                        code_len += 8;
                    }
                    i = decode_table1[code_buf % (1 << POLAR_MAXLEN)];
                    code_len  -= i / POLAR_SYMBOLS;
                    code_buf >>= i / POLAR_SYMBOLS;
                    tbuf[rpos++] = i % POLAR_SYMBOLS;

                    if (tbuf[rpos - 1] >= 256) {
                        i = decode_table2[code_buf % (1 << POLAR_MAXLEN)];
                        code_len  -= i / POLAR_SYMBOLS;
                        code_buf >>= i / POLAR_SYMBOLS;
                        tbuf[rpos++] = i % POLAR_SYMBOLS;

                        tbuf[rpos - 1] <<= MATCHIDX_EXBIT;
                        tbuf[rpos - 1] |= code_buf & MATCHIDX_EXBIT_MASK;
                        code_len  -= MATCHIDX_EXBIT;
                        code_buf >>= MATCHIDX_EXBIT;
                    }
                }
            }
            clock_during_polar += clock() - checkpoint;

            /* rolz-decode */
            checkpoint = clock();
            {
                ilen = rolz_decode(tbuf, cbuf, rlen);
            }
            clock_during_rolz += clock() - checkpoint;

            /* output */
            fwrite(cbuf, 1, ilen, stdout);
            size_src += ilen;
            size_dst += olen + sizeof(rlen) + sizeof(olen);
        }
        print_result(size_src, size_dst, 0);
        return 0;
    }

    /* help message */
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "   zling e source target\n");
    fprintf(stderr, "   zling d source target\n");
    fprintf(stderr, "    * source: default to stdin\n");
    fprintf(stderr, "    * target: default to stdout\n");
    return -1;
}
