/*
 * ccnd.c
 *  
 * Copyright 2008 Palo Alto Research Center, Inc. All rights reserved.
 * $Id$
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <ccn/ccn.h>
#include <ccn/ccnd.h>
#include <ccn/charbuf.h>
#include <ccn/bloom.h>
#include <ccn/hashtb.h>
#include <ccn/schedule.h>
#include <ccn/uri.h>

#include "ccnd_private.h"

static void cleanup_at_exit(void);
static void unlink_at_exit(const char *path);
static int create_local_listener(const char *sockname, int backlog);
static void accept_new_client(struct ccnd *h);
static void shutdown_client_fd(struct ccnd *h, int fd);
static void process_input_message(struct ccnd *h, struct face *face,
                                  unsigned char *msg, size_t size, int pdu_ok);
static void process_input(struct ccnd *h, int fd);
static void do_write(struct ccnd *h, struct face *face,
                     unsigned char *data, size_t size);
static void do_deferred_write(struct ccnd *h, int fd);
static void run(struct ccnd *h);
static void clean_needed(struct ccnd *h);
static int get_signature_offset(struct ccn_parsed_ContentObject *pco,
                         const unsigned char *msg);
static struct face *get_dgram_source(struct ccnd *h, struct face *face,
                              struct sockaddr *addr, socklen_t addrlen);
static void content_skiplist_insert(struct ccnd *h, struct content_entry *content);
static void content_skiplist_remove(struct ccnd *h, struct content_entry *content);
static ccn_accession_t content_skiplist_next(struct ccnd *h, struct content_entry *content);
static const char *unlink_this_at_exit = NULL;
static void
cleanup_at_exit(void)
{
    if (unlink_this_at_exit != NULL) {
        unlink(unlink_this_at_exit);
        unlink_this_at_exit = NULL;
    }
}

static void
handle_fatal_signal(int sig)
{
    cleanup_at_exit();
    _exit(sig);
}

static void
unlink_at_exit(const char *path)
{
    if (unlink_this_at_exit == NULL) {
        unlink_this_at_exit = path;
        signal(SIGTERM, &handle_fatal_signal);
        signal(SIGINT, &handle_fatal_signal);
        signal(SIGHUP, &handle_fatal_signal);
        atexit(&cleanup_at_exit);
    }
}

static int
comm_file_ok(void)
{
    struct stat statbuf;
    int res;
    if (unlink_this_at_exit == NULL)
        return(1);
    res = stat(unlink_this_at_exit, &statbuf);
    if (res == -1)
        return(0);
    return(1);
}

static void
fatal_err(const char *msg)
{
    perror(msg);
    exit(1);
}

static struct ccn_charbuf *
charbuf_obtain(struct ccnd *h)
{
    struct ccn_charbuf *c = h->scratch_charbuf;
    if (c == NULL)
        return(ccn_charbuf_create());
    h->scratch_charbuf = NULL;
    c->length = 0;
    return(c);
}

static void
charbuf_release(struct ccnd *h, struct ccn_charbuf *c)
{
    c->length = 0;
    if (h->scratch_charbuf == NULL)
        h->scratch_charbuf = c;
    else
        ccn_charbuf_destroy(&c);
}

static struct ccn_indexbuf *
indexbuf_obtain(struct ccnd *h)
{
    struct ccn_indexbuf *c = h->scratch_indexbuf;
    if (c == NULL)
        return(ccn_indexbuf_create());
    h->scratch_indexbuf = NULL;
    c->n = 0;
    return(c);
}

static void
indexbuf_release(struct ccnd *h, struct ccn_indexbuf *c)
{
    c->n = 0;
    if (h->scratch_indexbuf == NULL)
        h->scratch_indexbuf = c;
    else
        ccn_indexbuf_destroy(&c);
}

static struct face *
face_from_faceid(struct ccnd *h, unsigned faceid)
{
    unsigned slot = faceid & MAXFACES;
    struct face *face = NULL;
    if (slot < h->face_limit) {
        face = h->faces_by_faceid[slot];
        if (face != NULL && face->faceid != faceid)
            face = NULL;
    }
    return(face);
}

static int
enroll_face(struct ccnd *h, struct face *face)
{
    unsigned i;
    unsigned n = h->face_limit;
    struct face **a = h->faces_by_faceid;
    for (i = h->face_rover; i < n; i++)
        if (a[i] == NULL) goto use_i;
    for (i = 0; i < n; i++)
        if (a[i] == NULL) {
            /* bump gen only if second pass succeeds */
            h->face_gen += MAXFACES + 1;
            goto use_i;
        }
    i = (n + 1) * 3 / 2;
    if (i > MAXFACES) i = MAXFACES;
    if (i <= n)
        return(-1); /* overflow */
    a = realloc(a, i * sizeof(struct face *));
    if (a == NULL)
        return(-1); /* ENOMEM */
    h->face_limit = i;
    while (--i > n)
        a[i] = NULL;
    h->faces_by_faceid = a;
use_i:
    a[i] = face;
    h->face_rover = i + 1;
    face->faceid = i | h->face_gen;
    return (face->faceid);
}

static void
finalize_face(struct hashtb_enumerator *e)
{
    struct ccnd *h = hashtb_get_param(e->ht, NULL);
    struct face *face = e->data;
    unsigned i = face->faceid & MAXFACES;
    if (i < h->face_limit && h->faces_by_faceid[i] == face) {
        h->faces_by_faceid[i] = NULL;
        ccnd_msg(h, "releasing face id %u (slot %u)",
            face->faceid, face->faceid & MAXFACES);
        /* If face->addr is not NULL, it is our key so don't free it. */
        ccn_charbuf_destroy(&face->inbuf);
        ccn_charbuf_destroy(&face->outbuf);
    }
    else
        ccnd_msg(h, "orphaned face %u", face->faceid);
}

static struct content_entry *
content_from_accession(struct ccnd *h, ccn_accession_t accession)
{
    struct content_entry *ans = NULL;
    if (accession >= h->accession_base &&
        accession < h->accession_base + h->content_by_accession_window) {
        ans = h->content_by_accession[accession - h->accession_base];
        if (ans != NULL && ans->accession != accession)
            ans = NULL;
    }
    return(ans);
}

static void
enroll_content(struct ccnd *h, struct content_entry *content) // XXX - neworder
{
    unsigned new_window;
    struct content_entry **new_array;
    struct content_entry **old_array = h->content_by_accession;
    unsigned i = 0;
    unsigned j = 0;
    if (content->accession >= h->accession_base + h->content_by_accession_window) {
        new_window = ((h->content_by_accession_window + 20) * 3 / 2);
        new_array = calloc(new_window, sizeof(new_array[0]));
        if (new_array == NULL)
            return;
        while (i < h->content_by_accession_window && old_array[i] == NULL)
            i++;
        h->accession_base += i;
        h->content_by_accession = new_array;
        while (i < h->content_by_accession_window)
            new_array[j++] = old_array[i++];
        h->content_by_accession_window = new_window;
	free(old_array);
    }
    h->content_by_accession[content->accession - h->accession_base] = content;
}

static void
finalize_content(struct hashtb_enumerator *e) // XXX - neworder
{
    struct ccnd *h = hashtb_get_param(e->ht, NULL);
    struct content_entry *entry = e->data;
    unsigned i = entry->accession - h->accession_base;
    if (i < h->content_by_accession_window && h->content_by_accession[i] == entry) {
        content_skiplist_remove(h, entry);
        if (entry->sender != NULL) {
            ccn_schedule_cancel(h->sched, entry->sender);
            entry->sender = NULL;
        }
        h->content_by_accession[i] = NULL;
        if (entry->comps != NULL) {
            free(entry->comps);
            entry->comps = NULL;
        }
        ccn_indexbuf_destroy(&entry->faces);
    }
    else
        ccnd_msg(h, "orphaned content %u", i);
}

