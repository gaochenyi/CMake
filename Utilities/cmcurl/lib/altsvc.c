/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
/*
 * The Alt-Svc: header is defined in RFC 7838:
 * https://datatracker.ietf.org/doc/html/rfc7838
 */
#include "curl_setup.h"

#if !defined(CURL_DISABLE_HTTP) && !defined(CURL_DISABLE_ALTSVC)
#include <curl/curl.h>
#include "urldata.h"
#include "altsvc.h"
#include "curl_get_line.h"
#include "strcase.h"
#include "parsedate.h"
#include "sendf.h"
#include "warnless.h"
#include "fopen.h"
#include "rename.h"
#include "strdup.h"
#include "inet_pton.h"
#include "strparse.h"
#include "connect.h"

/* The last 3 #include files should be in this order */
#include "curl_printf.h"
#include "curl_memory.h"
#include "memdebug.h"

#define MAX_ALTSVC_LINE 4095
#define MAX_ALTSVC_DATELEN 256
#define MAX_ALTSVC_HOSTLEN 2048
#define MAX_ALTSVC_ALPNLEN 10

#define H3VERSION "h3"

/* Given the ALPN ID, return the name */
const char *Curl_alpnid2str(enum alpnid id)
{
  switch(id) {
  case ALPN_h1:
    return "h1";
  case ALPN_h2:
    return "h2";
  case ALPN_h3:
    return H3VERSION;
  default:
    return ""; /* bad */
  }
}


static void altsvc_free(struct altsvc *as)
{
  free(as->src.host);
  free(as->dst.host);
  free(as);
}

static struct altsvc *altsvc_createid(const char *srchost,
                                      size_t hlen,
                                      const char *dsthost,
                                      size_t dlen, /* dsthost length */
                                      enum alpnid srcalpnid,
                                      enum alpnid dstalpnid,
                                      size_t srcport,
                                      size_t dstport)
{
  struct altsvc *as = calloc(1, sizeof(struct altsvc));
  if(!as)
    return NULL;
  DEBUGASSERT(hlen);
  DEBUGASSERT(dlen);
  if(!hlen || !dlen)
    /* bad input */
    goto error;
  if((hlen > 2) && srchost[0] == '[') {
    /* IPv6 address, strip off brackets */
    srchost++;
    hlen -= 2;
  }
  else if(srchost[hlen - 1] == '.') {
    /* strip off trailing dot */
    hlen--;
    if(!hlen)
      goto error;
  }
  if((dlen > 2) && dsthost[0] == '[') {
    /* IPv6 address, strip off brackets */
    dsthost++;
    dlen -= 2;
  }

  as->src.host = Curl_memdup0(srchost, hlen);
  if(!as->src.host)
    goto error;

  as->dst.host = Curl_memdup0(dsthost, dlen);
  if(!as->dst.host)
    goto error;

  as->src.alpnid = srcalpnid;
  as->dst.alpnid = dstalpnid;
  as->src.port = (unsigned short)srcport;
  as->dst.port = (unsigned short)dstport;

  return as;
error:
  altsvc_free(as);
  return NULL;
}

static struct altsvc *altsvc_create(struct Curl_str *srchost,
                                    struct Curl_str *dsthost,
                                    struct Curl_str *srcalpn,
                                    struct Curl_str *dstalpn,
                                    size_t srcport,
                                    size_t dstport)
{
  enum alpnid dstalpnid = Curl_alpn2alpnid(dstalpn->str, dstalpn->len);
  enum alpnid srcalpnid = Curl_alpn2alpnid(srcalpn->str, srcalpn->len);
  if(!srcalpnid || !dstalpnid)
    return NULL;
  return altsvc_createid(srchost->str, srchost->len,
                         dsthost->str, dsthost->len,
                         srcalpnid, dstalpnid,
                         srcport, dstport);
}

