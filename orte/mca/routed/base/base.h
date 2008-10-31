/*
 * Copyright (c) 2007      Los Alamos National Security, LLC.
 *                         All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef MCA_ROUTED_BASE_H
#define MCA_ROUTED_BASE_H

#include "orte_config.h"

#include "opal/mca/mca.h"
#include "opal/class/opal_bitmap.h"

#include "orte/mca/routed/routed.h"

BEGIN_C_DECLS

ORTE_DECLSPEC int orte_routed_base_open(void);

#if !ORTE_DISABLE_FULL_SUPPORT

/* struct for tracking routing trees */
typedef struct {
    opal_list_item_t super;
    orte_vpid_t vpid;
    opal_bitmap_t relatives;
} orte_routed_tree_t;
ORTE_DECLSPEC OBJ_CLASS_DECLARATION(orte_routed_tree_t);


/*
 * Global functions for the ROUTED
 */

ORTE_DECLSPEC int orte_routed_base_select(void);
ORTE_DECLSPEC int orte_routed_base_close(void);

ORTE_DECLSPEC extern int orte_routed_base_output;
ORTE_DECLSPEC extern opal_list_t orte_routed_base_components;

ORTE_DECLSPEC extern int orte_routed_base_register_sync(bool setup);

ORTE_DECLSPEC int orte_routed_base_comm_start(void);
ORTE_DECLSPEC int orte_routed_base_comm_stop(void);
ORTE_DECLSPEC extern void orte_routed_base_process_msg(int fd, short event, void *data);
ORTE_DECLSPEC extern void orte_routed_base_recv(int status, orte_process_name_t* sender,
                                                opal_buffer_t* buffer, orte_rml_tag_t tag,
                                                void* cbdata);

#endif /* ORTE_DISABLE_FULL_SUPPORT */

END_C_DECLS

#endif /* MCA_ROUTED_BASE_H */