static int
content_skiplist_findbefore(struct ccnd *h, const unsigned char *key, size_t keysize, struct ccn_indexbuf **ans)
{
    int i;
    int n = h->skiplinks->n;
    struct ccn_indexbuf *c;
    struct content_entry *content;
    int order;
    
    c = h->skiplinks;
    for (i = n - 1; i >= 0; i--) {
        for (;;) {
            if (c->buf[i] == 0)
                break;
            content = content_from_accession(h, c->buf[i]);
            if (content == NULL)
                abort();
            order = ccn_compare_names(content->key, content->key_size, key, keysize);
            if (order >= 0)
                break;
            if (content->skiplinks == NULL || i >= content->skiplinks->n)
                abort();
            c = content->skiplinks;
        }
        ans[i] = c;
    }
    return(n);
}

#define CCN_SKIPLIST_MAX_DEPTH 30
static void
content_skiplist_insert(struct ccnd *h, struct content_entry *content)
{
    int d;
    int i;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    if (content->skiplinks != NULL) abort();
    for (d = 1; d < CCN_SKIPLIST_MAX_DEPTH - 1; d++)
        if ((nrand48(h->seed) & 3) != 0) break;
    while (h->skiplinks->n < d)
        ccn_indexbuf_append_element(h->skiplinks, 0);
    i = content_skiplist_findbefore(h, content->key, content->key_size, pred);
    if (i < d)
        d = i; /* just in case */
    content->skiplinks = ccn_indexbuf_create();
    for (i = 0; i < d; i++) {
        ccn_indexbuf_append_element(content->skiplinks, pred[i]->buf[i]);
        pred[i]->buf[i] = content->accession;
    }
}

static void
content_skiplist_remove(struct ccnd *h, struct content_entry *content)
{
    int i;
    int d;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    if (content->skiplinks == NULL) abort();
    d = content_skiplist_findbefore(h, content->key, content->key_size, pred);
    if (d > content->skiplinks->n)
        d = content->skiplinks->n;
    for (i = 0; i < d; i++) {
        pred[i]->buf[i] = content->skiplinks->buf[i];
    }
    ccn_indexbuf_destroy(&content->skiplinks);
}


static struct content_entry *
find_first_match_candidate(struct ccnd *h,
    const unsigned char *interest_msg,
    const struct ccn_parsed_interest *pi)
{
    int d;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    size_t size = pi->offset[CCN_PI_E_Name];
#if 0
    if ((pi->offset[CCN_PI_E_ComponentLast] - 
         pi->offset[CCN_PI_B_ComponentLast]) == 1 + 2 + 32 + 1) {
        /*
         * Last component may be a content digest, so don't
         * consider it when finding the first candidate match.
         */
        size = pi->offset[CCN_PI_B_ComponentLast];
    };
#endif
    d = content_skiplist_findbefore(h, interest_msg, size, pred);
    if (d == 0)
        return(NULL);
    return(content_from_accession(h, pred[0]->buf[0]));
}

static int
content_matches_interest_prefix(struct ccnd *h,
                                struct content_entry *content,
                                const unsigned char *interest_msg,
                                struct ccn_indexbuf *comps,
                                int prefix_comps)
{
    size_t prefixlen;
    if (prefix_comps < 0 || prefix_comps >= comps->n)
        abort();
    /* First verify the prefix match. */
    if (content->ncomps < prefix_comps + 1) {
        if (content->ncomps == prefix_comps &&
            prefix_comps > 0 &&
            (comps->buf[prefix_comps] - comps->buf[prefix_comps - 1] ==
                1 + 2 + 32 + 1)) {
            /* This could be a digest component - strip it */
            prefix_comps -= 1;
        }
        else
            return(0);
    }
    prefixlen = comps->buf[prefix_comps] - comps->buf[0];
    if (content->comps[prefix_comps] - content->comps[0] != prefixlen)
        return(0);
    if (0 != memcmp(content->key + content->comps[0],
                    interest_msg + comps->buf[0],
                    prefixlen))
        return(0);
    return(1);
}

static int
content_matches_interest_qualifiers(struct ccnd *h,
                                    struct content_entry *content,
                                    const unsigned char *interest_msg,
                                    struct ccn_parsed_interest *pi,
                                    struct ccn_indexbuf *comps)
{
    int ans;
    ans = ccn_content_matches_interest(content->key,
                                       content->key_size + content->tail_size,
                                       0, // XXX
                                       NULL,
                                       interest_msg,
                                       pi->offset[CCN_PI_E],
                                       pi);
    return(ans);
}

static ccn_accession_t
content_skiplist_next(struct ccnd *h, struct content_entry *content)
{
    if (content == NULL || content->skiplinks == NULL || content->skiplinks->n < 1)
        return(0);
    return(content->skiplinks->buf[0]);
}

static void
finished_propagating(struct propagating_entry *pe)
{
    if (pe->interest_msg != NULL) {
        free(pe->interest_msg);
        pe->interest_msg = NULL;
    }
    if (pe->next != NULL) {
        pe->next->prev = pe->prev;
        pe->prev->next = pe->next;
        pe->next = pe->prev = NULL;
    }
    ccn_indexbuf_destroy(&pe->outbound);
}

static void
finalize_interest(struct hashtb_enumerator *e)
{
    struct interestprefix_entry *entry = e->data;
    ccn_indexbuf_destroy(&entry->interested_faceid);
    ccn_indexbuf_destroy(&entry->counters);
    if (entry->propagating_head != NULL) {
        finished_propagating(entry->propagating_head);
        free(entry->propagating_head);
        entry->propagating_head = NULL;
    }
}

static void
link_propagating_interest_to_interest_entry(struct ccnd *h,
    struct propagating_entry *pe, struct interestprefix_entry *ipe)
{
    struct propagating_entry *head = ipe->propagating_head;
    if (head == NULL) {
        head = calloc(1, sizeof(*head));
        head->next = head;
        head->prev = head;
        ipe->propagating_head = head;
    }
    pe->next = head;
    pe->prev = head->prev;
    pe->prev->next = pe->next->prev = pe;
}

static void
finalize_propagating(struct hashtb_enumerator *e)
{
    finished_propagating(e->data);
}

static int
create_local_listener(const char *sockname, int backlog)
{
    int res;
    int sock;
    struct sockaddr_un a = { 0 };
    res = unlink(sockname);
    if (res == 0) {
        ccnd_msg(NULL, "unlinked old %s, please wait", sockname);
        sleep(9); /* give old ccnd a chance to exit */
    }
    if (!(res == 0 || errno == ENOENT))
        ccnd_msg(NULL, "failed to unlink %s", sockname);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sockname, sizeof(a.sun_path));
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
        return(sock);
    res = bind(sock, (struct sockaddr *)&a, sizeof(a));
    if (res == -1) {
        close(sock);
        return(-1);
    }
    unlink_at_exit(sockname);
    res = listen(sock, backlog);
    if (res == -1) {
        close(sock);
        return(-1);
    }
    return(sock);
}

static void
accept_new_client(struct ccnd *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct sockaddr who;
    socklen_t wholen = sizeof(who);
    int fd;
    int res;
    struct face *face;
    fd = accept(h->local_listener_fd, &who, &wholen);
    if (fd == -1) {
        perror("accept");
        return;
    }
    res = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (res == -1)
        perror("fcntl");
    hashtb_start(h->faces_by_fd, e);
    if (hashtb_seek(e, &fd, sizeof(fd), 0) != HT_NEW_ENTRY)
        fatal_err("ccnd: accept_new_client");
    face = e->data;
    face->fd = fd;
    res = enroll_face(h, face);
    hashtb_end(e);
    ccnd_msg(h, "accepted client fd=%d id=%d", fd, res);
}

