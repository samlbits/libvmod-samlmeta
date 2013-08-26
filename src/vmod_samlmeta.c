/*
    Copyright (C) 2013 Aivars Kalvans <aivars.kalvans@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published
    by the Free Software Foundation; either version 3 of the License,
    or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, see <http://www.gnu.org/licenses>.

    Additional permission under GNU GPL version 3 section 7

    If you modify this Program, or any covered work, by linking or
    combining it with libvmod-example (or a modified version of that
    library), containing parts covered by the terms of Aivars Kalvans
    the licensors of this Program grant you additional permission to
    convey the resulting work. {Corresponding Source for a non-source
    form of such a combination shall include the source code for the
    parts of libvmod-example used as well as that of the covered work.}
*/

#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <expat.h>
#include <math.h>

#include "vrt.h"
#include "vqueue.h"
#include "vsha256.h"
#include "bin/varnishd/cache.h"
#include "bin/varnishd/stevedore.h"
#include "vcc_if.h"

int init_function(struct vmod_priv *priv, const struct VCL_conf *conf) {
    return 0;
}

struct buf_t {
    char *ptr;
    size_t len;
    size_t size;
};

#define BUF_GROW(buf) do { \
    (buf)->size *= 2; \
    (buf)->ptr = realloc((buf)->ptr, (buf)->size); \
    AN((buf)->ptr); \
} while (0)

#define BUF_RESERVE(buf, n) do { \
    while ((buf)->size <= (buf)->len + (n)) { \
        BUF_GROW(buf); \
    } \
} while (0)

static struct buf_t _object_read(struct sess *sp) {
    struct storage *st;
    size_t len;
    struct buf_t buf = {NULL, 0, 4 * 1024};

    BUF_GROW(&buf);
    CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);

    if (!sp->obj->gziped) {

        VTAILQ_FOREACH(st, &sp->obj->store, list) {
            CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
            CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);

            BUF_RESERVE(&buf, st->len);
            memcpy(buf.ptr + buf.len, st->ptr, st->len);
            buf.len += st->len;
        }
    } else {
	struct vgz *vg;
	char obuf[params->gzip_stack_buffer];
	ssize_t obufl = 0;
	const void *dp;
	size_t dl;

	vg = VGZ_NewUngzip(sp, "U D -");

        VTAILQ_FOREACH(st, &sp->obj->store, list) {
            CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
            CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);

            BUF_RESERVE(&buf, st->len * 2);
	    VGZ_Ibuf(vg, st->ptr, st->len);
            do {
	        VGZ_Obuf(vg, buf.ptr + buf.len, buf.size - buf.len);
                if (buf.len < buf.size) {
                    VGZ_Gunzip(vg, &dp, &dl);
                    buf.len += dl;
                } else {
                    BUF_RESERVE(&buf, st->len);
                }
	    } while (!VGZ_IbufEmpty(vg));
        }
	VGZ_Destroy(&vg);
    }
    BUF_RESERVE(&buf, 1);
    buf.ptr[buf.len] = '\0';
    return buf;
}

typedef struct {
   char *cacheDuration;
   char *validUntil;
   int  tries;
} validity_data_t;

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define PARSELEN 256

static void
begin_element(void *data, const char *element, const char **attr)
{
   validity_data_t *validity = (validity_data_t *)data;
   validity->tries++;
   fprintf(stderr,"begin_element %s\n",element);
   if (strcasecmp(element,"EntityDescriptors") == 0) {
      for (int i = 0; attr[i]; i += 2) {
         if (strcasecmp(attr[i],"cacheDuration") == 0) {
            validity->cacheDuration = attr[i+1];
	    fprintf(stderr,"cd %s\n",attr[i+1]);
         } else if (strcasecmp(attr[i],"validUntil") == 0) {
            validity->validUntil = attr[i+1];
         }
      }
   }
}

static void 
end_element(void *data, const char *element)
{
   fprintf(stderr,"end_element %s\n",element);
}

const char * __match_proto__()
vmod_samlmeta_ttl(struct sess *sp) {
    struct buf_t buf;
    validity_data_t validity;
    char *ttl = NULL,*s = NULL;
    unsigned u = 0,v = 0;
    int ret = 0,tot = 0;

    if (sp->step != STP_PREPRESP) {
        /* Can be called only from vcl_deliver */
        abort();
        return (NULL);
    }

    buf = _object_read(sp);
    fprintf(stderr,"buf: '%s'\n",buf.ptr);

    memset(&validity,0,sizeof(validity));

    XML_Parser parser = XML_ParserCreate("UTF-8");
    XML_SetElementHandler(parser,begin_element,end_element);
    XML_SetUserData(parser,&validity);
    ret = XML_Parse(parser,buf.ptr,min(PARSELEN,buf.len),0);
    if (ret == 0) {
       fprintf(stderr,"XML parsing error (%d): %s\n",ret,XML_ErrorString(XML_GetErrorCode(parser)));
       goto out;
    }

    fprintf(stderr,"cacheDuration=%s\n",validity.cacheDuration);
    if (validity.cacheDuration != NULL) {
        int v = 0;
        char *p = validity.cacheDuration;
        char m;
        int time = 0;
        // PThHmMsS
        fprintf(stderr,"starting duration parse: %s\n",p);
        if (p[0] == 'P' && p[1] == 'T') {
           p += 2;
           fprintf(stderr,"@ %s\n",p);
           while (*p) {
              int ns = sscanf(p,"%d%c",&v,&m);
              fprintf(stderr,"ns: %d\n",ns);
              if (2 != ns) goto out;

              switch (m) {
                 case 'H':
                 case 'h':
                    tot += 3600*v;
                    break;
                 case 'M':
                 case 'm':
                    tot += 60*v;
                    break;
                 case 'S':
                 case 's':
                    tot += v;
                    break;
                 default:
		    fprintf(stderr,"ignoring format at ... %s\n",p);
                    break;
              }
              while (*p && *p >= 48 && *p <= 57) p++; // skip numbers
              p++;
           }
        }
    } else if (validity.validUntil != NULL) {
        tot = 1;
    }

    fprintf(stderr,"tot: %d\n", tot);

    if (tot > 0) {
       int len = (tot == 0 ? 1 : (int)(log10(tot)+1));
       ttl = WS_Alloc(sp->wrk->ws,len+2);
       sprintf(ttl,"%ds",tot);
    }

out:
    XML_ParserFree(parser);
    return ttl;
}
