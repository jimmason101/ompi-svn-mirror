/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#ifndef OPAL_CRS_BASE_H
#define OPAL_CRS_BASE_H

#include "opal_config.h"
#include "opal/mca/crs/crs.h"

/*
 * Global functions for MCA overall CRS
 */

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    /**
     * Snapshot Object Maintenance functions
     */
    OPAL_DECLSPEC void opal_crs_base_construct(opal_crs_base_snapshot_t *obj);
    OPAL_DECLSPEC void opal_crs_base_destruct( opal_crs_base_snapshot_t *obj);

    /**
     * Initialize the CRS MCA framework
     *
     * @retval OPAL_SUCCESS Upon success
     * @retval OPAL_ERROR   Upon failures
     * 
     * This function is invoked during opal_init();
     */
    OPAL_DECLSPEC int opal_crs_base_open(void);
    
    /**
     * Select an available component.
     *
     * @retval OPAL_SUCCESS Upon Success
     * @retval OPAL_NOT_FOUND If no component can be selected
     * @retval OPAL_ERROR Upon other failure
     *
     */
    OPAL_DECLSPEC int opal_crs_base_select(void);
    
    /**
     * Finalize the CRS MCA framework
     *
     * @retval OPAL_SUCCESS Upon success
     * @retval OPAL_ERROR   Upon failures
     * 
     * This function is invoked during opal_finalize();
     */
    OPAL_DECLSPEC int opal_crs_base_close(void);

    /**
     * Globals
     */
#define opal_crs_base_metadata_filename (strdup("snapshot_meta.data"))

    OPAL_DECLSPEC extern int  opal_crs_base_output;
    OPAL_DECLSPEC extern opal_list_t opal_crs_base_components_available;
    OPAL_DECLSPEC extern opal_crs_base_component_t opal_crs_base_selected_component;
    OPAL_DECLSPEC extern opal_crs_base_module_t opal_crs;
    OPAL_DECLSPEC extern char * opal_crs_base_snapshot_dir;

    /**
     * 'None' component functions
     * These are to be used when no component is selected.
     * They just return success, and empty strings as necessary.
     */
    int opal_crs_base_none_open(void);
    int opal_crs_base_none_close(void);

    int opal_crs_base_none_module_init(void);
    int opal_crs_base_none_module_finalize(void);

    int opal_crs_base_none_checkpoint(    pid_t pid, opal_crs_base_snapshot_t *sanpshot, opal_crs_state_type_t *state);

    int opal_crs_base_none_restart(    opal_crs_base_snapshot_t *snapshot, bool spawn_child, pid_t *child_pid);

    int opal_crs_base_none_disable_checkpoint(void);
    int opal_crs_base_none_enable_checkpoint(void);

    /**
     * Some utility functions
     */
    OPAL_DECLSPEC char * opal_crs_base_state_str(opal_crs_state_type_t state);

    char * opal_crs_base_unique_snapshot_name(pid_t pid);
    char * opal_crs_base_extract_expected_component(char *snapshot_loc, int *prev_pid);
    int    opal_crs_base_init_snapshot_directory(opal_crs_base_snapshot_t *snapshot);
    char * opal_crs_base_get_snapshot_directory(char *uniq_snapshot_name);

    /* Opens the metadata file and places all the base information in the file.
     * Options:
     *  'w' = Open for writing
     *  'a' = Open for writing and appending information
     */
    FILE *opal_crs_base_open_metadata(opal_crs_base_snapshot_t *snapshot, char mode );

    /* Open the metadata file, read off the base information and 
     * return the component and previous pid to the caller.
     * Note: component is allocated inside this function, it is the
     *       callers responsibility to free this memory.
     */
    FILE * opal_crs_base_open_read_metadata(char *location, char **component, int *prev_pid);


#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* OPAL_CRS_BASE_H */