/* only returns SERIOUS errors */
static CURLcode altsvc_add(struct altsvcinfo *asi, char *line)
{
  /* Example line:
     h2 example.com 443 h3 shiny.example.com 8443 "20191231 10:00:00" 1
   */
  struct Curl_str srchost;
  struct Curl_str dsthost;
  struct Curl_str srcalpn;
  struct Curl_str dstalpn;
  struct Curl_str date;
  size_t srcport;
  size_t dstport;
  size_t persist;
  size_t prio;

  if(Curl_str_word(&line, &srcalpn, MAX_ALTSVC_ALPNLEN) ||
     Curl_str_singlespace(&line) ||
     Curl_str_word(&line, &srchost, MAX_ALTSVC_HOSTLEN) ||
     Curl_str_singlespace(&line) ||
     Curl_str_number(&line, &srcport, 65535) ||
     Curl_str_singlespace(&line) ||
     Curl_str_word(&line, &dstalpn, MAX_ALTSVC_ALPNLEN) ||
     Curl_str_singlespace(&line) ||
     Curl_str_word(&line, &dsthost, MAX_ALTSVC_HOSTLEN) ||
     Curl_str_singlespace(&line) ||
     Curl_str_number(&line, &dstport, 65535) ||
     Curl_str_singlespace(&line) ||
     Curl_str_quotedword(&line, &date, MAX_ALTSVC_DATELEN) ||
     Curl_str_singlespace(&line) ||
     Curl_str_number(&line, &persist, 1) ||
     Curl_str_singlespace(&line) ||
     Curl_str_number(&line, &prio, 0) ||
     Curl_str_newline(&line))
    ;
  else {
    struct altsvc *as;
    char dbuf[MAX_ALTSVC_DATELEN + 1];
    time_t expires;

    /* The date parser works on a null terminated string. The maximum length
       is upheld by Curl_str_quotedword(). */
    memcpy(dbuf, date.str, date.len);
    dbuf[date.len] = 0;
    expires = Curl_getdate_capped(dbuf);
    as = altsvc_create(&srchost, &dsthost, &srcalpn, &dstalpn, srcport,
                       dstport);
    if(as) {
      as->expires = expires;
      as->prio = 0; /* not supported to just set zero */
      as->persist = persist ? 1 : 0;
      Curl_llist_append(&asi->list, as, &as->node);
    }
  }

  return CURLE_OK;
}

/*
 * Load alt-svc entries from the given file. The text based line-oriented file
 * format is documented here: https://curl.se/docs/alt-svc.html
 *
 * This function only returns error on major problems that prevent alt-svc
 * handling to work completely. It will ignore individual syntactical errors
 * etc.
 */
static CURLcode altsvc_load(struct altsvcinfo *asi, const char *file)
{
  CURLcode result = CURLE_OK;
  FILE *fp;

  /* we need a private copy of the filename so that the altsvc cache file
     name survives an easy handle reset */
  free(asi->filename);
  asi->filename = strdup(file);
  if(!asi->filename)
    return CURLE_OUT_OF_MEMORY;

  fp = fopen(file, FOPEN_READTEXT);
  if(fp) {
    struct dynbuf buf;
    Curl_dyn_init(&buf, MAX_ALTSVC_LINE);
    while(Curl_get_line(&buf, fp)) {
      char *lineptr = Curl_dyn_ptr(&buf);
      while(*lineptr && ISBLANK(*lineptr))
        lineptr++;
      if(*lineptr == '#')
        /* skip commented lines */
        continue;

      altsvc_add(asi, lineptr);
    }
    Curl_dyn_free(&buf); /* free the line buffer */
    fclose(fp);
  }
  return result;
}

/*
 * Write this single altsvc entry to a single output line
 */

static CURLcode altsvc_out(struct altsvc *as, FILE *fp)
{
  struct tm stamp;
  const char *dst6_pre = "";
  const char *dst6_post = "";
  const char *src6_pre = "";
  const char *src6_post = "";
  CURLcode result = Curl_gmtime(as->expires, &stamp);
  if(result)
    return result;
#ifdef USE_IPV6
  else {
    char ipv6_unused[16];
    if(1 == Curl_inet_pton(AF_INET6, as->dst.host, ipv6_unused)) {
      dst6_pre = "[";
      dst6_post = "]";
    }
    if(1 == Curl_inet_pton(AF_INET6, as->src.host, ipv6_unused)) {
      src6_pre = "[";
      src6_post = "]";
    }
  }
#endif
  fprintf(fp,
          "%s %s%s%s %u "
          "%s %s%s%s %u "
          "\"%d%02d%02d "
          "%02d:%02d:%02d\" "
          "%u %u\n",
          Curl_alpnid2str(as->src.alpnid),
          src6_pre, as->src.host, src6_post,
          as->src.port,

          Curl_alpnid2str(as->dst.alpnid),
          dst6_pre, as->dst.host, dst6_post,
          as->dst.port,

          stamp.tm_year + 1900, stamp.tm_mon + 1, stamp.tm_mday,
          stamp.tm_hour, stamp.tm_min, stamp.tm_sec,
          as->persist, as->prio);
  return CURLE_OK;
}