static void
shutdown_client_fd(struct ccnd *h, int fd)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct face *face;
    hashtb_start(h->faces_by_fd, e);
    if (hashtb_seek(e, &fd, sizeof(fd), 0) != HT_OLD_ENTRY)
        fatal_err("ccnd: shutdown_client_fd");
    face = e->data;
    if (face->fd != fd) abort();
    close(fd);
    face->fd = -1;
    ccnd_msg(h, "shutdown client fd=%d id=%d", fd, (int)face->faceid);
    ccn_charbuf_destroy(&face->inbuf);
    ccn_charbuf_destroy(&face->outbuf);
    hashtb_delete(e);
    hashtb_end(e);
}

static void
send_content(struct ccnd *h, struct face *face, struct content_entry *content)
{
    struct ccn_charbuf *c = charbuf_obtain(h);
    if ((face->flags & CCN_FACE_LINK) != 0)
        ccn_charbuf_append_tt(c, CCN_DTAG_CCNProtocolDataUnit, CCN_DTAG);
    ccn_charbuf_append(c, content->key, content->key_size + content->tail_size);
    /* stuff interest here */
    if ((face->flags & CCN_FACE_LINK) != 0)
        ccn_charbuf_append_closer(c);
    do_write(h, face, c->buf, c->length);
    h->content_items_sent += 1;
    charbuf_release(h, c);
}

#define CCN_DATA_PAUSE (16U*1024U)

static int
choose_content_delay(struct ccnd *h, unsigned faceid, int content_flags)
{
    struct face *face = face_from_faceid(h, faceid);
    int shift = (content_flags & CCN_CONTENT_ENTRY_SLOWSEND) ? 2 : 0;
    if (face == NULL)
        return(1); /* Going nowhere, get it over with */
    if ((face->flags & CCN_FACE_DGRAM) != 0)
        return(100); /* localhost udp, delay just a little */
    if ((face->flags & CCN_FACE_LINK) != 0) /* udplink or such, delay more */
        return(((nrand48(h->seed) % CCN_DATA_PAUSE) + CCN_DATA_PAUSE/2) << shift);
    return(10); /* local stream, answer quickly */
}

static int
content_sender(struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    struct ccnd *h = clienth;
    struct content_entry *content = ev->evdata;
    (void)sched;
    
    if (content == NULL ||
        content != content_from_accession(h, content->accession)) {
        ccnd_msg(h, "ccn.c:%d bogon", __LINE__);
        return(0);
    }
    if ((flags & CCN_SCHEDULE_CANCEL) != 0 || content->faces == NULL) {
        content->sender = NULL;
        return(0);
    }
    while (content->nface_done < content->faces->n) {
        unsigned faceid = content->faces->buf[content->nface_done++];
        struct face *face = face_from_faceid(h, faceid);
        if (face != NULL) {
            send_content(h, face, content);
            if (content->nface_done < content->faces->n)
                return(choose_content_delay(h,
                        content->faces->buf[content->nface_done],
                        content->flags));
        }
    }
    content->sender = NULL;
    return(0);
}

/*
 * Returns index at which the element was found or added,
 * or -1 in case of error.
 */
static int
indexbuf_unordered_set_insert(struct ccn_indexbuf *x, size_t val)
{
    int i;
    if (x == NULL)
        return (-1);
    for (i = 0; i < x->n; i++)
        if (x->buf[i] == val)
            return(i);
    if (ccn_indexbuf_append_element(x, val) < 0)
        return(-1);
    return(i);
}

static int
content_faces_set_insert(struct content_entry *content, unsigned faceid)
{
    if (content->faces == NULL) {
        content->faces = ccn_indexbuf_create();
        content->nface_done = 0;
    }
    return (indexbuf_unordered_set_insert(content->faces, faceid));
}

static void
schedule_content_delivery(struct ccnd *h, struct content_entry *content)
{
    if (content->sender == NULL &&
          content->faces != NULL &&
          content->faces->n > content->nface_done)
        content->sender = ccn_schedule_event(h->sched,
            choose_content_delay(h, content->faces->buf[content->nface_done],
                                    content->flags),
            content_sender, content, 0);
}

/*
 * cancel_one_propagating_interest:
 * (provided one exists)
 * 
 */
static void
cancel_one_propagating_interest(struct ccnd *h,
                                struct interestprefix_entry *ipe, unsigned faceid)
{
    struct propagating_entry *head = ipe->propagating_head;
    struct propagating_entry *p;
    if (head == NULL)
        return;
    /* XXX - Should we consume the oldest or the newest? */
    for (p = head->next; p != head; p = p->next) {
        if (p->faceid == faceid) {
            finished_propagating(p);
            return;
        }
    }
}

/*
 * match_interests: Find and consume interests that match given content
 * Adds to content->faces the faceids that should receive copies,
 * and schedules content_sender if needed.  Returns number of matches.
 */
static int
match_interests(struct ccnd *h, struct content_entry *content)
{
    int n_matched = 0;
    int n;
    int i;
    int k;
    int ci;
    unsigned faceid;
    struct face *face = NULL;
    unsigned c0 = content->comps[0];
    const unsigned char *key = content->key + c0;
    for (ci = content->ncomps - 1; ci >= 0; ci--) {
        int size = content->comps[ci] - c0;
        struct interestprefix_entry *ipe = hashtb_lookup(h->interestprefix_tab, key, size);
        if (ipe != NULL) {
            n = (ipe->counters == NULL) ? 0 : ipe->counters->n;
            for (i = 0; i < n; i++) {
                /* Use signed count for this calculation */
                intptr_t count = ipe->counters->buf[i];
                if (count > 0) {
                    faceid = ipe->interested_faceid->buf[i];
                    face = face_from_faceid(h, faceid);
                    if (face != NULL) {
                        k = content_faces_set_insert(content, faceid);
                        if (k >= content->nface_done) {
                            n_matched += 1;
                            count -= CCN_UNIT_INTEREST;
                            if (count < 0)
                                count = 0;
                            cancel_one_propagating_interest(h, ipe, faceid);
                        }
                    }
                    else
                        count = 0;
                    ipe->counters->buf[i] = count;
                }
            }
        }
    }
    if (n_matched != 0)
        schedule_content_delivery(h, content);
    return(n_matched);
}

/*
 * match_interest_for_faceid: Find and consume interests that match given
 *  content, restricted to the given faceid.  This is used when a new interest
 *  arrives, so we do not want to cancel any propagating interest for that one.
 *  But since the content may match other interests as well, we do need
 *  to examine all the possible matches anyway to update the counts.
 *  XXX we should chase down propagating interests for those.
 */
static int
match_interest_for_faceid(struct ccnd *h, struct content_entry *content, unsigned faceid)
{
    int n_matched = 0;
    int n;
    int i;
    int k;
    int ci;
    struct face *face = NULL;
    unsigned c0 = content->comps[0];
    const unsigned char *key = content->key + c0;
    for (ci = content->ncomps - 1; ci >= 0; ci--) {
        int size = content->comps[ci] - c0;
        struct interestprefix_entry *ipe = hashtb_lookup(h->interestprefix_tab, key, size);
        if (ipe != NULL) {
            n = (ipe->counters == NULL) ? 0 : ipe->counters->n;
            for (i = 0; i < n; i++) {
                if (faceid == ipe->interested_faceid->buf[i]) {
                    intptr_t count = ipe->counters->buf[i];
                    if (count == 0)
                        break;
                    face = face_from_faceid(h, faceid);
                    if (face != NULL) {
                        k = content_faces_set_insert(content, faceid);
                        if (k >= content->nface_done) {
                            /* XXX TODO - loop over the associated propagating interests and check for qualifier matches. */
                            n_matched += 1;
                            count -= CCN_UNIT_INTEREST;
                            if (count < 0)
                                count = 0;
                        }
                    }
                    else
                        count = 0;
                    ipe->counters->buf[i] = count;
                    break;
                }
            }
        }
    }
    schedule_content_delivery(h, content);
    return(n_matched);
}

