/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2010 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/**
 * @file ms_lattice.c Word lattices for MultiSphinx.
 */

#include <string.h>

#include <sphinxbase/pio.h>
#include <sphinxbase/err.h>
#include <sphinxbase/strfuncs.h>
#include <sphinxbase/ckd_alloc.h>

#include "ms_lattice.h"

ms_lattice_t *
ms_lattice_init(logmath_t *lmath, dict_t *dict)
{
    ms_lattice_t *l = ckd_calloc(1, sizeof(*l));
    l->refcount = 1;
    l->lmath = logmath_retain(lmath);
    if (dict) {
        l->dict = dict_retain(dict);
        l->autodict = FALSE;
    }
    else {
        l->dict = dict_init(NULL, NULL);
        l->autodict = TRUE;
    }
    l->node_list = garray_init(0, sizeof(ms_latnode_t));
    l->link_list = garray_init(0, sizeof(ms_latlink_t));
    l->node_map = nodeid_map_init();
    return l;
}

ms_lattice_t *
ms_lattice_retain(ms_lattice_t *l)
{
    ++l->refcount;
    return l;
}

int
ms_lattice_free(ms_lattice_t *l)
{
    if (l == NULL)
        return 0;
    if (--l->refcount > 0)
        return l->refcount;

    garray_free(l->node_list);
    nodeid_map_free(l->node_map);
    logmath_free(l->lmath);
    dict_free(l->dict);
    ckd_free(l);
    return 0;
}

ms_latnode_t *
ms_lattice_node_init(ms_lattice_t *l, int sf, int32 lmstate)
{
    return NULL;
}

ms_latnode_t *
ms_lattice_get_node_idx(ms_lattice_t *l, int32 idx)
{
    return garray_ptr(l->node_list, ms_latnode_t, idx);
}

ms_latnode_t *
ms_lattice_get_node_id(ms_lattice_t *l, int sf, int32 lmstate)
{
    int32 idx = nodeid_map_map(l->node_map, sf, lmstate);
    if (idx == -1)
        return NULL;
    return ms_lattice_get_node_idx(l, idx);
}

ms_latnode_t *
ms_lattice_get_start(ms_lattice_t *l)
{
}

ms_latnode_t *
ms_lattice_get_end(ms_lattice_t *l)
{
}

ms_latlink_t *
ms_lattice_link(ms_lattice_t *l,
                ms_latnode_t *src, ms_latnode_t *dest,
                int32 wid, int32 ascr)
{
    ms_latlink_t *arc;
    int32 lid;

    if (src->exits == NULL)
        src->exits = garray_init(0, sizeof(int32));
    if (dest->entries == NULL)
        dest->entries = garray_init(0, sizeof(int32));

    lid = garray_next_idx(l->link_list);
    garray_expand(l->link_list, lid + 1);
    arc = garray_ptr(l->link_list, ms_latlink_t, lid);
    garray_append(src->exits, &lid);
    garray_append(dest->entries, &lid);

    arc->wid = wid;
    arc->ascr = ascr;

    return arc;
}

static int
get_or_create_wid(ms_lattice_t *l, char *word, int alt)
{
    int32 wid;

    /* HACK, but what can we do... */
    if (0 == strcmp(word, "!SENT_END"))
        strcpy(word, "</s>");
    if (0 == strcmp(word, "!SENT_START"))
        strcpy(word, "<s>");
    /* NOTE: In reality we want to collapse alternate pronunciations,
     * but at this point it's best to be faithful to the input.
     * Therefore we include the alternate pronunciation in the
     * language model state, even though that makes no sense in the
     * grand scheme of things. */
    if (alt != 1) {
        /* First make sure the base word exists. */
        wid = dict_wordid(l->dict, word);
        if (wid == -1 && l->autodict)
            wid = dict_add_word(l->dict, word, NULL, 0);
        if (wid == -1) {
            E_ERROR("No base word for %s(%d) in dictionary\n", word, alt);
            return -1;
        }
        sprintf(word + strlen(word), "(%d)", alt);
    }
    wid = dict_wordid(l->dict, word);
    if (wid == -1 && l->autodict)
        wid = dict_add_word(l->dict, word, NULL, 0);
    return wid;
}

static int
process_htk_node_line(ms_lattice_t *l, lineiter_t *li,
                      garray_t *wptr, int n_wptr, int frate)
{
    ms_latnode_t *node = NULL;
    char *word = NULL;
    int32 nodeidx, wid;
    int i, sf, alt = 1;

    for (i = 0; i < n_wptr; ++i) {
        char *f = garray_ent(wptr, char *, i);
        char *e = strchr(f, '=');
        if (e == NULL) {
            E_ERROR("Invalid field %s in line %d\n",
                    f, (int)li->lineno);
            ckd_free(word);
            return -1;
        }
        *e++ = '\0';
        if (0 == strcmp(f, "I")) {
            nodeidx = atoi(e);
            garray_expand(l->node_list, nodeidx + 1);
            node = garray_ptr(l->node_list, ms_latnode_t, nodeidx);
        }
        else if (0 == strcmp(f, "t")) {
            sf = (int)(atof_c(e) * frate);
        }
        else if (0 == strcmp(f, "W")) {
            word = ckd_calloc(1, strlen(e) + 16);
            strcpy(word, e);
        }
        else if (0 == strcmp(f, "v")) {
            alt = atoi(e);
            if (alt < 1 || alt > 255) {
                E_ERROR("Invalid pronunciation alternate %s at line %d\n",
                        e, (int)li->lineno);
                ckd_free(word);
                return -1;
            }
        }
        else {
            E_WARN("Unknown field type %s in line %d\n",
                   f, (int)li->lineno);
        }
    }
    if (node == NULL) {
        E_ERROR("Found no node ID in line %d\n", (int)li->lineno);
        ckd_free(word);
        return -1;
    }
    wid = get_or_create_wid(l, word, alt);
    if (wid == -1) {
        E_ERROR("Failed to add word %s to dictionary\n", word);
        ckd_free(word);
        return -1;
    }
    node->id.sf = sf;
    node->id.lmstate = wid; /* This may get updated later. */
    node->fan = 0;
    node->exits = node->entries = NULL;
    nodeid_map_add(l->node_map, sf, wid, nodeidx);

    ckd_free(word);
    return 0;
}