/* ---- library-wide functions below ---- */

/*
 * Curl_altsvc_init() creates a new altsvc cache.
 * It returns the new instance or NULL if something goes wrong.
 */
struct altsvcinfo *Curl_altsvc_init(void)
{
  struct altsvcinfo *asi = calloc(1, sizeof(struct altsvcinfo));
  if(!asi)
    return NULL;
  Curl_llist_init(&asi->list, NULL);

  /* set default behavior */
  asi->flags = CURLALTSVC_H1
#ifdef USE_HTTP2
    | CURLALTSVC_H2
#endif
#ifdef USE_HTTP3
    | CURLALTSVC_H3
#endif
    ;
  return asi;
}

/*
 * Curl_altsvc_load() loads alt-svc from file.
 */
CURLcode Curl_altsvc_load(struct altsvcinfo *asi, const char *file)
{
  DEBUGASSERT(asi);
  return altsvc_load(asi, file);
}

/*
 * Curl_altsvc_ctrl() passes on the external bitmask.
 */
CURLcode Curl_altsvc_ctrl(struct altsvcinfo *asi, const long ctrl)
{
  DEBUGASSERT(asi);
  asi->flags = ctrl;
  return CURLE_OK;
}

/*
 * Curl_altsvc_cleanup() frees an altsvc cache instance and all associated
 * resources.
 */
void Curl_altsvc_cleanup(struct altsvcinfo **altsvcp)
{
  if(*altsvcp) {
    struct Curl_llist_node *e;
    struct Curl_llist_node *n;
    struct altsvcinfo *altsvc = *altsvcp;
    for(e = Curl_llist_head(&altsvc->list); e; e = n) {
      struct altsvc *as = Curl_node_elem(e);
      n = Curl_node_next(e);
      altsvc_free(as);
    }
    free(altsvc->filename);
    free(altsvc);
    *altsvcp = NULL; /* clear the pointer */
  }
}

/*
 * Curl_altsvc_save() writes the altsvc cache to a file.
 */
CURLcode Curl_altsvc_save(struct Curl_easy *data,
                          struct altsvcinfo *altsvc, const char *file)
{
  CURLcode result = CURLE_OK;
  FILE *out;
  char *tempstore = NULL;

  if(!altsvc)
    /* no cache activated */
    return CURLE_OK;

  /* if not new name is given, use the one we stored from the load */
  if(!file && altsvc->filename)
    file = altsvc->filename;

  if((altsvc->flags & CURLALTSVC_READONLYFILE) || !file || !file[0])
    /* marked as read-only, no file or zero length filename */
    return CURLE_OK;

  result = Curl_fopen(data, file, &out, &tempstore);
  if(!result) {
    struct Curl_llist_node *e;
    struct Curl_llist_node *n;
    fputs("# Your alt-svc cache. https://curl.se/docs/alt-svc.html\n"
          "# This file was generated by libcurl! Edit at your own risk.\n",
          out);
    for(e = Curl_llist_head(&altsvc->list); e; e = n) {
      struct altsvc *as = Curl_node_elem(e);
      n = Curl_node_next(e);
      result = altsvc_out(as, out);
      if(result)
        break;
    }
    fclose(out);
    if(!result && tempstore && Curl_rename(tempstore, file))
      result = CURLE_WRITE_ERROR;

    if(result && tempstore)
      unlink(tempstore);
  }
  free(tempstore);
  return result;
}

static CURLcode getalnum(const char **ptr, char *alpnbuf, size_t buflen)
{
  size_t len;
  const char *protop;
  const char *p = *ptr;
  while(*p && ISBLANK(*p))
    p++;
  protop = p;
  while(*p && !ISBLANK(*p) && (*p != ';') && (*p != '='))
    p++;
  len = p - protop;
  *ptr = p;

  if(!len || (len >= buflen))
    return CURLE_BAD_FUNCTION_ARGUMENT;
  memcpy(alpnbuf, protop, len);
  alpnbuf[len] = 0;
  return CURLE_OK;
}

/* hostcompare() returns true if 'host' matches 'check'. The first host
 * argument may have a trailing dot present that will be ignored.
 */