/*
 * age_interests:
 * This is called several times per interest halflife to age
 * the interest counters.  Returns the number of still-active counts.
 */
#define CCN_INTEREST_AGING_MICROSEC (CCN_INTEREST_HALFLIFE_MICROSEC / 4)
static int
age_interests(struct ccnd *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct interestprefix_entry *ipe;
    int n;
    int i;
    int n_active = 0;
    hashtb_start(h->interestprefix_tab, e);
    for (ipe = e->data; ipe != NULL; ipe = e->data) {
        n = ipe->counters->n;
        if (n > 0)
            ipe->idle = 0;
        else if ((++ipe->idle) > 8) {
            hashtb_delete(e);
            continue;
        }
        for (i = 0; i < n; i++) {
            size_t count = ipe->counters->buf[i];
            if (count > CCN_UNIT_INTEREST) {
                /* factor of approximately the fourth root of 1/2 */
                ipe->counters->buf[i] = (count * 5 + 3) / 6;
            }
            else if (count > 0) {
                ipe->counters->buf[i] -= 1;
            }
            else {
                /* count was 0, remove this counter */
                ipe->interested_faceid->buf[i] = ipe->interested_faceid->buf[n-1];
                ipe->counters->buf[i] = ipe->counters->buf[n-1];
                i -= 1;
                n -= 1;
                ipe->interested_faceid->n = ipe->counters->n = n;
            }
        }
        n_active += n;
        hashtb_next(e);
    }
    hashtb_end(e);
    return(n_active);
}

/*
 * do_write_BFI:
 * This is temporary...
 */
static void
do_write_BFI(struct ccnd *h, struct face *face,
             unsigned char *data, size_t size) {
    struct ccn_charbuf *c;
    if ((face->flags & CCN_FACE_LINK) != 0) {
        c = charbuf_obtain(h);
        ccn_charbuf_reserve(c, size + 5);
        ccn_charbuf_append_tt(c, CCN_DTAG_CCNProtocolDataUnit, CCN_DTAG);
        ccn_charbuf_append(c, data, size);
        ccn_charbuf_append_closer(c);
        do_write(h, face, c->buf, c->length);
        charbuf_release(h, c);
        return;
    }
    do_write(h, face, data, size);
}

/*
 * This checks for inactivity on datagram faces.
 * Returns number of faces that have gone away.
 */
static int
check_dgram_faces(struct ccnd *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int count = 0;
    hashtb_start(h->dgram_faces, e);
    while (e->data != NULL) {
        struct face *face = e->data;
        if ((face->flags & CCN_FACE_DGRAM) != 0 && face->addr != NULL) {
            if (face->recvcount == 0) {
                count += 1;
                hashtb_delete(e);
                continue;
            }
            face->recvcount = (face->recvcount > 1); /* go around twice */
        }
        hashtb_next(e);
    }
    hashtb_end(e);
    return(count);
}

/*
 * This checks for expired propagating interests.
 * Returns number that have gone away.
 */
static int
check_propagating(struct ccnd *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int count = 0;
    hashtb_start(h->propagating_tab, e);
    while (e->data != NULL) {
        struct propagating_entry *pe = e->data;
        if (pe->interest_msg == NULL) {
            if (pe->size == 0) {
                count += 1;
                hashtb_delete(e);
                continue;
            }
            pe->size = (pe->size > 1); /* go around twice */
        }
        hashtb_next(e);
    }
    hashtb_end(e);
    return(count);
}

static void
check_comm_file(struct ccnd *h)
{
    if (!comm_file_ok()) {
        ccnd_msg(h, "exiting (%s gone)", unlink_this_at_exit);
        exit(0);
    }
}

static int
reap(
    struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    struct ccnd *h = clienth;
    (void)(sched);
    (void)(ev);
    if ((flags & CCN_SCHEDULE_CANCEL) == 0) {
        check_dgram_faces(h);
        check_propagating(h);
        check_comm_file(h);
        if (hashtb_n(h->dgram_faces) > 0 || hashtb_n(h->propagating_tab) > 0)
            return(2 * CCN_INTEREST_HALFLIFE_MICROSEC);
    }
    /* nothing on the horizon, so go away */
    h->reaper = NULL;
    return(0);
}

static void
reap_needed(struct ccnd *h, int init_delay_usec)
{
    if (h->reaper == NULL)
        h->reaper = ccn_schedule_event(h->sched, init_delay_usec, reap, NULL, 0);
}

static int
aging_deamon(
    struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    struct ccnd *h = clienth;
    (void)(sched);
    if ((flags & CCN_SCHEDULE_CANCEL) == 0) {
        age_interests(h);
        if (hashtb_n(h->interestprefix_tab) != 0)
            return(ev->evint);
    }
    /* nothing on the horizon, so go away */
    h->age = NULL;
    return(0);
}

static void
aging_needed(struct ccnd *h)
{
    if (h->age == NULL) {
        int period = CCN_INTEREST_AGING_MICROSEC;
        h->age = ccn_schedule_event(h->sched, period, aging_deamon, NULL, period);
    }
}

/*
 * clean_deamon: weeds expired faceids out of the content table
 * and expires short-term blocking state.
 */
static int
clean_deamon( // XXX - neworder
             struct ccn_schedule *sched,
             void *clienth,
             struct ccn_scheduled_event *ev,
             int flags)
{
    struct ccnd *h = clienth;
    unsigned i;
    unsigned n;
    struct content_entry* content;
    (void)(sched);
    (void)(ev);
    if ((flags & CCN_SCHEDULE_CANCEL) != 0) {
        h->clean = NULL;
        return(0);
    }
    n = h->accession - h->accession_base + 1;
    if (n > h->content_by_accession_window)
        n = h->content_by_accession_window;
    for (i = 0; i < n; i++) {
        content = h->content_by_accession[i];
        if (content != NULL && content->faces != NULL) {
            int j, k, d;
            for (j = 0, k = 0, d = 0; j < content->faces->n; j++) {
                unsigned faceid = content->faces->buf[j];
                struct face *face = face_from_faceid(h, faceid);
                if (face != NULL) {
                    if (j < content->nface_old) {
                        if ((face->flags & CCN_FACE_LINK) != 0)
                            continue;
                    }
                    if (j < content->nface_done)
                        d++;
                    content->faces->buf[k++] = faceid;
                }
            }
            if (k < content->faces->n) {
                content->faces->n = k;
                content->nface_done = d;
            }
            content->nface_old = d;
        }
    }
    return(15000000);
}

static void
clean_needed(struct ccnd *h)
{
    if (h->clean == NULL)
        h->clean = ccn_schedule_event(h->sched, 1000000, clean_deamon, NULL, 0);
}

/*
 * This is where a forwarding table would be plugged in.
 * For now we forward everywhere but the source, subject to scope.
 */
static struct ccn_indexbuf *
get_outbound_faces(struct ccnd *h,
    struct face *from,
    unsigned char *msg,
    struct ccn_parsed_interest *pi)
{
    struct ccn_indexbuf *x = ccn_indexbuf_create();
    unsigned i;
    struct face **a = h->faces_by_faceid;
    int blockmask = 0;
    if (pi->scope == 0)
        return(x);
    if (pi->scope == 1)
        blockmask = CCN_FACE_LINK;
    for (i = 0; i < h->face_limit; i++)
        if (a[i] != NULL && a[i] != from && ((a[i]->flags & blockmask) == 0))
            ccn_indexbuf_append_element(x, a[i]->faceid);
    return(x);
}