static int
process_htk_arc_line(ms_lattice_t *l, lineiter_t *li, garray_t *wptr, int n_wptr)
{
    char *word = NULL;
    ms_latnode_t *src = NULL, *dest = NULL;
    ms_latlink_t *arc = NULL;
    int32 wid, ascr, prob;
    int i, alt;

    for (i = 0; i < n_wptr; ++i) {
        char *f = garray_ent(wptr, char *, i);
        char *e = strchr(f, '=');
        if (e == NULL) {
            E_ERROR("Invalid field %s in line %d\n",
                    f, (int)li->lineno);
            ckd_free(word);
            return -1;
        }
        *e++ = '\0';
        if (0 == strcmp(f, "J")) {
            /* Link ID is irrelevant (for now at least). */
        }
        else if (0 == strcmp(f, "S")) {
            src = ms_lattice_get_node_idx(l, atoi(e));
        }
        else if (0 == strcmp(f, "E")) {
            dest = ms_lattice_get_node_idx(l, atoi(e));
        }
        else if (0 == strcmp(f, "W")) {
            word = ckd_calloc(1, strlen(e) + 16);
            strcpy(word, e);
        }
        else if (0 == strcmp(f, "v")) {
            alt = atoi(e);
            if (alt < 1 || alt > 255) {
                E_ERROR("Invalid pronunciation alternate %s at line %d\n",
                        e, (int)li->lineno);
                ckd_free(word);
                return -1;
            }
        }
        else if (0 == strcmp(f, "a")) {
            ascr = logmath_ln_to_log(l->lmath, atof_c(e));
        }
        else if (0 == strcmp(f, "p")) {
            prob = logmath_log(l->lmath, atof_c(e));
        }
        else {
            E_WARN("Unknown field type %s in line %d\n",
                   f, (int)li->lineno);
        }
    }
    if (src == NULL || dest == NULL) {
        E_ERROR("Found no valid src and dest IDs in line %d\n",
                (int)li->lineno);
        ckd_free(word);
        return -1;
    }
    if (word)
        wid = get_or_create_wid(l, word, alt);
    else
        /* NOTE: push forward word IDs immediately (might as well). */
        wid = src->id.lmstate;
    arc = ms_lattice_link(l, src, dest, wid, ascr);

    ckd_free(word);
    return 0;
}

int
ms_lattice_read_htk(ms_lattice_t *l, FILE *fh, int frate)
{
    lineiter_t *li;
    garray_t *wptr;
    int n_wptr;
    int n_nodes = 0, n_arcs = 0;
    int start_idx = -1, end_idx = -1;
    enum {
        HEADER, ENTRIES
    } state = HEADER;

    wptr = garray_init(0, sizeof(char *));
    for (li = lineiter_start(fh); li; li = lineiter_next(li)) {
        string_trim(li->buf, STRING_BOTH);
        if (li->buf[0] == '#')
            continue;
        switch (state) {
        case HEADER:
        {
            char *e;
            if (0 == strncmp(li->buf, "N=", 2)) {
                n_nodes = atoi(li->buf + 2);
                if ((e = strchr(li->buf + 2, '=')) == NULL) {
                    E_ERROR("Invalid node/link count line %d: %s\n",
                            (int)li->lineno, li->buf);
                    goto error_out;
                }
                n_arcs = atoi(e + 1);
                state = ENTRIES;
            }
            else {
                if ((e = strchr(li->buf, '=')) == NULL) {
                    E_ERROR("Invalid header count line %d: %s\n",
                            (int)li->lineno, li->buf);
                    goto error_out;
                }
                *e++ = '\0';
                if (0 == strcmp(li->buf, "start"))
                    start_idx = atoi(e);
                else if (0 == strcmp(li->buf, "end"))
                    end_idx = atoi(e);
            }
            break;
        }
        case ENTRIES: {
            int i;

            n_wptr = str2words(li->buf, NULL, 0);
            garray_expand(wptr, n_wptr);
            str2words(li->buf, garray_void(wptr, 0), n_wptr);
            for (i = 0; i < n_wptr; ++i) {
                char *f = garray_ent(wptr, char *, i);
                if (0 == strncmp(f, "I=", 2)) {
                    if (process_htk_node_line(l, li, wptr, n_wptr, frate) < 0)
                        goto error_out;
                    break;
                }
                if (0 == strncmp(f, "J=", 2)) {
                    if (process_htk_arc_line(l, li, wptr, n_wptr) < 0)
                        goto error_out;
                    break;
                }
            }
            if (i == n_wptr)
                E_WARN("Not a node or arc on line %d\n", (int)li->lineno);
            break;
        }
        }
    }
    if (start_idx == -1) {
        E_WARN("No explicit start node, using first node\n");
        start_idx = 0;
    }
    if (end_idx == -1) {
        end_idx = garray_size(l->node_list) - 1;
        E_WARN("No explicit end node, using last node %d\n", end_idx);
    }
    return 0;

error_out:
    lineiter_free(li);
    return -1;
}

int
ms_lattice_write_htk(ms_lattice_t *l, FILE *fh, int frate)
{
}

int
ms_lattice_write_dot(ms_lattice_t *l, FILE *fh)
{
}