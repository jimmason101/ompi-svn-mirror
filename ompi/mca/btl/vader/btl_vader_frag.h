/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef MCA_BTL_VADER_SEND_FRAG_H
#define MCA_BTL_VADER_SEND_FRAG_H

#include "ompi_config.h"

#define MCA_BTL_VADER_FLAG_INLINE      0
#define MCA_BTL_VADER_FLAG_SINGLE_COPY 1
#define MCA_BTL_VADER_FLAG_FBOX        2
#define MCA_BTL_VADER_FLAG_COMPLETE    4

struct mca_btl_vader_frag_t;

struct mca_btl_vader_hdr_t {
    volatile intptr_t next; /* next item in fifo. many peers may touch this */
    struct mca_btl_vader_frag_t *frag;
    mca_btl_base_tag_t tag; /* tag associated with this fragment (used to lookup callback) */
    uint8_t flags;          /* vader send flags */
    int src_smp_rank;       /* smp rank of owning process */
    size_t len;             /* length of data following this header */
    struct iovec sc_iov;    /* io vector containing pointer to single-copy data */
};
typedef struct mca_btl_vader_hdr_t mca_btl_vader_hdr_t;

/**
 * shared memory send fragment derived type.
 */
struct mca_btl_vader_frag_t {
    mca_btl_base_descriptor_t base;
    mca_btl_base_segment_t segments[2];
    struct mca_btl_base_endpoint_t *endpoint;
    mca_btl_vader_hdr_t *hdr; /* in the shared memory region */
    ompi_free_list_t *my_list;
};

typedef struct mca_btl_vader_frag_t mca_btl_vader_frag_t;

OBJ_CLASS_DECLARATION(mca_btl_vader_frag_t);

static inline int mca_btl_vader_frag_alloc (mca_btl_vader_frag_t **frag, ompi_free_list_t *list) {
    ompi_free_list_item_t *item;

    OMPI_FREE_LIST_GET_MT(list, item);
    *frag = (mca_btl_vader_frag_t *) item;
    if (OPAL_LIKELY(NULL != item)) {
        if (NULL == (*frag)->hdr) {
            OMPI_FREE_LIST_RETURN_MT(list, (ompi_free_list_item_t *)*frag);
            *frag = NULL;
            return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        }

        (*frag)->hdr->flags = MCA_BTL_VADER_FLAG_INLINE;
        (*frag)->my_list = list;
    }

    return OMPI_SUCCESS;
}

void mca_btl_vader_frag_return (mca_btl_vader_frag_t *frag);

#define MCA_BTL_VADER_FRAG_ALLOC_EAGER(frag)                            \
    mca_btl_vader_frag_alloc (&(frag), &mca_btl_vader_component.vader_frags_eager)
#define MCA_BTL_VADER_FRAG_ALLOC_USER(frag)                             \
    mca_btl_vader_frag_alloc (&(frag), &mca_btl_vader_component.vader_frags_user)
#define MCA_BTL_VADER_FRAG_RETURN(frag) mca_btl_vader_frag_return(frag)


static inline void mca_btl_vader_frag_complete (mca_btl_vader_frag_t *frag) {
    if (OPAL_UNLIKELY(MCA_BTL_DES_SEND_ALWAYS_CALLBACK & frag->base.des_flags)) {
        /* completion callback */
        frag->base.des_cbfunc(&mca_btl_vader.super, frag->endpoint,
                              &frag->base, OMPI_SUCCESS);
    }

    if (OPAL_LIKELY(frag->base.des_flags & MCA_BTL_DES_FLAGS_BTL_OWNERSHIP)) {
        MCA_BTL_VADER_FRAG_RETURN(frag);
    }
}

void mca_btl_vader_frag_init (ompi_free_list_item_t *item, void *ctx);

#endif /* MCA_BTL_VADER_SEND_FRAG_H */