static int
indexbuf_member(struct ccn_indexbuf *x, size_t val)
{
    int i;
    if (x == NULL)
        return (-1);
    for (i = x->n - 1; i >= 0; i--)
        if (x->buf[i] == val)
            return(i);
    return(-1);
}

static void
indexbuf_remove_element(struct ccn_indexbuf *x, size_t val)
{
    int i;
    if (x == NULL) return;
    for (i = x->n - 1; i >= 0; i--)
        if (x->buf[i] == val) {
            x->buf[i] = x->buf[--x->n]; /* move last element into vacant spot */
            return;
        }
}

static int
do_propagate(
    struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    struct ccnd *h = clienth;
    struct propagating_entry *pe = ev->evdata;
    (void)(sched);
    if (pe->outbound == NULL || pe->interest_msg == NULL)
        return(0);
    if (flags & CCN_SCHEDULE_CANCEL)
        pe->outbound->n = 0;
    if (pe->outbound->n > 0) {
        unsigned faceid = pe->outbound->buf[--pe->outbound->n];
        struct face *face = face_from_faceid(h, faceid);
        if (face != NULL) {
            do_write_BFI(h, face, pe->interest_msg, pe->size);
            h->interests_sent += 1;
        }
    }
    if (pe->outbound->n == 0) {
        finished_propagating(pe);
        reap_needed(h, 0);
        return(0);
    }
    return(nrand48(h->seed) % 8192 + 500);
}

static int
propagate_interest(struct ccnd *h, struct face *face,
                      unsigned char *msg, size_t msg_size,
                      struct ccn_parsed_interest *pi,
                      struct interestprefix_entry *ipe)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    unsigned char *pkey;
    size_t pkeysize;
    struct ccn_charbuf *cb = NULL;
    int res;
    struct propagating_entry *pe = NULL;
    unsigned char *msg_out = msg;
    size_t msg_out_size = msg_size;
    struct ccn_indexbuf *outbound = get_outbound_faces(h, face, msg, pi);
    if (outbound == NULL || outbound->n == 0) {
        ccn_indexbuf_destroy(&outbound);
        return(0);
    } 
    if (pi->offset[CCN_PI_B_Nonce] == pi->offset[CCN_PI_E_Nonce]) {
        /* This interest has no nonce; add one before going on */
        int noncebytes = 6;
        size_t nonce_start = 0;
        int i;
        unsigned char *s;
        cb = charbuf_obtain(h);
        ccn_charbuf_append(cb, msg, pi->offset[CCN_PI_B_Nonce]);
        nonce_start = cb->length;
        ccn_charbuf_append_tt(cb, CCN_DTAG_Nonce, CCN_DTAG);
        ccn_charbuf_append_tt(cb, noncebytes, CCN_BLOB);
        s = ccn_charbuf_reserve(cb, noncebytes);
        for (i = 0; i < noncebytes; i++)
            s[i] = nrand48(h->seed) >> i;
        cb->length += noncebytes;
        ccn_charbuf_append_closer(cb);
        pkeysize = cb->length - nonce_start;
        ccn_charbuf_append(cb, msg + pi->offset[CCN_PI_B_OTHER],
                               msg_size - pi->offset[CCN_PI_B_OTHER]);
        pkey = cb->buf + nonce_start;
        msg_out = cb->buf;
        msg_out_size = cb->length;
    }
    else {
        pkey = msg + pi->offset[CCN_PI_B_Nonce];
        pkeysize = pi->offset[CCN_PI_E_Nonce] - pi->offset[CCN_PI_B_Nonce];
    }
    hashtb_start(h->propagating_tab, e);
    res = hashtb_seek(e, pkey, pkeysize, 0);
    pe = e->data;
    if (res == HT_NEW_ENTRY) {
        unsigned char *m;
        m = calloc(1, msg_out_size);
        if (m == NULL) {
            res = -1;
            pe = NULL;
            hashtb_delete(e);
        }
        else {
            memcpy(m, msg_out, msg_out_size);
            pe->interest_msg = m;
            pe->size = msg_out_size;
            pe->faceid = face->faceid;
            pe->outbound = outbound;
            outbound = NULL;
            link_propagating_interest_to_interest_entry(h, pe, ipe);
            res = 0;
            ccn_schedule_event(h->sched, nrand48(h->seed) % 8192, do_propagate, pe, 0);
        }
    }
    else if (res == HT_OLD_ENTRY) {
        ccnd_msg(h, "Interesting - this shouldn't happen much - ccnd.c:%d", __LINE__);
        if (pe->outbound != NULL)
            indexbuf_remove_element(pe->outbound, face->faceid);
        res = -1; /* We've seen this already, do not propagate */
    }
    hashtb_end(e);
    if (cb != NULL)
        charbuf_release(h, cb);
    ccn_indexbuf_destroy(&outbound);
    return(res);
}

static int
is_duplicate_flooded(struct ccnd *h, unsigned char *msg, struct ccn_parsed_interest *pi)
{
    struct propagating_entry *pe = NULL;
    size_t nonce_start = pi->offset[CCN_PI_B_Nonce];
    size_t nonce_size = pi->offset[CCN_PI_E_Nonce] - nonce_start;
    if (nonce_size == 0)
        return(0);
    pe = hashtb_lookup(h->propagating_tab, msg + nonce_start, nonce_size);
    return(pe != NULL);
}

int use_short_term_blocking_state = 0;
/*
 * content_is_unblocked: 
 * Decide whether to send content in response to the interest, which
 * we already know is a prefix match.
 */
static int
content_is_unblocked(struct content_entry *content,
        struct ccn_parsed_interest *pi, unsigned char *msg, unsigned faceid)
{
    const unsigned char *filtbuf = NULL;
    size_t filtsize = 0;
    const struct ccn_bloom_wire *f = NULL;
    int k;
    if (pi->offset[CCN_PI_E_OTHER] > pi->offset[CCN_PI_B_OTHER]) {
        struct ccn_buf_decoder decoder;
        struct ccn_buf_decoder *d = ccn_buf_decoder_start(&decoder,
                msg + pi->offset[CCN_PI_B_OTHER],
                pi->offset[CCN_PI_E_OTHER] - pi->offset[CCN_PI_B_OTHER]);
        if (ccn_buf_match_dtag(d, CCN_DTAG_ExperimentalResponseFilter)) {
            ccn_buf_advance(d);
            ccn_buf_match_blob(d, &filtbuf, &filtsize);
            f = ccn_bloom_validate_wire(filtbuf, filtsize);
        }
    }
    if (f != NULL || !use_short_term_blocking_state) {
        if (f != NULL && content->sig_offset > 0 &&
              ccn_bloom_match_wire(f, content->key + content->sig_offset, 32))
            return(0);
        /* Not in filter, so send even if we have sent before. */
        k = indexbuf_member(content->faces, faceid);
        if (0 <= k && k < content->nface_done) {
            content->faces->buf[k] = ~0;
            return(1);
        }
        /* Say no if we are already planning to send anyway */
        return(k == -1);
    }
    return(indexbuf_member(content->faces, faceid) == -1);
}