static bool hostcompare(const char *host, const char *check)
{
  size_t hlen = strlen(host);
  size_t clen = strlen(check);

  if(hlen && (host[hlen - 1] == '.'))
    hlen--;
  if(hlen != clen)
    /* they cannot match if they have different lengths */
    return FALSE;
  return strncasecompare(host, check, hlen);
}

/* altsvc_flush() removes all alternatives for this source origin from the
   list */
static void altsvc_flush(struct altsvcinfo *asi, enum alpnid srcalpnid,
                         const char *srchost, unsigned short srcport)
{
  struct Curl_llist_node *e;
  struct Curl_llist_node *n;
  for(e = Curl_llist_head(&asi->list); e; e = n) {
    struct altsvc *as = Curl_node_elem(e);
    n = Curl_node_next(e);
    if((srcalpnid == as->src.alpnid) &&
       (srcport == as->src.port) &&
       hostcompare(srchost, as->src.host)) {
      Curl_node_remove(e);
      altsvc_free(as);
    }
  }
}

#ifdef DEBUGBUILD
/* to play well with debug builds, we can *set* a fixed time this will
   return */
static time_t altsvc_debugtime(void *unused)
{
  char *timestr = getenv("CURL_TIME");
  (void)unused;
  if(timestr) {
    long val = strtol(timestr, NULL, 10);
    return (time_t)val;
  }
  return time(NULL);
}
#undef time
#define time(x) altsvc_debugtime(x)
#endif

/*
 * Curl_altsvc_parse() takes an incoming alt-svc response header and stores
 * the data correctly in the cache.
 *
 * 'value' points to the header *value*. That is contents to the right of the
 * header name.
 *
 * Currently this function rejects invalid data without returning an error.
 * Invalid hostname, port number will result in the specific alternative
 * being rejected. Unknown protocols are skipped.
 */
