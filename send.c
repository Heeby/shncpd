/*
Copyright (c) 2015 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <assert.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "shncpd.h"
#include "trickle.h"
#include "state.h"
#include "send.h"
#include "prefix.h"
#include "dhcpv4.h"
#include "util.h"

#define SENDBUF_SIZE 4000
static unsigned char *sendbuf = NULL;
static int buffered = 0;
/* At most one of those can be set. */
static struct interface *buffered_interface = NULL;
static struct sockaddr_in6 buffered_sin6;

int
flushbuf()
{
    struct sockaddr_in6 tob, *to;
    int rc;

    if(!buffered)
        return 0;

    if(buffered_interface) {
        memset(&tob, 0, sizeof(tob));
        tob.sin6_family = AF_INET6;
        memcpy(&tob.sin6_addr, &protocol_group, 16);
        tob.sin6_port = htons(protocol_port);
        tob.sin6_scope_id = buffered_interface->ifindex;
        to = &tob;
    } else {
        to = &buffered_sin6;
    }

    rc = sendto(protocol_socket, sendbuf, buffered, 0,
                (struct sockaddr*)to, sizeof(struct sockaddr_in6));

    if(rc < 0) {
        int saved_errno = errno;
        perror("sendto");
        errno = saved_errno;
    }

    buffered_interface = NULL;
    buffered = 0;
    MEM_UNDEFINED(sendbuf, SENDBUF_SIZE);
    MEM_UNDEFINED(&buffered_sin6, sizeof(buffered_sin6));

    return rc;
}