static void
process_incoming_interest(struct ccnd *h, struct face *face,  // XXX! - neworder
                      unsigned char *msg, size_t size)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_parsed_interest parsed_interest = {0};
    struct ccn_parsed_interest *pi = &parsed_interest;
    size_t namesize = 0;
    int res;
    int matched;
    struct interestprefix_entry *ipe = NULL;
    struct ccn_indexbuf *comps = indexbuf_obtain(h);
    if (size > 65535)
        res = -__LINE__;
    else
        res = ccn_parse_interest(msg, size, pi, comps);
    if (res < 0) {
        ccnd_msg(h, "error parsing Interest - code %d", res);
    }
    else if (pi->scope > 0 && pi->scope < 2 &&
             (face->flags & CCN_FACE_LINK) != 0) {
        ccnd_msg(h, "Interest from %u out of scope - discarded", face->faceid);
        res = -__LINE__;
    }
    else if (is_duplicate_flooded(h, msg, pi)) {
        h->interests_dropped += 1;
    }
    else {
        if (pi->orderpref > 1 || pi->prefix_comps != comps->n - 1)
            face->cached_accession = 0;
        namesize = comps->buf[pi->prefix_comps] - comps->buf[0];
        h->interests_accepted += 1;
        matched = 0;
        hashtb_start(h->interestprefix_tab, e);
        res = hashtb_seek(e, msg + comps->buf[0], namesize, 0);
        ipe = e->data;
        if (res == HT_NEW_ENTRY) {
            ipe->ncomp = comps->n - 1;
            ipe->interested_faceid = ccn_indexbuf_create();
            ipe->counters = ccn_indexbuf_create();
            ccnd_msg(h, "New interest prefix");
        }
        if (ipe != NULL) {
            struct content_entry *content = NULL;
            struct content_entry *last_match = NULL;
            res = indexbuf_unordered_set_insert(ipe->interested_faceid, face->faceid);
            while (ipe->counters->n <= res)
                if (0 > ccn_indexbuf_append_element(ipe->counters, 0)) break;
            if (0 <= res && res < ipe->counters->n)
                ipe->counters->buf[res] += CCN_UNIT_INTEREST;
            // XXX test AnswerOriginKind here.
            if (h->debug) ccnd_debug_ccnb(h, __LINE__, "interest", msg, size);
            content = NULL;
            if (face->cached_accession != 0) {
                /* some help for old clients that are expecting suppression state */
                content = content_from_accession(h, face->cached_accession);
                face->cached_accession = 0;
                if (content != NULL && content_matches_interest_prefix(h, content, msg, comps, pi->prefix_comps))
                    content = content_from_accession(h, content_skiplist_next(h, content));
                if (h->debug && content != NULL)
                    ccnd_debug_ccnb(h, __LINE__, "resume", content->key, content->key_size + content->tail_size);
                if (content != NULL &&
                    !content_matches_interest_prefix(h, content, msg, comps, pi->prefix_comps)) {
                    if (h->debug) ccnd_debug_ccnb(h, __LINE__, "prefix_mismatch", msg, size);
                    content = NULL;
                }
            }
            if (content == NULL) {
                content = find_first_match_candidate(h, msg, pi);
                if (h->debug && content != NULL)
                    ccnd_debug_ccnb(h, __LINE__, "firstmatch", content->key, content->key_size + content->tail_size);
                if (content != NULL &&
                    !content_matches_interest_prefix(h, content, msg, comps, pi->prefix_comps)) {
                    if (h->debug) ccnd_debug_ccnb(h, __LINE__, "prefix_mismatch", msg, size);
                    content = NULL;
                }
            }
            while (content != NULL) {
                if (content_is_unblocked(content, pi, msg, face->faceid) &&
                    content_matches_interest_qualifiers(h, content, msg, pi, comps)) {
                    if (h->debug)
                        ccnd_debug_ccnb(h, __LINE__, "matches", content->key, content->key_size + content->tail_size);
                    if (pi->orderpref != 5) // XXX - should be symbolic
                        break;
                    last_match = content;
                }
                // XXX - accessional ordering is NYI
                content = content_from_accession(h, content_skiplist_next(h, content));
                if (content != NULL &&
                    !content_matches_interest_prefix(h, content, msg, comps, pi->prefix_comps)) {
                    if (h->debug)
                        ccnd_debug_ccnb(h, __LINE__, "prefix_mismatch", content->key, content->key_size + content->tail_size);
                    content = NULL;
                }
            }
            if (last_match != NULL)
                content = last_match;
            if (content != NULL) {
                match_interest_for_faceid(h, content, face->faceid);
                face->cached_accession = content->accession;
                matched = 1;
            }
        }
        hashtb_end(e);
        aging_needed(h);
        if (!matched && pi->scope != 0)
            propagate_interest(h, face, msg, size, pi, ipe);
    }
    indexbuf_release(h, comps);
}

static int
get_signature_offset(struct ccn_parsed_ContentObject *pco, const unsigned char *msg)
{
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;
    int start = pco->offset[CCN_PCO_B_Signature];
    int stop  = pco->offset[CCN_PCO_E_Signature];
    if (start < stop) {
        d = ccn_buf_decoder_start(&decoder, msg + start, stop - start);
        if (ccn_buf_match_dtag(d, CCN_DTAG_Signature)) {
            ccn_buf_advance(d);
            // XXX - only works for default sig
            if (ccn_buf_match_dtag(d, CCN_DTAG_SignatureBits)) {
                ccn_buf_advance(d);
                if (ccn_buf_match_some_blob(d) && d->decoder.numval >= 32) {
                    return(start + d->decoder.index);
                }
            }
            if (ccn_buf_match_some_blob(d) && d->decoder.numval >= 32) {
                    // XXX - this is for compatibilty - remove after July 2008
                    return(start + d->decoder.index);
            }
        }
    }
    return(0);
}

static void
process_incoming_content(struct ccnd *h, struct face *face, // XXX - neworder
                      unsigned char *msg, size_t size)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_parsed_ContentObject obj = {0};
    int res;
    size_t keysize = 0;
    size_t tailsize = 0;
    unsigned char *tail = NULL;
    struct content_entry *content = NULL;
    int i;
    struct ccn_indexbuf *comps = indexbuf_obtain(h);
    res = ccn_parse_ContentObject(msg, size, &obj, comps);
    if (res < 0) {
        ccnd_msg(h, "error parsing ContentObject - code %d", res);
    }
    else if (comps->n < 1 ||
             (keysize = comps->buf[comps->n - 1]) > 65535) {
        ccnd_msg(h, "ContentObject with keysize %lu discarded",
                (unsigned long)keysize);
        ccnd_debug_ccnb(h, __LINE__, "oversize", msg, size);
        res = -__LINE__;
    }
    else {
        if (obj.magic != 20080711) {
            if (++(h->oldformatcontent) == h->oldformatcontentgrumble) {
                h->oldformatcontentgrumble *= 10;
                ccnd_msg(h, "downrev content items received: %d (%d)",
                    h->oldformatcontent,
                    obj.magic);
            }
        }
        keysize = obj.offset[CCN_PCO_B_Content];
        tail = msg + keysize;
        tailsize = size - keysize;
        hashtb_start(h->content_tab, e);
        res = hashtb_seek(e, msg, keysize, tailsize);
        content = e->data;
        if (res == HT_OLD_ENTRY) {
            if (tailsize != e->extsize ||
                  0 != memcmp(tail, e->key + keysize, tailsize)) {
                ccnd_msg(h, "ContentObject name collision!!!!!");
                ccnd_debug_ccnb(h, __LINE__, "new", msg, size);
                ccnd_debug_ccnb(h, __LINE__, "old", e->key, e->keysize + e->extsize);
                content = NULL;
                hashtb_delete(e); /* XXX - Mercilessly throw away both of them. */
                res = -__LINE__;
            }
            else {
                h->content_dups_recvd++;
                ccnd_msg(h, "received duplicate ContentObject from %u (accession %llu)",
                    face->faceid, (unsigned long long)content->accession);
                ccnd_debug_ccnb(h, __LINE__, "dup", msg, size);
                /* Make note that this face knows about this content */
                    // XXX - should distinguish the case that we were waiting
                    //  to send this content - in that case we might have
                    //  some other content we should send instead.
                    // Also, if we have a matching pending interest from this
                    //  face, we should consume it in some cases.
                i = content_faces_set_insert(content, face->faceid);
                if (i >= content->nface_done) {
                    content->faces->buf[i] = content->faces->buf[content->nface_done];
                    content->faces->buf[content->nface_done++] = face->faceid;
                }
            }
        }
        else if (res == HT_NEW_ENTRY) {
            content->accession = ++(h->accession);
            content->faces = ccn_indexbuf_create();
            ccn_indexbuf_append_element(content->faces, face->faceid);
            content->nface_done = 1;
            enroll_content(h, content);
            content->ncomps = comps->n;
            content->comps = calloc(comps->n, sizeof(comps[0]));
            content->sig_offset = get_signature_offset(&obj, msg);
            content->key_size = e->keysize;
            content->tail_size = e->extsize;
            content->key = e->key;
            if (content->comps != NULL && content->faces != NULL) {
                for (i = 0; i < comps->n; i++)
                    content->comps[i] = comps->buf[i];
                content_skiplist_insert(h, content);
            }
            else {
                perror("process_incoming_content");
                hashtb_delete(e);
                res = -__LINE__;
                content = NULL;
            }
        }
        hashtb_end(e);
    }
    indexbuf_release(h, comps);
    if (res >= 0 && content != NULL) {
        int n_matches;
        n_matches = match_interests(h, content);
        if (res == HT_NEW_ENTRY && n_matches == 0)
            content->flags |= CCN_CONTENT_ENTRY_SLOWSEND;
    }
}