CURLcode Curl_altsvc_parse(struct Curl_easy *data,
                           struct altsvcinfo *asi, const char *value,
                           enum alpnid srcalpnid, const char *srchost,
                           unsigned short srcport)
{
  const char *p = value;
  char alpnbuf[MAX_ALTSVC_ALPNLEN] = "";
  struct altsvc *as;
  unsigned short dstport = srcport; /* the same by default */
  CURLcode result = getalnum(&p, alpnbuf, sizeof(alpnbuf));
  size_t entries = 0;
  size_t alpnlen = strlen(alpnbuf);
  size_t srchostlen = strlen(srchost);
#ifdef CURL_DISABLE_VERBOSE_STRINGS
  (void)data;
#endif
  if(result) {
    infof(data, "Excessive alt-svc header, ignoring.");
    return CURLE_OK;
  }

  DEBUGASSERT(asi);

  /* "clear" is a magic keyword */
  if(strcasecompare(alpnbuf, "clear")) {
    /* Flush cached alternatives for this source origin */
    altsvc_flush(asi, srcalpnid, srchost, srcport);
    return CURLE_OK;
  }

  do {
    if(*p == '=') {
      /* [protocol]="[host][:port]" */
      enum alpnid dstalpnid = Curl_alpn2alpnid(alpnbuf, alpnlen);
      p++;
      if(*p == '\"') {
        const char *dsthost = "";
        size_t dstlen = 0; /* destination hostname length */
        const char *value_ptr;
        char option[32];
        unsigned long num;
        char *end_ptr;
        bool quoted = FALSE;
        time_t maxage = 24 * 3600; /* default is 24 hours */
        bool persist = FALSE;
        bool valid = TRUE;
        p++;
        if(*p != ':') {
          /* hostname starts here */
          const char *hostp = p;
          if(*p == '[') {
            /* pass all valid IPv6 letters - does not handle zone id */
            dstlen = strspn(++p, "0123456789abcdefABCDEF:.");
            if(p[dstlen] != ']')
              /* invalid host syntax, bail out */
              break;
            /* we store the IPv6 numerical address *with* brackets */
            dstlen += 2;
            p = &p[dstlen-1];
          }
          else {
            while(*p && (ISALNUM(*p) || (*p == '.') || (*p == '-')))
              p++;
            dstlen = p - hostp;
          }
          if(!dstlen || (dstlen >= MAX_ALTSVC_HOSTLEN)) {
            infof(data, "Excessive alt-svc hostname, ignoring.");
            valid = FALSE;
          }
          else {
            dsthost = hostp;
          }
        }
        else {
          /* no destination name, use source host */
          dsthost = srchost;
          dstlen = strlen(srchost);
        }
        if(*p == ':') {
          unsigned long port = 0;
          p++;
          if(ISDIGIT(*p))
            /* a port number */
            port = strtoul(p, &end_ptr, 10);
          else
            end_ptr = (char *)p; /* not left uninitialized */
          if(!port || port > USHRT_MAX || end_ptr == p || *end_ptr != '\"') {
            infof(data, "Unknown alt-svc port number, ignoring.");
            valid = FALSE;
          }
          else {
            dstport = curlx_ultous(port);
            p = end_ptr;
          }
        }
        if(*p++ != '\"')
          break;
        /* Handle the optional 'ma' and 'persist' flags. Unknown flags
           are skipped. */
        for(;;) {
          while(ISBLANK(*p))
            p++;
          if(*p != ';')
            break;
          p++; /* pass the semicolon */
          if(!*p || ISNEWLINE(*p))
            break;
          result = getalnum(&p, option, sizeof(option));
          if(result) {
            /* skip option if name is too long */
            option[0] = '\0';
          }
          while(*p && ISBLANK(*p))
            p++;
          if(*p != '=')
            return CURLE_OK;
          p++;
          while(*p && ISBLANK(*p))
            p++;
          if(!*p)
            return CURLE_OK;
          if(*p == '\"') {
            /* quoted value */
            p++;
            quoted = TRUE;
          }
          value_ptr = p;
          if(quoted) {
            while(*p && *p != '\"')
              p++;
            if(!*p++)
              return CURLE_OK;
          }
          else {
            while(*p && !ISBLANK(*p) && *p!= ';' && *p != ',')
              p++;
          }
          num = strtoul(value_ptr, &end_ptr, 10);
          if((end_ptr != value_ptr) && (num < ULONG_MAX)) {
            if(strcasecompare("ma", option))
              maxage = (time_t)num;
            else if(strcasecompare("persist", option) && (num == 1))
              persist = TRUE;
          }
        }
        if(dstalpnid && valid) {
          if(!entries++)
            /* Flush cached alternatives for this source origin, if any - when
               this is the first entry of the line. */
            altsvc_flush(asi, srcalpnid, srchost, srcport);

          as = altsvc_createid(srchost, srchostlen,
                               dsthost, dstlen,
                               srcalpnid, dstalpnid,
                               srcport, dstport);
          if(as) {
            time_t secs = time(NULL);
            /* The expires time also needs to take the Age: value (if any)
               into account. [See RFC 7838 section 3.1] */
            if(maxage > (TIME_T_MAX - secs))
              as->expires = TIME_T_MAX;
            else
              as->expires = maxage + secs;
            as->persist = persist;
            Curl_llist_append(&asi->list, as, &as->node);
            infof(data, "Added alt-svc: %s:%d over %s", dsthost, dstport,
                  Curl_alpnid2str(dstalpnid));
          }
        }
      }
      else
        break;
      /* after the double quote there can be a comma if there is another
         string or a semicolon if no more */
      if(*p == ',') {
        /* comma means another alternative is presented */
        p++;
        result = getalnum(&p, alpnbuf, sizeof(alpnbuf));
        if(result)
          break;
      }
    }
    else
      break;
  } while(*p && (*p != ';') && (*p != '\n') && (*p != '\r'));

  return CURLE_OK;
}

/*
 * Return TRUE on a match
 */
bool Curl_altsvc_lookup(struct altsvcinfo *asi,
                        enum alpnid srcalpnid, const char *srchost,
                        int srcport,
                        struct altsvc **dstentry,
                        const int versions) /* one or more bits */
{
  struct Curl_llist_node *e;
  struct Curl_llist_node *n;
  time_t now = time(NULL);
  DEBUGASSERT(asi);
  DEBUGASSERT(srchost);
  DEBUGASSERT(dstentry);

  for(e = Curl_llist_head(&asi->list); e; e = n) {
    struct altsvc *as = Curl_node_elem(e);
    n = Curl_node_next(e);
    if(as->expires < now) {
      /* an expired entry, remove */
      Curl_node_remove(e);
      altsvc_free(as);
      continue;
    }
    if((as->src.alpnid == srcalpnid) &&
       hostcompare(srchost, as->src.host) &&
       (as->src.port == srcport) &&
       (versions & (int)as->dst.alpnid)) {
      /* match */
      *dstentry = as;
      return TRUE;
    }
  }
  return FALSE;
}

#endif /* !CURL_DISABLE_HTTP && !CURL_DISABLE_ALTSVC */