int
buffer_tlv(int type, const unsigned char *data, int datalen,
           const struct sockaddr_in6 *sin6, struct interface *interface)
{
    if(interface && sin6) {
        errno = EINVAL;
        return -1;
    }

    if(sendbuf == NULL)
        sendbuf = allocate_buffer(SENDBUF_SIZE);
    if(sendbuf == NULL)
        return -1;

    if(buffered &&
       ((interface && interface != buffered_interface) ||
        (!interface && (buffered_interface ||
                        memcmp(sin6, &buffered_sin6, sizeof(*sin6)) != 0)) ||
        buffered + datalen > 1400))
        flushbuf();

    if(buffered + datalen + 4 > SENDBUF_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    if(!buffered) {
        /* NODE-ENDPOINT */
        DO_HTONS(sendbuf + 0, 3);
        DO_HTONS(sendbuf + 2, 8);
        memcpy(sendbuf + 4, myid, 4);
        DO_HTONL(sendbuf + 8,
                 interface ? interface->ifindex : sin6->sin6_scope_id);
        buffered = 12;
    }

    if(interface)
        buffered_interface = interface;
    else
        memcpy(&buffered_sin6, sin6, sizeof(*sin6));

    DO_HTONS(sendbuf + buffered, type); buffered += 2;
    DO_HTONS(sendbuf + buffered, datalen); buffered += 2;
    memcpy(sendbuf + buffered, data, datalen); buffered += datalen;

    if(debug_level >= 3)
        debugf("Buffering %s %d (%d)\n",
               interface ? "multicast" : "unicast", type, datalen);

    return 1;
}

int
buffer_network_state(const struct sockaddr_in6 *sin6,
                     struct interface *interface)
{
    unsigned char h[8];
    int rc;
    rc = network_hash(h);
    if(rc < 0)
        return -1;
    debugf("-> NETWORK-STATE %s%s\n",
           format_64(h), interface ? " (multicast)" : "");
    buffer_tlv(4, h, 8, sin6, interface);
    return 1;
}

#define CHECK(_n) if(buflen < i + (_n)) goto fail
#define BYTE(_v) buf[i] = (_v); i++
#define BYTES(_v, _len) memcpy(buf + i, (_v), (_len)); i += (_len)
#define SHORT(_v) DO_HTONS(buf + i, (_v)); i += 2
#define LONG(_v) DO_HTONL(buf + i, (_v)); i += 4
#define PAD() while((i & 3) != 0) buf[i++] = 0

int
buffer_node_state(struct node *node, int full,
                  const struct sockaddr_in6 *sin6, struct interface *interface)
{
    int buflen = full ? 20 + node->datalen : 20;
    unsigned char buf[buflen];
    int i = 0;
    long long orig_delay =
        (now.tv_sec - node->orig_time.tv_sec) * 1000LL +
        ((long long)now.tv_nsec - node->orig_time.tv_nsec + 999999) / 1000000;

    BYTES(node->id, 4);
    LONG(node->seqno);
    LONG((unsigned)orig_delay);
    BYTES(node->datahash, 8);
    if(full) {
        BYTES(node->data, node->datalen);
    }
    assert(i == buflen);
    debugf("-> NODE-STATE %s%s\n",
           format_32(node->id),
           full && interface ? " (full, multicast)" :
           full ? " (full)" :
           interface ? " (multicast)" : "");
    return buffer_tlv(5, buf, buflen, sin6, interface);
}

int
format_my_state(unsigned char *buf, int buflen)
{
    int i = 0, j, k;
    struct node *node = find_node(myid, 0);
    int dlen, n_dns6, dns6_len, n_dns4, dns4_len;

    MEM_UNDEFINED(buf, buflen);

    for(j = 0; j < numneighs; j++) {
        CHECK(12);
        SHORT(8);
        SHORT(12);
        BYTES(neighs[j].id, 4);
        LONG(neighs[j].eid);
        LONG(neighs[j].interface->ifindex);
        PAD();
    }

    CHECK(11);
    SHORT(32);
    SHORT(12);
    SHORT(0);
    BYTE(0);
    BYTE(interface_dhcpv4_prio(neighs[j].interface) & 0x0F);

    BYTES("SHNCPD/0", 8);
    PAD();

    for(j = 0; j < numinterfaces; j++) {
        for(k = 0; k < interfaces[j].numassigned; k++) {
            const struct assigned_prefix *aa = &interfaces[j].assigned[k];
            int pbytes = (aa->assigned.plen + 7) / 8;
            if(aa->published) {
                assert(aa->assigned.plen > 0);
                CHECK(10 + pbytes);
                SHORT(35);
                SHORT(6 + pbytes);
                LONG(interfaces[j].ifindex);
                BYTE((aa->assigned.prio & 0x0F));
                BYTE(aa->assigned.plen);
                BYTES(&aa->assigned.p, pbytes);
                PAD();
            }
            if(!IN6_IS_ADDR_UNSPECIFIED(&aa->assigned_address)) {
                CHECK(24);
                SHORT(36);
                SHORT(20);
                LONG(interfaces[j].ifindex);
                BYTES(&aa->assigned_address, 16);
                PAD();
            }
        }
    }

    dlen = n_dns6 = n_dns4 = 0;
    for(j = 0; j < node->numexts; j++) {
        if(node->exts[j]->delegated) {
            for(k = 0; k < node->exts[j]->delegated->numprefixes; k++) {
                struct prefix *p = &node->exts[j]->delegated->prefixes[k];
                dlen += 4 + 9 + (p->plen + 7) / 8;
            }
        }
        if(node->exts[j]->dns) {
            for(k = 0; k < node->exts[j]->dns->numprefixes; k++) {
                struct prefix *p = &node->exts[j]->dns->prefixes[k];
                if(!prefix_v4(p))
                    n_dns6++;
                else
                    n_dns4++;
            }
        }
    }

    dns6_len = n_dns6 > 0 ? 4 + 4 + 16 * n_dns6 : 0;
    dns4_len = n_dns4 > 0 ? 4 + 2 + 4 * n_dns4 : 0;

    for(j = 0; j < node->numexts; j++) {
        CHECK(4);
        SHORT(33);
        SHORT(dlen + (-dlen & 3) +
              dns6_len + (-dns6_len & 3) +
              dns4_len + (-dns4_len & 3));
        if(node->exts[j]->delegated) {
            for(k = 0; k < node->exts[j]->delegated->numprefixes; k++) {
                struct prefix *p = &node->exts[j]->delegated->prefixes[k];
                CHECK(4 + 9 + (p->plen + 7) / 8 + 4);
                SHORT(34);
                SHORT(9 + (p->plen + 7) / 8);
                LONG(3600);
                LONG(1800);
                BYTE(p->plen);
                BYTES(&p->p, (p->plen + 7) / 8);
                PAD();
            }
        }
        if(n_dns6 > 0) {
            CHECK(4 + 4 + 16 * n_dns6 + 4);
            SHORT(37);
            SHORT(4 + 16 * n_dns6);
            SHORT(23);
            SHORT(16 * n_dns6);
            for(k = 0; k < node->exts[j]->dns->numprefixes; k++) {
                struct prefix *p = &node->exts[j]->dns->prefixes[k];
                if(!prefix_v4(p)) {
                    BYTES(&p->p, 16);
                }
            }
            PAD();
        }
        if(n_dns4 > 0) {
            CHECK(4 + 2 + 4 * n_dns4 + 4);
            SHORT(38);
            SHORT(2 + 4 * n_dns4);
            BYTE(6);
            BYTE(4 * n_dns4);
            for(k = 0; k < node->exts[j]->dns->numprefixes; k++) {
                struct prefix *p = &node->exts[j]->dns->prefixes[k];
                if(prefix_v4(p)) {
                    BYTES((char*)&p->p + 12, 4);
                }
            }
            PAD();
        }
    }

    return i;

 fail:
    return -1;
}

#undef CHECK
#undef BYTE
#undef BYTES
#undef SHORT
#undef LONG
#undef PAD