static void
process_input_message(struct ccnd *h, struct face *face,
                      unsigned char *msg, size_t size, int pdu_ok)
{
    struct ccn_skeleton_decoder decoder = {0};
    struct ccn_skeleton_decoder *d = &decoder;
    ssize_t dres;
    d->state |= CCN_DSTATE_PAUSE;
    dres = ccn_skeleton_decode(d, msg, size);
    if (d->state >= 0 && CCN_GET_TT_FROM_DSTATE(d->state) == CCN_DTAG) {
        if (pdu_ok && d->numval == CCN_DTAG_CCNProtocolDataUnit) {
            size -= d->index;
            if (size > 0)
                size--;
            msg += d->index;
            face->flags |= CCN_FACE_LINK;
            memset(d, 0, sizeof(*d));
            while (d->index < size) {
                dres = ccn_skeleton_decode(d, msg + d->index, size - d->index);
                if (d->state != 0)
                    break;
                /* The pdu_ok parameter limits the recursion depth */
                process_input_message(h, face, msg + d->index - dres, dres, 0);
            }
            return;
        }
        else if (d->numval == CCN_DTAG_Interest) {
            process_incoming_interest(h, face, msg, size);
            return;
        }
        else if (d->numval == CCN_DTAG_ContentObject) {
            process_incoming_content(h, face, msg, size);
            return;
        }
    }
    ccnd_msg(h, "discarding unknown message; size = %lu", (unsigned long)size);
}

static struct face *
get_dgram_source(struct ccnd *h, struct face *face,
           struct sockaddr *addr, socklen_t addrlen)
{
    struct face *source = NULL;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    if ((face->flags & CCN_FACE_DGRAM) == 0)
        return(face);
    hashtb_start(h->dgram_faces, e);
    res = hashtb_seek(e, addr, addrlen, 0);
    if (res >= 0) {
        source = e->data;
        if (source->addr == NULL) {
            source->addr = e->key;
            source->addrlen = e->keysize;
            source->fd = face->fd;
            source->flags |= CCN_FACE_DGRAM;
            res = enroll_face(h, source);
            ccnd_msg(h, "accepted datagram client id=%d", res);
            reap_needed(h, CCN_INTEREST_HALFLIFE_MICROSEC);
        }
        source->recvcount++;
    }
    hashtb_end(e);
    return(source);
}

static void
process_input(struct ccnd *h, int fd)
{
    struct face *face = NULL;
    struct face *source = NULL;
    ssize_t res;
    ssize_t dres;
    ssize_t msgstart;
    unsigned char *buf;
    struct ccn_skeleton_decoder *d;
    struct sockaddr_storage sstor;
    socklen_t addrlen = sizeof(sstor);
    struct sockaddr *addr = (struct sockaddr *)&sstor;
    face = hashtb_lookup(h->faces_by_fd, &fd, sizeof(fd));
    if (face == NULL)
        return;
    d = &face->decoder;
    if (face->inbuf == NULL)
        face->inbuf = ccn_charbuf_create();
    if (face->inbuf->length == 0)
        memset(d, 0, sizeof(*d));
    buf = ccn_charbuf_reserve(face->inbuf, 8800);
    res = recvfrom(face->fd, buf, face->inbuf->limit - face->inbuf->length,
            /* flags */ 0, addr, &addrlen);
    if (res == -1)
        perror("ccnd: recvfrom");
    else if (res == 0 && (face->flags & CCN_FACE_DGRAM) == 0)
        shutdown_client_fd(h, fd);
    else {
        source = get_dgram_source(h, face, addr, addrlen);
        source->recvcount++;
        if (res <= 1 && (source->flags & CCN_FACE_DGRAM) != 0) {
            ccnd_msg(h, "%d-byte heartbeat on %d", (int)res, source->faceid);
            return;
        }
        face->inbuf->length += res;
        msgstart = 0;
        dres = ccn_skeleton_decode(d, buf, res);
        if (0) ccnd_msg(h, "ccn_skeleton_decode of %d bytes accepted %d",
                        (int)res, (int)dres);
        while (d->state == 0) {
            if (0) ccnd_msg(h, "%lu byte msg received on %d",
                (unsigned long)(d->index - msgstart), fd);
            process_input_message(h, source, face->inbuf->buf + msgstart, 
                                           d->index - msgstart, 1);
            msgstart = d->index;
            if (msgstart == face->inbuf->length) {
                face->inbuf->length = 0;
                return;
            }
            dres = ccn_skeleton_decode(d,
                    face->inbuf->buf + d->index,
                    res = face->inbuf->length - d->index);
            if (0) ccnd_msg(h, "  ccn_skeleton_decode of %d bytes accepted %d",
                            (int)res, (int)dres);
        }
        if ((face->flags & CCN_FACE_DGRAM) != 0) {
            ccnd_msg(h, "ccnd[%d]: protocol error, discarding %d bytes",
                getpid(), (int)(face->inbuf->length - d->index));
            face->inbuf->length = 0;
            return;
        }
        else if (d->state < 0) {
            ccnd_msg(h, "ccnd[%d]: protocol error on fd %d", getpid(), fd);
            shutdown_client_fd(h, fd);
            return;
        }
        if (msgstart < face->inbuf->length && msgstart > 0) {
            /* move partial message to start of buffer */
            memmove(face->inbuf->buf, face->inbuf->buf + msgstart,
                face->inbuf->length - msgstart);
            face->inbuf->length -= msgstart;
            d->index -= msgstart;
        }
    }
}

static void
do_write(struct ccnd *h, struct face *face, unsigned char *data, size_t size)
{
    ssize_t res;
    if (face->outbuf != NULL) {
        ccn_charbuf_append(face->outbuf, data, size);
        return;
    }
    if (face->addr == NULL)
        res = send(face->fd, data, size, 0);
    else {
        res = sendto(face->fd, data, size, 0, face->addr, face->addrlen);
    }
    if (res == size)
        return;
    if (res == -1) {
        if (errno == EAGAIN)
            res = 0;
        else {
            perror("ccnd: send");
            return;
        }
    }
    if ((face->flags & CCN_FACE_DGRAM) != 0) {
        ccnd_msg(h, "sendto short");
        return;
    }
    face->outbuf = ccn_charbuf_create();
    if (face->outbuf == NULL)
        fatal_err("ccnd: ccn_charbuf_create");
    ccn_charbuf_append(face->outbuf, data + res, size - res);
    face->outbufindex = 0;
}

static void
do_deferred_write(struct ccnd *h, int fd)
{
    /* This only happens on connected sockets */
    ssize_t res;
    struct face *face = hashtb_lookup(h->faces_by_fd, &fd, sizeof(fd));
    if (face != NULL && face->outbuf != NULL) {
        ssize_t sendlen = face->outbuf->length - face->outbufindex;
        if (sendlen > 0) {
            res = send(fd, face->outbuf->buf + face->outbufindex, sendlen, 0);
            if (res == -1) {
                perror("ccnd: send");
                shutdown_client_fd(h, fd);
                return;
            }
            if (res == sendlen) {
                face->outbufindex = 0;
                ccn_charbuf_destroy(&face->outbuf);
                return;
            }
            face->outbufindex += res;
            return;
        }
        face->outbufindex = 0;
        ccn_charbuf_destroy(&face->outbuf);
    }
    ccnd_msg(h, "ccnd:do_deferred_write: something fishy on %d", fd);
}

static void
run(struct ccnd *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int i;
    int res;
    int timeout_ms = -1;
    int prev_timeout_ms = -1;
    int usec;
    int specials = 2; /* local_listener_fd, httpd_listener_fd */
    for (;;) {
        usec = ccn_schedule_run(h->sched);
        timeout_ms = (usec < 0) ? -1 : usec / 1000;
        if (timeout_ms == 0 && prev_timeout_ms == 0)
            timeout_ms = 1;
        if (hashtb_n(h->faces_by_fd) + specials != h->nfds) {
            h->nfds = hashtb_n(h->faces_by_fd) + specials;
            h->fds = realloc(h->fds, h->nfds * sizeof(h->fds[0]));
            memset(h->fds, 0, h->nfds * sizeof(h->fds[0]));
        }
        h->fds[0].fd = h->local_listener_fd;
        h->fds[0].events = POLLIN;
        h->fds[1].fd = h->httpd_listener_fd;
        h->fds[1].events = (h->httpd_listener_fd == -1) ? 0 : POLLIN;
        for (i = specials, hashtb_start(h->faces_by_fd, e);
             i < h->nfds && e->data != NULL;
             i++, hashtb_next(e)) {
            struct face *face = e->data;
            h->fds[i].fd = face->fd;
            h->fds[i].events = POLLIN;
            if (face->outbuf != NULL)
                h->fds[i].events |= POLLOUT;
        }
        hashtb_end(e);
        h->nfds = i;
        res = poll(h->fds, h->nfds, timeout_ms);
        prev_timeout_ms = ((res == 0) ? timeout_ms : 1);
        if (-1 == res) {
            perror("ccnd: poll");
            sleep(1);
            continue;
        }
        /* Check for new clients first */
        if (h->fds[0].revents != 0) {
            if (h->fds[0].revents & (POLLERR | POLLNVAL | POLLHUP))
                return;
            if (h->fds[0].revents & (POLLIN))
                accept_new_client(h);
            res--;
        }
        /* Maybe it's time for a status display */
        if (h->fds[1].revents != 0) {
            if (h->fds[1].revents & (POLLIN))
                ccnd_stats_check_for_http_connection(h);
            check_comm_file(h);
            res--;
        }
        for (i = specials; res > 0 && i < h->nfds; i++) {
            if (h->fds[i].revents != 0) {
                res--;
                if (h->fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    if (h->fds[i].revents & (POLLIN))
                        process_input(h, h->fds[i].fd);
                    else
                        shutdown_client_fd(h, h->fds[i].fd);
                    continue;
                }
                if (h->fds[i].revents & (POLLOUT))
                    do_deferred_write(h, h->fds[i].fd);
                else if (h->fds[i].revents & (POLLIN))
                    process_input(h, h->fds[i].fd);
            }
        }
    }
}

static void
ccnd_reseed(struct ccnd *h)
{
    int fd;
        fd = open("/dev/random", O_RDONLY);
    if (fd != -1) {
        read(fd, h->seed, sizeof(h->seed));
        close(fd);
    }
    else {
        h->seed[1] = (unsigned short)getpid(); /* better than no entropy */
        h->seed[2] = (unsigned short)time(NULL);
    }
}

static const char *
ccnd_get_local_sockname(void)
{
    char *s = getenv(CCN_LOCAL_PORT_ENVNAME);
    char name_buf[60];
    if (s == NULL || s[0] == 0 || strlen(s) > 10)
        return(CCN_DEFAULT_LOCAL_SOCKNAME);
    snprintf(name_buf, sizeof(name_buf), "%s.%s",
                     CCN_DEFAULT_LOCAL_SOCKNAME, s);
    return(strdup(name_buf));
}

static struct ccnd *
ccnd_create(void) // XXX - neworder
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct face *face;
    const char *sockname;
    const char *portstr;
    const char *debugstr;
    int fd;
    int res;
    struct ccnd *h;
    struct addrinfo hints = {0};
    struct addrinfo *addrinfo = NULL;
    struct addrinfo *a;
    struct hashtb_param param = { &finalize_face };
    sockname = ccnd_get_local_sockname();
    h = calloc(1, sizeof(*h));
    h->skiplinks = ccn_indexbuf_create();
    param.finalize_data = h;
    h->face_limit = 10; /* soft limit */
    h->faces_by_faceid = calloc(h->face_limit, sizeof(h->faces_by_faceid[0]));
    param.finalize = &finalize_face;
    h->faces_by_fd = hashtb_create(sizeof(struct face), &param);
    h->dgram_faces = hashtb_create(sizeof(struct face), &param);
    param.finalize = &finalize_content;
    h->content_tab = hashtb_create(sizeof(struct content_entry), &param);
    param.finalize = &finalize_interest;
    h->interestprefix_tab = hashtb_create(sizeof(struct interestprefix_entry), &param);
    param.finalize = &finalize_propagating;
    h->propagating_tab = hashtb_create(sizeof(struct propagating_entry), &param);
    h->sched = ccn_schedule_create(h);
    h->oldformatcontentgrumble = 1;
    fd = create_local_listener(sockname, 42);
    if (fd == -1) fatal_err(sockname);
    ccnd_msg(h, "listening on %s", sockname);
    h->local_listener_fd = fd;
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_ADDRCONFIG;
    debugstr = getenv("CCND_DEBUG");
    if (debugstr != NULL && debugstr[0] != 0)
        h->debug = 1;
    portstr = getenv(CCN_LOCAL_PORT_ENVNAME);
    if (portstr == NULL || portstr[0] == 0 || strlen(portstr) > 10)
        portstr = "4485";
    res = getaddrinfo(NULL, portstr, &hints, &addrinfo);
    if (res == 0) {
        for (a = addrinfo; a != NULL; a = a->ai_next) {
            fd = socket(a->ai_family, SOCK_DGRAM, 0);
            if (fd != -1) {
                res = bind(fd, a->ai_addr, a->ai_addrlen);
                if (res != 0) {
                    close(fd);
                    continue;
                }
                res = fcntl(fd, F_SETFL, O_NONBLOCK);
                if (res == -1)
                    perror("fcntl");
                hashtb_start(h->faces_by_fd, e);
                if (hashtb_seek(e, &fd, sizeof(fd), 0) != HT_NEW_ENTRY)
                    exit(1);
                face = e->data;
                face->fd = fd;
                face->flags |= CCN_FACE_DGRAM;
                hashtb_end(e);
                ccnd_msg(h, "accepting datagrams on fd %d", fd);
            }
        }
        freeaddrinfo(addrinfo);
    }
    ccnd_reseed(h);
    clean_needed(h);
    return(h);
}

int
main(int argc, char **argv)
{
    struct ccnd *h;
    signal(SIGPIPE, SIG_IGN);
    h = ccnd_create();
    ccnd_stats_httpd_start(h);
    run(h);
    ccnd_msg(h, "exiting.", (int)getpid());
    exit(0);
}
