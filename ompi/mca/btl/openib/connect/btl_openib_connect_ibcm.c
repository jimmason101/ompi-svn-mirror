/*
 * Copyright (c) 2007-2008 Cisco, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/*
 * TO-DO:
 *
 * - audit control values passed to req_send()
 * - More show_help() throughout 
 * - error handling in case of broken connection is not good; need to
 *   notify btl module safely
 */

/*
 * For a description of how IBCM works, see chapter 12 of the vol 1 of
 * the IBTA doc.  The general connection scheme described in chapter
 * is a UD-based state-transition protcol that utilizes on a 3 way
 * handshake:
 *                                                                              
 * 1. active side creates local side of the QP and sends connect
 *    request to remote peer.
 * 2. passive side receives the request, makes its local side of QP,
 *    posts receive buffers, transitions the QP to RTR, and sends back
 *    a reply.
 * 3. active side gets reply, transitions its QP to both RTR and RTS,
 *    and sends back a "ready to use" (RTU) message.
 * 4. passive side gets the RTU, transitions its QP to RTS.
 *
 * It looks like this (time flowing in the down direction):
 *
 *         active     passive
 *              |     |
 *      request |---->|
 *              |     |
 *              |<----| reply
 *              |     |
 *          RTU |---->|
 *              |     |
 *
 * The fact that it's based on UD allows the IBCM service to sit on a
 * single, well-advertised location that any remote peer can contact
 * (don't need to setup a per-peer RC QP to talk to it).  But the
 * drawback is that UD fragments can be lost, reordered, etc.
 *
 * The OMPI openib BTL has a few additional requirements:
 *
 * - both peers must have IBCM available and on the same subnet
 * - because of BSRQ, each MPI-level connection to another peer
 *   actually consists of N QPs (where N>=1)
 * - MPI might initiate connections from both directions
 *   "simultaneously"; need to unambigously resolve which one "wins"
 *   in a distributed fashion
 * - MPI may have more than one process per node
 *
 * Note that the IBCM request/reply/RTU messages allow for "private"
 * data to be included, so we can include pointers to relevant data
 * structures to make matching simpler.  The request message includes
 * a pointer that is relevant in the active process context that is
 * echoed back in the reply message.  Similarly, the reply message
 * includes a pointer that is relevant in the passive process context
 * that is echoed back in the RTU message.  This allows active side
 * receipt of the reply and the passive side receipt of the RTU to
 * find their matches easily.  The phase that has to do the most
 * amount of work is the passive side when receiving an incoming
 * connection request.  It has to do a lot of work to find a matching
 * endpoint with wich to accept the request.
 *
 * The IBCM software in OFED has some points that are worth bearing,
 * as they influenced the design and implementation of this CPC:
 *                                                                              
 * - IBCM is actually quite intelligent about timeouts and
 *   retransmissions; it will try to retransmit each request/reply/RTU
 *   several times before reporting the failure up to the ULP (i.e.,
 *   this CPC).
 * - IBCM also nicely detects most obvious duplicates.  For example,
 *   say the active side sends a request, and the passive side sends a
 *   reply.  But the reply gets lost, so the active side sends the
 *   request again.  The passive side will recognize it as a duplicate
 *   because the corresponding CM ID will be in a state indicating
 *   that it sent the reply
 * - One point that was not clear about IBCM to me when I read cm.h is
 *   that there is a function called ib_cm_notify() that is used to
 *   tell IBCM (among other things) when the first message arrives on
 *   a QP when the RTU has not yet been received.  This can happen, of
 *   course, since IBCM traffic is UD.
 * - Also, note that IBCM "listener" IDs are per HCA, not per port.  
 * - CM ID's are persistent throughout the life of a QP.  If you
 *   destroy a CM ID (ib_cm_destroy_id), the IBCM system will tear
 *   down the connection.  So the CM ID you get when receiving a
 *   request message is the same one you'll have all throughout the
 *   life of the QP that you are creating.  Likewise, the CM ID you
 *   explicitly create when *sending* a connect request will exist for
 *   the entire lifetime of the QP that is being created.
 * - In short CM IDs are mapped 1:1 to QPs.
 * - There is a formal disconnect protocol in IBCM that contains a
 *   multi-part handshake.  When you destroy a CM ID
 *   (ib_cm_destroy_id), the disconnect protocol is enacted in the
 *   IBCM kernel (similar to invoking "close()" on a TCP socket -- the
 *   kernel handles the multi-way handshake).
 *
 * With that background, here's how we use the IBCM in the openib BTL
 * IBCM CPC.  Note that we actually have to add a *4th* leg in the
 * handshake (beyond the request, reply, RTU) :-( -- more details
 * below.
 *
 * openib BTL modules have one or more CPC modules.  Each CPC module
 * comes from a different CPC component.  One CPC module is bound to
 * exactly one openib BTL module.  The CPC base glue will find a pair
 * of matching CPC modules and choose them to make the QP(s).  One or
 * more endpoints will be bound to a BTL module, depending on the LMC.
 * There is only one CPC module per BTL module, so multiple endpoints
 * may share a CPC module.  However, QPs are made on a per-endpoint
 * basis, so the CPC caches info on the endpoint, and destroys this
 * info when the endpoint is destroyed (e.g., we destroy the CM IDs in
 * the endpoint CPC destructor -- this fires the IBCM disconnect
 * protocol, but it's handled entirely within the IBCM kernel; we
 * don't have to worry about single threaded progress or deadlock
 * because all progress can happen asynchronously within the kernel).
 *
 * The IBCM CPC adds LID and GUID information into the MPI modex.  So
 * every process does not need to do any lookup on how to use the IBCM
 * system to make a connection to its peer; it can use the LID and GID
 * information to create a path record (vs. querying the SM to get a
 * patch record for that peer) and use that to make the IBCM
 * connection.
 *
 * The IBCM listener service ID is the PID of the MPI process.  This
 * is how we distinguish between multiple MPI processes listening via
 * IBCM on the same host.
 *
 * Since IBCM listners are per HCA (vs. per port), we maintain a list
 * of existing listeners.  When a new query comes in, we check the
 * list to see if the HCA connected to this BTL module already has a
 * listener.  If it does, we OBJ_RETAIN it and move on.  Otherwise, we
 * create a new one.  When CPC modules are destroyed, we simply
 * OBJ_RELEASE the listener object; the destructor takes care of all
 * the actual cleanup.
 *
 * Note that HCAs are capable of having multiple GIDs.  OMPI defaults
 * to using the 0th GID, but the MCA param
 * btl_openib_connect_ibcm_gid_index allows the user to choose a
 * different one.
 *
 * This CPC uses the openib FD monitoring service to listen for
 * incoming IBCM activity on the listener CM IDs.  Activity on these
 * FDs will trigger a callback which launches the dispatcher to handle
 * the incoming event.  There's efforts to make this activity thread
 * safe -- in the case where the IBCM "listener" is in the FD service,
 * which is blocking in its own separate thread.  In recognition of
 * this, the callbacks from the from the FD service is careful about
 * accessing data that the main thread/upper-level openib BTL needs to
 * access, and re-invoking callbacks through the FD service so that
 * the execute in the main thread (vs. running in the FD service
 * thread).  It dual compiles so that when OMPI is compiled in a
 * single-threaded mode, the FD service simply uses the main libevent
 * fd progress engine.  This single-threaded mode has been tested well
 * and appears to work nicely.  The multi-threaded mode has only been
 * lightly tested; we were hampered by other thread safety bugs and
 * couldn't fully test this mode.
 *
 * To avoid some race conditions and complex code for when two MPI
 * process peers initiate connections "simultaneously", we only allow
 * IBCM connections to be made in one direction.  That is, a QP will
 * only be created if the IBCM request flows from a process with the
 * lower GUID to the process with the higher GUID (if two processes
 * are on the same host / have the same GUID, their PIDs are compared
 * instead).  However, since OMPI can operate in a single-threaded
 * mode, if the MPI sender is the "wrong" one (e.g., it has a higher
 * GUID), then it can starve waiting for the other MPI peer to
 * initiate a send.  So we use an alternate scheme in this scenario:
 *
 * 1. The "wrong" process will send a single IBCM connection request
 *    to its peer on a bogus QP that was created just for this
 *    request.  
 * 2. The receiver will get the request, detect that it came
 *    in from the "wrong" direction, and reject it (IBCM has an
 *    explicit provision for rejecting incoming connections).
 * 3. The receiver will then turn around and re-initiate the
 *    connection in the "right" direction.
 * 4. The originator will receive the reject message and destroy the
 *    bogus QP.
 * 5. The originator will then receive the new request message(s) from
 *    the receiver and the connection will proceed in the "right"
 *    direction.
 *
 * In this way, the initial connection request's only purpose is to
 * wake up the receiver so that it can re-initiate the connection in
 * the "right" direction.  The reason for this is to be able to easily
 * detect and handle "simultaneous" connections from both directions.
 * If processes A and B both initiate connections simultaneously, when
 * A receives B's request, it can see that a) it's coming from the
 * "wrong" direction and b) that there's already a connection in
 * progress in the "right" direction.  Regardless, B will eventually
 * get the connection request from the "right" direction and all
 * proceeds as expected.
 *
 * Without this kind of protocol, there would have had to have been
 * complicated buffers / credit negotiation with the upper-level
 * openib BTL (e.g., make some QP's, post some receives, then
 * potentially move those buffers to a different QP, ...etc.).  Yuck.
 *
 * Note that since an endpoint represents 1 or more QPs, we do a few
 * actions "in blocks" because it's cheaper / easier to do a bunch of
 * things at once rather than for each callback for that QP.  For
 * example:
 *
 * - create all CM IDs and send connection requests (in the right
 *   direction only) for mca_btl_openib_component.num_qps.
 * - upon receipt of the first connection request for an endpoint, if
 *   the QPs have not already been created (they will have been
 *   created if we're initiating in the "wrong" direction -- because
 *   start_connect() will have created them already), create all
 *   num_qps QPs.  Also post receive buffers on all QPs.
 *
 * We wholly reply on the IBCM system for all retransmissions and
 * duplicate filtering of IBCM requests, replies, and RTUs.  If IBCM
 * reports a timeout error up to OMPI, we abort the connection.  Lists
 * are maintained of pending IBCM requests and replies solely for
 * error handling; request/reply timeouts are reported via CM ID.  We
 * can cross-reference this CM ID to the endpoint that it was trying
 * to connect via these lists.
 *
 * Note that there is a race condition: because UD is unordered, the
 * first message may arrive on the QP before the RTU has arrived.
 * This will cause an IBV_EVENT_COMM_EST event to be raised, which
 * would then be picked up by the async event handler in the
 * upper-level openib BTL, which would be Bad.  :-(
 *
 * To fix this race, we have to do the following:
 *
 * - Have the active side send the RTU.  This is necessary to
 *   transition the IBCM system on the active side to the "connected"
 *   state.
 * - Have the active side IBCM CPC send a 0 byte message on the new
 *   QP.  Since the QP is RC, it's guaranteed to get there (or die
 *   trying).  So we don't have to play all the UD games.
 * - If the RTU is received first on the passive side, do the normal
 *   RTU processing.
 * - If the 0 byte message is received first on the passive side, call
 *   ib_cm_notify() with the COMM_EST event, which will also do the
 *   normal RTU processing.  If the RTU is received later, the IBCM
 *   system on the passive side will know that it's effectively a
 *   duplicate, and therefore can be ignored.
 */                                                                             

#include "ompi_config.h"

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <infiniband/cm.h>

#include "opal/util/if.h"
#include "opal/util/error.h"
#include "orte/util/output.h"
#include "opal/event/event.h"
#include "opal/class/opal_pointer_array.h"

#include "btl_openib_endpoint.h"
#include "btl_openib_proc.h"
#include "btl_openib_fd.h"
#include "connect/connect.h"

/* JMS to be removed: see #1264 */
#undef event

/*--------------------------------------------------------------------*/

/*
 * Message that this CPC includes in the modex.  Filed are laid out in
 * order to avoid holes.
 */
typedef struct {
    /** The GUID of the port; used to locate the source endpoint when
        an IB CM request arrives */
    uint64_t mm_port_guid;
    /** The service ID that we're listening on */
    uint32_t mm_service_id;
    /** The LID that we're sitting on; it also identifies the source
        endpoint when an IB CM request arrives */
    uint16_t mm_lid;
    /** The port number of this port, also used to locate the source
        endpoint when an IB CM request arrives */
    uint8_t mm_port_num;
} modex_msg_t;

/*
 * Forward reference to a struct defined below
 */
struct ibcm_request_t;

/*
 * Message (private data) that is sent with the IB CM connect request
 */
typedef struct {
    struct ibcm_request_t *ireqd_request;
    uint32_t ireqd_pid;
    uint32_t ireqd_ep_index;
    uint8_t ireqd_qp_index;
} ibcm_req_data_t;

/*
 * Forward reference to a struct defined below
 */
struct ibcm_reply_t;

/*
 * Message (private data) that is sent with the IB CM reply
 */
typedef struct {
    struct ibcm_request_t *irepd_request;
    struct ibcm_reply_t *irepd_reply;
    uint32_t irepd_ep_index;
    uint8_t irepd_qp_index;
} ibcm_rep_data_t;

/*
 * Message (private data) that is sent with the IB CM RTU
 */
typedef struct {
    struct ibcm_reply_t *irtud_reply;
    uint8_t irtud_qp_index;
} ibcm_rtu_data_t;

/*
 * Specific reasons for connection rejection
 */
typedef enum {
    REJ_ALREADY_CONNECTED,
    REJ_QP_ALREADY_CONNECTED,
    REJ_WRONG_DIRECTION,
    REJ_PEER_NOT_FOUND,
    REJ_PASSIVE_SIDE_ERROR,
    REJ_MAX
} ibcm_reject_reason_t;

/*
 * Need to maintain a list of IB CM listening handles since they are
 * per *HCA*, and openib BTL modules are per *LID*.
 */
typedef struct {
    opal_list_item_t super;

    /* IB device context that was used to create this IB CM context */
    struct ibv_context *ib_context;
    /* The actual CM device handle */
    struct ib_cm_device *cm_device;
    /* The listening handle */
    struct ib_cm_id *listen_cm_id;

    /* List of ibcm_module_t's that use this handle */
    opal_list_t ibcm_modules;
} ibcm_listen_cm_id_t;

static void ibcm_listen_cm_id_constructor(ibcm_listen_cm_id_t *h);
static void ibcm_listen_cm_id_destructor(ibcm_listen_cm_id_t *h);
static OBJ_CLASS_INSTANCE(ibcm_listen_cm_id_t, opal_list_item_t, 
                          ibcm_listen_cm_id_constructor,
                          ibcm_listen_cm_id_destructor);

/*
 * Generic base type for holding an ib_cm_id.  Used for base classes
 * of requests, replies, and RTUs.
 */
typedef struct {
    opal_list_item_t super;

    /* The active handle, representing an active CM ID */
    struct ib_cm_id *cm_id;
} ibcm_base_cm_id_t;

static OBJ_CLASS_INSTANCE(ibcm_base_cm_id_t, opal_list_item_t, NULL, NULL);

/*
 * Need to maintain a list of pending CM ID requests (for error
 * handling if the requests timeout).  Need to use the struct name
 * here because it was forward referenced, above.
 */
typedef struct ibcm_request_t {
    ibcm_base_cm_id_t super;

    /* Request */
    struct ib_cm_req_param cm_req;

    /* Path record */
    struct ibv_sa_path_rec path_rec;

    /* Private data sent with the request */
    ibcm_req_data_t private_data;

    /* Endpoint for this request */
    mca_btl_openib_endpoint_t *endpoint;
} ibcm_request_t;

static void ibcm_request_cm_id_constructor(ibcm_request_t *h);
static OBJ_CLASS_INSTANCE(ibcm_request_t, ibcm_base_cm_id_t,
                          ibcm_request_cm_id_constructor, NULL);

/*
 * Need to maintain a list of pending CM ID replies (for error
 * handling if the replies timeout).  Need to use a struct name here
 * because it was forward referenced, above.
 */
typedef struct ibcm_reply_t {
    ibcm_base_cm_id_t super;

    /* Reply */
    struct ib_cm_rep_param cm_rep;

    /* Private data sent with the reply */
    ibcm_rep_data_t private_data;

    /* Endpoint for this reply */
    mca_btl_openib_endpoint_t *endpoint;
} ibcm_reply_t;

static void ibcm_reply_cm_id_constructor(ibcm_reply_t *h);
static OBJ_CLASS_INSTANCE(ibcm_reply_t, ibcm_base_cm_id_t,
                          ibcm_reply_cm_id_constructor, NULL);

/*
 * The IBCM module (i.e., the base module plus more meta data required
 * by this CPC)
 */
typedef struct {
    ompi_btl_openib_connect_base_module_t cpc;

    /* IB CM listen CM ID */
    ibcm_listen_cm_id_t *cmh;

    /* The associated BTL */
    struct mca_btl_openib_module_t *btl;
} ibcm_module_t;

/*
 * List item container for ibcm_module_t
 */
typedef struct {
    opal_list_item_t super;
    ibcm_module_t *ibcm_module;
} ibcm_module_list_item_t;

static OBJ_CLASS_INSTANCE(ibcm_module_list_item_t, opal_list_item_t, 
                          NULL, NULL);

/*
 * Flags for per-endpoint IBCM data
 */
enum {
    CFLAGS_ONGOING = 1,
    CFLAGS_COMPLETED = 2
};

/*
 * Per-endpoint IBCM data
 */
typedef struct {
    /* Pointer to the base */
    ompi_btl_openib_connect_base_module_t *ie_cpc;
    /* Back pointer to the endpoint */
    struct mca_btl_base_endpoint_t *ie_endpoint;

    /* Array of active CM ID's */
    ibcm_base_cm_id_t **ie_cm_id_cache;
    /* Length of ie_request_cm_cache array */
    int ie_cm_id_cache_size;

    /* Used for sending a CM request that we know will be rejected
       (i.e., if inititing in the "wrong" direction) */
    struct ibv_qp *ie_bogus_qp;

    /* Whether we've created all the qp's or not */
    bool ie_qps_created;
    /* Whether all the receive buffers have been posted to the qp's or
       not */
    bool ie_recv_buffers_posted;
    /* IPC between threads in the ibcm CPC */
    volatile uint32_t ie_connection_flags;
    /* How many qp's are left to connect */
    int ie_qps_to_connect;
    /* Lock for IPC between threads in the ibcm CPC */
    opal_mutex_t ie_lock;
} ibcm_endpoint_t;

/*
 * Info passed to start_connect() when it's invoked via an incoming
 * IBCM request
 */
typedef struct {
    ompi_btl_openib_connect_base_module_t *cscd_cpc;
    struct mca_btl_base_endpoint_t *cscd_endpoint;
} callback_start_connect_data_t;

/*--------------------------------------------------------------------*/

static void ibcm_component_register(void);
static int ibcm_component_query(mca_btl_openib_module_t *btl, 
                                ompi_btl_openib_connect_base_module_t **cpc);

static int ibcm_endpoint_init(struct mca_btl_base_endpoint_t *endpoint);
static int ibcm_module_start_connect(ompi_btl_openib_connect_base_module_t *cpc,
                                     mca_btl_base_endpoint_t *endpoint);
static int ibcm_endpoint_finalize(struct mca_btl_base_endpoint_t *endpoint);
static int ibcm_module_finalize(mca_btl_openib_module_t *btl,
                                ompi_btl_openib_connect_base_module_t *cpc);

static void *ibcm_event_dispatch(int fd, int flags, void *context);

/*--------------------------------------------------------------------*/

static bool initialized = false;
static int ibcm_priority = 40;
static int ibcm_gid_table_index = 0;
static uint32_t ibcm_pid;
static opal_list_t ibcm_cm_listeners;
static opal_list_t ibcm_pending_requests;
static opal_list_t ibcm_pending_replies;

/*******************************************************************
 * Component
 *******************************************************************/

ompi_btl_openib_connect_base_component_t ompi_btl_openib_connect_ibcm = {
    "ibcm",
    ibcm_component_register,
    NULL,
    ibcm_component_query,
    NULL
};

/*--------------------------------------------------------------------*/

static void ibcm_component_register(void)
{
    mca_base_param_reg_int(&mca_btl_openib_component.super.btl_version,
                           "connect_ibcm_priority",
                           "The selection method priority for ibcm",
                           false, false, ibcm_priority, &ibcm_priority);
    if (ibcm_priority > 100) {
        ibcm_priority = 100;
    } else if (ibcm_priority < -1) {
        ibcm_priority = 0;
    }

    mca_base_param_reg_int(&mca_btl_openib_component.super.btl_version,
                           "connect_ibcm_gid_index",
                           "GID table index to use to obtain each port's GUID",
                           false, false, ibcm_gid_table_index, 
                           &ibcm_gid_table_index);
    if (ibcm_gid_table_index < 0) {
        ibcm_gid_table_index = 0;
    }
}

/*--------------------------------------------------------------------*/

static int ibcm_component_query(mca_btl_openib_module_t *btl, 
                                ompi_btl_openib_connect_base_module_t **cpc)
{
    int rc;
    modex_msg_t *msg;
    ibcm_module_t *m = NULL;
    opal_list_item_t *item;
    ibcm_listen_cm_id_t *cmh;
    ibcm_module_list_item_t *imli;
    union ibv_gid gid;

    /* If we do not have struct ibv_device.transport_device, then
       we're in an old version of OFED that is IB only (i.e., no
       iWarp), so we can safely assume that we can use this CPC. */
#if defined(HAVE_STRUCT_IBV_DEVICE_TRANSPORT_TYPE)
    if (IBV_TRANSPORT_IB != btl->hca->ib_dev->transport_type) {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                             "openib BTL: ibcm CPC only supported on InfiniBand"));
        rc = OMPI_ERR_NOT_SUPPORTED;
        goto error;
    }
#endif

    /* IBCM is not supported if we have any XRC QPs */
    if (mca_btl_openib_component.num_xrc_qps > 0) {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                             "openib BTL: ibcm CPC not supported with XRC receive queues"));
        rc = OMPI_ERR_NOT_SUPPORTED;
        goto error;
    }

    /* Do some setup only once -- the first time this query function
       is invoked */
    if (!initialized) {
        ibcm_pid = (uint32_t) getpid();
        OBJ_CONSTRUCT(&ibcm_cm_listeners, opal_list_t);
        OBJ_CONSTRUCT(&ibcm_pending_requests, opal_list_t);
        OBJ_CONSTRUCT(&ibcm_pending_replies, opal_list_t);
        initialized = true;
    }

    /* Allocate the module struct.  Use calloc so that it's safe to
       finalize the module if something goes wrong. */
    m = calloc(1, sizeof(*m) + sizeof(*msg));
    if (NULL == m) {
        rc = OMPI_ERR_OUT_OF_RESOURCE;
        goto error;
    }
    msg = (modex_msg_t*) (m + 1);
    OPAL_OUTPUT((-1, "ibcm: created cpc module %p for btl %p",
                 (void*)m, (void*)btl));

    /* See if we've already for an IB CM listener for this device */
    for (item = opal_list_get_first(&ibcm_cm_listeners);
         item != opal_list_get_end(&ibcm_cm_listeners);
         item = opal_list_get_next(item)) {
        cmh = (ibcm_listen_cm_id_t*) item;
        if (cmh->ib_context == btl->hca->ib_dev_context) {
            break;
        }
    }
    /* If we got to the end of the list without finding a match, setup
       IB CM to start listening for connect requests.  Use our PID as
       the service ID. */
    if (opal_list_get_end(&ibcm_cm_listeners) == item) {
        char *filename;

        cmh = OBJ_NEW(ibcm_listen_cm_id_t);
        if (NULL == cmh) {
            rc = OMPI_ERR_OUT_OF_RESOURCE;
            OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                                 "openib BTL: ibcm CPC system error (malloc failed)"));
            goto error;
        }

        /* libibcm <= v1.0.2 will print out a message to stderr if it
           can't find the /dev/infiniband/ucmX device.  So check for
           this file first; if it's not there, don't even bother
           calling ib_cm_open_device().  The "+6" accounts for
           "uverbs". */
        asprintf(&filename, "/dev/infiniband/ucm%s",
                 btl->hca->ib_dev_context->device->dev_name + 6);
	rc = open(filename, O_RDWR);
        if (rc < 0) {
            /* We can't open the device for some reason (can't read,
               can't write, doesn't exist, ...etc.); IBCM is not setup
               on this node. */
            OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                                 "openib BTL: ibcm CPC failed to open IB CM device: %s", filename));
            free(filename);
            rc = OMPI_ERR_NOT_SUPPORTED;
            goto error;
        }
        close(rc);
        free(filename);

        cmh->ib_context = btl->hca->ib_dev_context;
        cmh->cm_device = ib_cm_open_device(btl->hca->ib_dev_context);
        if (NULL == cmh->cm_device) {
            /* If we fail to open the IB CM device, it's not an error
               -- it's likely that IBCM simply isn't supported on this
               platform.  So print an optional message and return
               ERR_NOT_SUPPORTED (i.e., gracefully fail). */
            OBJ_RELEASE(cmh);
            OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                                 "openib BTL: ibcm CPC failed to open IB CM device"));
            rc = OMPI_ERR_NOT_SUPPORTED;
            goto error;
        }

        if (0 != ib_cm_create_id(cmh->cm_device, 
                                 &cmh->listen_cm_id, NULL) ||
            0 != ib_cm_listen(cmh->listen_cm_id, ibcm_pid, 0)) {
            /* Same rationale as above */
            OBJ_RELEASE(cmh);
            OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                                 "openib BTL: ibcm CPC failed to initialize IB CM handles"));
            rc = OMPI_ERR_NOT_SUPPORTED;
            goto error;
        }
        opal_list_append(&ibcm_cm_listeners, &(cmh->super));
    } else {
        /* We found an existing IB CM handle -- bump up the refcount */
        OBJ_RETAIN(cmh);
    }
    m->cmh = cmh;
    imli = OBJ_NEW(ibcm_module_list_item_t);
    if (NULL == imli) {
        OBJ_RELEASE(cmh);
        rc = OMPI_ERR_OUT_OF_RESOURCE;
        goto error;
    }
    imli->ibcm_module = m;
    opal_list_append(&(cmh->ibcm_modules), &(imli->super));

    /* IB CM initialized properly.  So fill in the rest of the CPC
       module. */
    m->btl = btl;
    m->cpc.data.cbm_component = &ompi_btl_openib_connect_ibcm;
    m->cpc.data.cbm_priority = ibcm_priority;
    m->cpc.data.cbm_modex_message = msg;

    /* Note that the LID is already included in the main modex message
       -- it is not ibcm-specific.  Also, don't assume that the port
       GUID is node_guid+port_number (e.g., QLogic HCAs use a
       different formula).  Query for the Nth GID (N = MCA param) on
       the port. */
    if (ibcm_gid_table_index > btl->ib_port_attr.gid_tbl_len) {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                             "openib BTL: ibcm CPC desired GID table index (%d) is larger than the actual table size (%d) on device %s",
                             ibcm_gid_table_index,
                             btl->ib_port_attr.gid_tbl_len,
                             ibv_get_device_name(btl->hca->ib_dev)));
        rc = OMPI_ERR_UNREACH;
        goto error;
    }
    rc = ibv_query_gid(btl->hca->ib_dev_context, btl->port_num, ibcm_gid_table_index, 
                       &gid);
    if (0 != rc) {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                             "openib BTL: ibcm CPC system error (ibv_query_gid failed)"));
        rc = OMPI_ERR_UNREACH;
        goto error;
    }
    msg->mm_port_guid = ntoh64(gid.global.interface_id);
    msg->mm_lid = btl->lid;
    msg->mm_port_num = btl->port_num;
    msg->mm_service_id = ibcm_pid;
    m->cpc.data.cbm_modex_message_len = sizeof(*msg);

    m->cpc.cbm_endpoint_init = ibcm_endpoint_init;
    m->cpc.cbm_start_connect = ibcm_module_start_connect;
    m->cpc.cbm_endpoint_finalize = ibcm_endpoint_finalize;
    m->cpc.cbm_finalize = ibcm_module_finalize;

    /* Start monitoring the fd associated with the cm_device */
    ompi_btl_openib_fd_monitor(cmh->cm_device->fd, OPAL_EV_READ,
                               ibcm_event_dispatch, cmh);

    /* All done */
    *cpc = (ompi_btl_openib_connect_base_module_t *) m;
    OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                         "openib BTL: ibcm CPC available for use on %s",
                         ibv_get_device_name(btl->hca->ib_dev)));
    return OMPI_SUCCESS;

 error:
    ibcm_module_finalize(btl, (ompi_btl_openib_connect_base_module_t *) m);
    if (OMPI_ERR_NOT_SUPPORTED == rc) {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                             "openib BTL: ibcm CPC unavailable for use on %s; skipped",
                             ibv_get_device_name(btl->hca->ib_dev)));
    } else {
        OPAL_OUTPUT_VERBOSE((5, mca_btl_base_output,
                            "openib BTL: ibcm CPC unavailable for use on %s; fatal error %d (%s)",
                             ibv_get_device_name(btl->hca->ib_dev), rc, 
                             opal_strerror(rc)));
    }
    return rc;
}

/*******************************************************************
 * Module
 *******************************************************************/

/*
 * Create the local side of one qp.  The remote side will be connected
 * later.
 */
static int qp_create_one(mca_btl_base_endpoint_t* endpoint, int qp, 
                         struct ibv_srq *srq, uint32_t max_recv_wr, 
                         uint32_t max_send_wr)
{
    mca_btl_openib_module_t *openib_btl = endpoint->endpoint_btl;
    struct ibv_qp *my_qp;
    struct ibv_qp_init_attr init_attr;

    memset(&init_attr, 0, sizeof(init_attr));

    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = openib_btl->hca->ib_cq[BTL_OPENIB_LP_CQ];
    init_attr.recv_cq = openib_btl->hca->ib_cq[qp_cq_prio(qp)];
    init_attr.srq = srq;
    init_attr.cap.max_send_sge = mca_btl_openib_component.ib_sg_list_size;
    init_attr.cap.max_recv_sge = mca_btl_openib_component.ib_sg_list_size;
    init_attr.cap.max_recv_wr = max_recv_wr;
    init_attr.cap.max_send_wr = max_send_wr;

    my_qp = ibv_create_qp(openib_btl->hca->ib_pd, &init_attr); 
    if (NULL == my_qp) { 
        BTL_ERROR(("error creating qp errno says %s", strerror(errno))); 
        return OMPI_ERROR; 
    }

    endpoint->qps[qp].qp->lcl_qp = my_qp;
    openib_btl->ib_inline_max = init_attr.cap.max_inline_data; 
    
    /* Setup meta data on the endpoint */
    endpoint->qps[qp].qp->lcl_psn = lrand48() & 0xffffff;
    endpoint->qps[qp].credit_frag = NULL;

    return OMPI_SUCCESS;
}


/*
 * Create the local side of all the qp's.  The remote sides will be
 * connected later.
 */
static int qp_create_all(mca_btl_base_endpoint_t* endpoint,
                         ibcm_module_t *m)
{
    int qp, rc, pp_qp_num = 0;
    int32_t rd_rsv_total = 0;
    ibcm_endpoint_t *ie = (ibcm_endpoint_t*) endpoint->endpoint_local_cpc_data;

    for (qp = 0; qp < mca_btl_openib_component.num_qps; ++qp) {
        if (BTL_OPENIB_QP_TYPE_PP(qp)) {
            rd_rsv_total +=
                mca_btl_openib_component.qp_infos[qp].u.pp_qp.rd_rsv;
            pp_qp_num++;
        }
    }

    /* if there is no pp QPs we still need reserved WQE for eager rdma flow
     * control */
    if (0 == pp_qp_num && true == endpoint->use_eager_rdma) {
        pp_qp_num = 1;
    }

    for (qp = 0; qp < mca_btl_openib_component.num_qps; ++qp) { 
        struct ibv_srq *srq = NULL;
        uint32_t max_recv_wr, max_send_wr;
        int32_t rd_rsv, rd_num_credits;

        /* QP used for SW flow control need some additional recourses */
        if (qp == mca_btl_openib_component.credits_qp) {
            rd_rsv = rd_rsv_total;
            rd_num_credits = pp_qp_num;
        } else {
            rd_rsv = rd_num_credits = 0;
        }

        if (BTL_OPENIB_QP_TYPE_PP(qp)) {
            max_recv_wr = mca_btl_openib_component.qp_infos[qp].rd_num + 
                rd_rsv;
            max_send_wr = mca_btl_openib_component.qp_infos[qp].rd_num +
                rd_num_credits;
        } else {
            srq = endpoint->endpoint_btl->qps[qp].u.srq_qp.srq;
            /* no receives are posted to SRQ qp */
            max_recv_wr = 0;
            max_send_wr = mca_btl_openib_component.qp_infos[qp].u.srq_qp.sd_max
                + rd_num_credits;
        }

        /* Go create the actual qp */
        rc = qp_create_one(endpoint, qp, srq, max_recv_wr, max_send_wr);
        if (OMPI_SUCCESS != rc) {
            return rc;
        }
    }

    /* All done! */
    ie->ie_qps_created = true;
    return OMPI_SUCCESS;
}


/*
 * Fill in a path record for a peer.  For the moment, use the RDMA CM,
 * but someday we might just fill in the values ourselves.
 */
static int fill_path_record(ibcm_module_t *m,
                            mca_btl_base_endpoint_t *endpoint,
                            struct ibv_sa_path_rec *path_rec)
{
    modex_msg_t *remote_msg = 
        (modex_msg_t*) endpoint->endpoint_remote_cpc_data->cbm_modex_message;
    modex_msg_t *local_msg = 
        (modex_msg_t*) m->cpc.data.cbm_modex_message;

    OPAL_OUTPUT((-1, "filling path record"));
    /* Global attributes */
    path_rec->dgid.global.subnet_prefix = 
        path_rec->sgid.global.subnet_prefix = 
        m->btl->port_info.subnet_id;
    path_rec->dgid.global.interface_id = hton64(remote_msg->mm_port_guid);
    path_rec->sgid.global.interface_id = hton64(local_msg->mm_port_guid);
    path_rec->dlid = htons(endpoint->rem_info.rem_lid);
    path_rec->slid = htons(m->btl->port_info.lid);

    /* Several remarks below are from e-mail exchanges with Sean
       Hefty.  We probably don't need all of these items, but I'm
       going to include them anyway (along with comments explaining
       why we don't need them) just for the sake of someone who is
       going to look at this code in the future. */
    /* 0 = IB traffic */
    path_rec->raw_traffic = 0;

    /* This is QoS stuff, which we're not using -- so just set to 0 */
    path_rec->flow_label = 0;
    path_rec->hop_limit = 0;
    path_rec->traffic_class = 0;

    /* IBCM currently only supports reversible paths */
    path_rec->reversible = 0x1000000;

    /* These are only used for SA queries, so set numb_path 1 to and
       others to 2 (from Sean) */
    path_rec->numb_path = 1;
    path_rec->mtu_selector = 2;
    path_rec->rate_selector = 2;
    path_rec->packet_life_time_selector = 2;

    /* Indicates which path record to use -- use the 0th one */
    path_rec->preference = 0;

    /* If the user specified a pkey, use it.  If not, use the first
       pkey on this port */
    path_rec->pkey = mca_btl_openib_component.ib_pkey_val;
    if (0 == path_rec->pkey) {
        uint16_t pkey;
        ibv_query_pkey(endpoint->endpoint_btl->hca->ib_dev_context, 
                       endpoint->endpoint_btl->port_num, 0, &pkey);
        path_rec->pkey = ntohs(pkey);
    }

    path_rec->packet_life_time = mca_btl_openib_component.ib_timeout;
    path_rec->packet_life_time = 0;
    path_rec->sl = mca_btl_openib_component.ib_service_level;
    path_rec->mtu = endpoint->rem_info.rem_mtu;

    /* The rate is actually of type enum ibv_rate.  Figure it out from
       the bandwidth that we calculated for the btl. */
    switch (m->btl->super.btl_bandwidth) {
    case 2000:
        path_rec->rate = IBV_RATE_2_5_GBPS; break;
    case 4000:
        path_rec->rate = IBV_RATE_5_GBPS; break;
    case 8000:
        path_rec->rate = IBV_RATE_10_GBPS; break;
    case 16000:
        path_rec->rate = IBV_RATE_20_GBPS; break;
    case 24000:
        path_rec->rate = IBV_RATE_30_GBPS; break;
    case 32000:
        path_rec->rate = IBV_RATE_40_GBPS; break;
    case 48000:
        path_rec->rate = IBV_RATE_60_GBPS; break;
    case 64000:
        path_rec->rate = IBV_RATE_80_GBPS; break;
    case 96000:
        path_rec->rate = IBV_RATE_120_GBPS; break;
    default:
        /* Shouldn't happen */
        path_rec->rate = IBV_RATE_MAX; break;
    }

    OPAL_OUTPUT((-1, "Got src/dest subnet id: 0x%lx / 0x%lx", 
                 path_rec->sgid.global.subnet_prefix,
                 path_rec->dgid.global.subnet_prefix));
    OPAL_OUTPUT((-1, "Got src/dest interface id: 0x%lx / 0x%lx", 
                 path_rec->sgid.global.interface_id,
                 path_rec->dgid.global.interface_id));
    OPAL_OUTPUT((-1, "Got src/dest lid: 0x%x / 0x%x", 
                 path_rec->slid, path_rec->dlid));
    OPAL_OUTPUT((-1, "Got raw_traffic: %d\n", path_rec->raw_traffic));

    OPAL_OUTPUT((-1, "Got flow_label: %d\n", path_rec->flow_label));
    OPAL_OUTPUT((-1, "Got hop_limit: %d\n", path_rec->hop_limit));
    OPAL_OUTPUT((-1, "Got traffic_class: %d\n", path_rec->traffic_class));
    OPAL_OUTPUT((-1, "Got reversible: 0x%x\n", path_rec->reversible));
    OPAL_OUTPUT((-1, "Got numb_path: %d\n", path_rec->numb_path));
    OPAL_OUTPUT((-1, "Got pkey: 0x%x\n", path_rec->pkey));

    OPAL_OUTPUT((-1, "Got sl: %d\n", path_rec->sl));
    OPAL_OUTPUT((-1, "Got mtu_selector: %d\n", path_rec->mtu_selector));
    OPAL_OUTPUT((-1, "Got mtu: %d\n", path_rec->mtu));
    OPAL_OUTPUT((-1, "Got rate_selector: %d\n", path_rec->rate_selector));
    OPAL_OUTPUT((-1, "Got rate: %d\n", path_rec->rate));
    OPAL_OUTPUT((-1, "Got packet_life_time_selector: %d\n", path_rec->packet_life_time_selector));
    OPAL_OUTPUT((-1, "Got packet lifetime: 0x%x\n", path_rec->packet_life_time));
    OPAL_OUTPUT((-1, "Got preference: %d\n", path_rec->preference));

    return OMPI_SUCCESS;
}

static int ibcm_endpoint_init(struct mca_btl_base_endpoint_t *endpoint)
{
    ibcm_endpoint_t *ie = endpoint->endpoint_local_cpc_data = 
        calloc(1, sizeof(ibcm_endpoint_t));
    if (NULL == ie) {
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    OPAL_OUTPUT((-1, "ibcm endpoint init for endpoint %p / %p", 
                 (void*)endpoint, (void*)ie));
    ie->ie_cpc = endpoint->endpoint_local_cpc;
    ie->ie_endpoint = endpoint;
    ie->ie_qps_created = 
        ie->ie_recv_buffers_posted = false;
    ie->ie_qps_to_connect = mca_btl_openib_component.num_qps;

    return OMPI_SUCCESS;
}

/* To avoid all kinds of nasty race conditions (because IBCM is based
 * on UD, in which the ordering of messages is not guaranteed), we
 * only allow connections to be made in one direction.  So use a
 * simple (arbitrary) test to decide which direction is allowed to
 * initiate the connection: the process with the lower GUID wins.  If
 * the GUIDs are the same (i.e., the MPI procs are on the same node),
 * then compare PIDs.
 */
static bool i_initiate(ibcm_module_t *m,
                       mca_btl_openib_endpoint_t *endpoint)
{
    modex_msg_t *msg = 
        (modex_msg_t*) endpoint->endpoint_remote_cpc_data->cbm_modex_message;
    uint64_t my_port_guid = ntoh64(m->btl->hca->ib_dev_attr.node_guid) + 
        m->btl->port_num;
    
    OPAL_OUTPUT((-1, "i_initiate: my guid (%0lx), msg guid (%0lx)",
                 my_port_guid, msg->mm_port_guid));
    OPAL_OUTPUT((-1, "i_initiate: my pid (%d), msg pid (%d)",
                 ibcm_pid, msg->mm_service_id));

    return
        (my_port_guid == msg->mm_port_guid &&
         ibcm_pid < msg->mm_service_id) ? true : 
        (my_port_guid < msg->mm_port_guid) ? true : false;
}

/*
 * Allocate a CM request structure and initialize some common fields
 * (that are independent of the specific QP, etc.)
 */
static ibcm_request_t *alloc_request(ibcm_module_t *m, modex_msg_t *msg,
                                     struct ibv_sa_path_rec *path_rec,
                                     mca_btl_base_endpoint_t *endpoint)
{
    struct ib_cm_req_param *cm_req;
    ibcm_request_t *req = OBJ_NEW(ibcm_request_t);
    OPAL_OUTPUT((-1, "allocated cached req id: %p", (void*)req));
        
    if (NULL == req) {
        return NULL;
    }
    
    /* Create this CM ID */
    if (0 != ib_cm_create_id(m->cmh->cm_device,
                             &(req->super.cm_id),
                             NULL)) {
        OPAL_OUTPUT((-1, "ib cm: failed to create active device id"));
        OBJ_RELEASE(req);
        return NULL;
    }
    
    /* This data is constant for all the QP's */
    req->path_rec = *path_rec;
    req->endpoint = endpoint;
    
    cm_req = &(req->cm_req);
    cm_req->qp_type = IBV_QPT_RC;
    cm_req->alternate_path = NULL;
    cm_req->service_id = msg->mm_service_id;
    cm_req->responder_resources = 10;
    cm_req->initiator_depth = 10;
    cm_req->retry_count = mca_btl_openib_component.ib_retry_count;
    cm_req->peer_to_peer = 0;
    /* JMS what does this do? */
    cm_req->flow_control = 0;
    /* JMS what's the units? */
    cm_req->remote_cm_response_timeout = 20;
    cm_req->local_cm_response_timeout = 20;
    cm_req->max_cm_retries = 5;
    
    req->private_data.ireqd_pid = ibcm_pid;
    req->private_data.ireqd_ep_index = endpoint->index;

    return req;
}
 
static int ibcm_module_start_connect(ompi_btl_openib_connect_base_module_t *cpc,
                                     mca_btl_base_endpoint_t *endpoint)
{
    int i, rc, num_ids;
    ibcm_module_t *m = (ibcm_module_t *) cpc;
    ibcm_endpoint_t *ie = 
        (ibcm_endpoint_t *) endpoint->endpoint_local_cpc_data;
    modex_msg_t *msg = 
        (modex_msg_t*) endpoint->endpoint_remote_cpc_data->cbm_modex_message;
    struct ibv_sa_path_rec path_rec;
    bool do_initiate;

    OPAL_OUTPUT((-1,"ibcm start connect, endpoint %p (lid %d, ep index %d)", 
                 (void*)endpoint, endpoint->endpoint_btl->port_info.lid,
                 endpoint->index));

    /* Has an incoming request already initiated the connect sequence
       on this endpoint?  If so, just exit successfully -- the
       incoming request process will eventually complete
       successfully. */
    opal_mutex_lock(&ie->ie_lock);
    if (0 != ie->ie_connection_flags) {
        opal_mutex_unlock(&ie->ie_lock);
        OPAL_OUTPUT((-1,"ibcm start connect already ongoing %p", (void*)endpoint));
        return OMPI_SUCCESS;
    }
    ie->ie_connection_flags = CFLAGS_ONGOING;
    opal_mutex_unlock(&ie->ie_lock);

    /* Set the endpoint state to "connecting" (this function runs in
       the main MPI thread; not the service thread, so we can set the
       endpoint_state here). */
    endpoint->endpoint_state = MCA_BTL_IB_CONNECTING;

    /* Fill in the path record for this peer */
    if (OMPI_SUCCESS != fill_path_record(m, endpoint, &path_rec)) {
        OPAL_OUTPUT((-1, "================ start connect failed!!!"));
        rc = OMPI_ERR_NOT_FOUND;
        goto err;
    }
        
    /* If we're not the initiator, make a bogus QP (must be done
       before we make all the other QPs) */

    do_initiate = i_initiate(m, endpoint);
    if (!do_initiate) {
        rc = qp_create_one(endpoint, 0, NULL, 1, 1);
        if (OMPI_SUCCESS != rc) {
            goto err;
        }
        ie->ie_bogus_qp = endpoint->qps[0].qp->lcl_qp;
    }
        
    /* Make the local side of all the QP's */
    if (OMPI_SUCCESS != (rc = qp_create_all(endpoint, m))) {
        goto err;
    }

    /* Check initiation direction (see comment above i_initiate()
       function): 

       - if this is the side that is not supposed to initiate, then
         send a single bogus request that we expect to be rejected.
         The purpose of the request is to wake up the other side to
         force *them* to initiate the connection.

       - if this is the real initiation side, we create all the QPs
         and send off as many connect requests as is appropriate

       Note that there are completel separate code paths for these two
       cases.  Having separate paths for these options makes the code
       *much* cleaner / easier to read at the expense of some
       duplication.  Trying to integrate the two results in oodles of
       special cases that just isn't worth it. */

    if (do_initiate) {
        ie->ie_cm_id_cache_size = mca_btl_openib_component.num_qps;
        ie->ie_cm_id_cache = calloc(ie->ie_cm_id_cache_size,
                                    sizeof(ibcm_base_cm_id_t*));
        if (NULL == ie->ie_cm_id_cache) {
            OPAL_OUTPUT((-1, "ib cm: failed to malloc %d active device ids",
                         num_ids));
            rc = OMPI_ERR_OUT_OF_RESOURCE;
            goto err;
        }

        for (i = 0; i < mca_btl_openib_component.num_qps; ++i) {
            ibcm_request_t *req;
            struct ib_cm_req_param *cm_req;

            /* Allocate a CM ID cache object */
            ie->ie_cm_id_cache[i] = OBJ_NEW(ibcm_base_cm_id_t);
            if (NULL == ie->ie_cm_id_cache[i]) {
                rc = OMPI_ERR_OUT_OF_RESOURCE;
                goto err;
            }

            /* Initialize the request-common fields */
            req = alloc_request(m, msg, &path_rec, endpoint);
            if (NULL == req) {
                rc = OMPI_ERR_OUT_OF_RESOURCE;
                goto err;
            }
            ie->ie_cm_id_cache[i]->cm_id = req->super.cm_id;

            /* On PP QPs we have SW flow control, no need for rnr
               retries. Setting it to zero helps to catch bugs */
            cm_req = &(req->cm_req);
            cm_req->rnr_retry_count = BTL_OPENIB_QP_TYPE_PP(i) ? 0 :
                mca_btl_openib_component.ib_rnr_retry;
            cm_req->srq = BTL_OPENIB_QP_TYPE_SRQ(i);
            cm_req->qp_num = endpoint->qps[i].qp->lcl_qp->qp_num;
            cm_req->starting_psn = endpoint->qps[i].qp->lcl_psn;
            OPAL_OUTPUT((-1, "ibcm: sending my qpn %d, psn %d\n", 
                         cm_req->qp_num, cm_req->starting_psn));
            
            req->private_data.ireqd_request = req;
            req->private_data.ireqd_qp_index = i;
            
            /* Send the request */
            OPAL_OUTPUT((-1, "ibcm sending connect request %d of %d (id %p)",
                        i, mca_btl_openib_component.num_qps,
                         (void*)req->super.cm_id));
            if (0 != ib_cm_send_req(req->super.cm_id, cm_req)) {
                rc = OMPI_ERR_UNREACH;
                goto err;
            }

            /* Save the request on the global "pending requests" list */
            opal_list_append(&ibcm_pending_requests, &(req->super.super));
        }
    }

    /* The case where we're sending the request and expecting it to be
       rejected */
    else {
        ibcm_request_t *req;
        struct ib_cm_req_param *cm_req;

        /* Initialize the request-common fields */
        req = alloc_request(m, msg, &path_rec, endpoint);
        if (NULL == req) {
            rc = OMPI_ERR_OUT_OF_RESOURCE;
            goto err;
        }
        cm_req = &(req->cm_req);

        /* Setup one request to be sent (and eventually rejected) */
        cm_req->rnr_retry_count = mca_btl_openib_component.ib_rnr_retry;
        cm_req->srq = 0;
        cm_req->qp_num = ie->ie_bogus_qp->qp_num;
        cm_req->starting_psn = 0;
        OPAL_OUTPUT((-1, "ibcm: sending BOGUS qpn %d, psn %d (id %p)", 
                    cm_req->qp_num, cm_req->starting_psn,
                     (void*)req->super.cm_id));

        req->private_data.ireqd_request = req;
        req->private_data.ireqd_qp_index = 0;

        /* Send the request */
        if (0 != (rc = ib_cm_send_req(req->super.cm_id, cm_req))) {
            return OMPI_ERR_UNREACH;
        }

        /* Save the request on the global "pending requests" list */
        opal_list_append(&ibcm_pending_requests, &(req->super.super));
    }

    return OMPI_SUCCESS;

 err:
    if (NULL != ie && NULL != ie->ie_cm_id_cache) {
        free(ie->ie_cm_id_cache);
        ie->ie_cm_id_cache = NULL;
        ie->ie_cm_id_cache_size = 0;
    }
    return rc;
}

/*--------------------------------------------------------------------*/

/*
 * Callback from when we stop monitoring the cm_device fd
 */
static void *callback_unlock(int fd, int flags, void *context)
{
    opal_mutex_t *m = (opal_mutex_t*) context;
    OPAL_THREAD_UNLOCK(m);
    return NULL;
}

/*--------------------------------------------------------------------*/

static void ibcm_listen_cm_id_constructor(ibcm_listen_cm_id_t *cmh)
{
    OBJ_CONSTRUCT(&(cmh->ibcm_modules), opal_list_t);
}

static void ibcm_listen_cm_id_destructor(ibcm_listen_cm_id_t *cmh)
{
    opal_mutex_t mutex;
    opal_list_item_t *item;

    /* Remove all the ibcm module items */
    for (item = opal_list_remove_first(&(cmh->ibcm_modules));
         NULL != item; 
         item = opal_list_remove_first(&(cmh->ibcm_modules))) {
        OBJ_RELEASE(item);
    }

    /* Remove this handle from the ibcm_cm_listeners list */
    for (item = opal_list_get_first(&ibcm_cm_listeners);
         item != opal_list_get_end(&ibcm_cm_listeners);
         item = opal_list_get_next(item)) {
        if (item == &(cmh->super)) {
            opal_list_remove_item(&ibcm_cm_listeners, item);
            break;
        }
    }

    /* If this handle wasn't in the ibcm_cm_listeners list, then this
       handle was destroyed before it was used and we're done.
       Otherwise, there's some more cleanup to do. */
    if (item != opal_list_get_end(&ibcm_cm_listeners)) {

        /* Stop monitoring the cm_device's fd (wait for it to be
           released from the monitoring entity) */
        OPAL_THREAD_LOCK(&mutex);
        ompi_btl_openib_fd_unmonitor(cmh->cm_device->fd, 
                                     callback_unlock,
                                     &mutex);
        OPAL_THREAD_LOCK(&mutex);

        /* Destroy the listener */
        if (NULL != cmh->listen_cm_id) {
            ib_cm_destroy_id(cmh->listen_cm_id);
        }

        /* Close the CM device */
        if (NULL != cmh->cm_device) {
            ib_cm_close_device(cmh->cm_device);
        }
    }
}

/*--------------------------------------------------------------------*/

static void ibcm_request_cm_id_constructor(ibcm_request_t *h)
{
    memset(&(h->cm_req), 0, sizeof(h->cm_req));
    memset(&(h->path_rec), 0, sizeof(h->path_rec));
    memset(&(h->private_data), 0, sizeof(h->private_data));

    h->cm_req.primary_path = &(h->path_rec);
    h->cm_req.private_data = &(h->private_data);
    h->cm_req.private_data_len = sizeof(h->private_data);
}

/*--------------------------------------------------------------------*/

static void ibcm_reply_cm_id_constructor(ibcm_reply_t *h)
{
    memset(&(h->cm_rep), 0, sizeof(h->cm_rep));
    memset(&(h->private_data), 0, sizeof(h->private_data));

    h->cm_rep.private_data = &(h->private_data);
    h->cm_rep.private_data_len = sizeof(h->private_data);
}

/*--------------------------------------------------------------------*/

static int ibcm_endpoint_finalize(struct mca_btl_base_endpoint_t *endpoint)
{
    ibcm_endpoint_t *ie =
        (ibcm_endpoint_t *) endpoint->endpoint_local_cpc_data;
    OPAL_OUTPUT((-1, "ibcm endpoint finalize: %p", (void*)endpoint));
    
    /* Free the stuff we allocated in ibcm_module_init */
    if (NULL != ie) {
        int i;
        for (i = 0; i < ie->ie_cm_id_cache_size; ++i) {
            if (NULL != ie->ie_cm_id_cache[i]) {
                OPAL_OUTPUT((-1, "Endpoint %p (%p), destroying ID %d (%p)\n",
                            (void*)endpoint,
                            (void*)ie,
                             i, (void*)&(ie->ie_cm_id_cache[i]->cm_id)));
                ib_cm_destroy_id(ie->ie_cm_id_cache[i]->cm_id);
                OBJ_RELEASE(ie->ie_cm_id_cache[i]);
            }
        }

        if (ie->ie_cm_id_cache_size > 0) {
            free(ie->ie_cm_id_cache);
        }
        free(ie);
        endpoint->endpoint_local_cpc_data = NULL;
    }

    OPAL_OUTPUT((-1, "ibcm endpoint finalize done: %p", (void*)endpoint));
    return OMPI_SUCCESS;
}

/*--------------------------------------------------------------------*/

static int ibcm_module_finalize(mca_btl_openib_module_t *btl,
                                ompi_btl_openib_connect_base_module_t *cpc)
{
    ibcm_module_t *m = (ibcm_module_t *) cpc;

    /* If we previously successfully initialized, then destroy
       everything */
    if (NULL != m && NULL != m->cmh) {
        OBJ_RELEASE(m->cmh);
    }
    
    return OMPI_SUCCESS;
}

/*--------------------------------------------------------------------*/

/*
 * We have received information about the remote peer's QP; move the
 * local QP through INIT to RTR.
 */
static int qp_to_rtr(int qp_index, struct ib_cm_id *cm_id,
                     mca_btl_openib_endpoint_t *endpoint)
{
    int attr_mask;
    struct ibv_qp_attr attr;
    struct ibv_qp *qp = endpoint->qps[qp_index].qp->lcl_qp;
    mca_btl_openib_module_t *btl = endpoint->endpoint_btl;
    enum ibv_mtu mtu;

    /* IB CM does not negotiate the MTU for us, so we have to figure
       it out ourselves.  Luckly, we know what the MTU is of the other
       port (from its modex message), so we can figure out the highest
       MTU that we have in common. */
    mtu = (btl->hca->mtu < endpoint->rem_info.rem_mtu) ?
        btl->hca->mtu : endpoint->rem_info.rem_mtu;

    if (mca_btl_openib_component.verbose) {
        BTL_OUTPUT(("Set MTU to IBV value %d (%s bytes)", mtu,
                    (mtu == IBV_MTU_256) ? "256" :
                    (mtu == IBV_MTU_512) ? "512" :
                    (mtu == IBV_MTU_1024) ? "1024" :
                    (mtu == IBV_MTU_2048) ? "2048" :
                    (mtu == IBV_MTU_4096) ? "4096" :
                    "unknown (!)"));
    }
    OPAL_OUTPUT((-1, "ibm cm handler: connect qp set to IBV value %d (%s bytes)", mtu,
                (mtu == IBV_MTU_256) ? "256" :
                (mtu == IBV_MTU_512) ? "512" :
                (mtu == IBV_MTU_1024) ? "1024" :
                (mtu == IBV_MTU_2048) ? "2048" :
                (mtu == IBV_MTU_4096) ? "4096" :
                 "unknown (!)"));
    
    /* Move the QP into the INIT state */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    if (0 != ib_cm_init_qp_attr(cm_id, &attr, &attr_mask)) {
        BTL_ERROR(("error initializing IB CM qp attr INIT"));
        return OMPI_ERROR;
    }

    if (0 != ibv_modify_qp(qp, &attr, attr_mask)) {
        BTL_ERROR(("error modifying qp to INIT errno says %s", strerror(errno))); 
        return OMPI_ERROR;
    } 

    /* Move the QP into the RTR state */
    attr.qp_state = IBV_QPS_RTR;
    if (0 != ib_cm_init_qp_attr(cm_id, &attr, &attr_mask)) {
        BTL_ERROR(("error initializing IB CM qp attr RTR"));
        return OMPI_ERROR;
    }

    /* The IB CM API just told us a few more attributes about the
       remote side, so save these as well */
    endpoint->rem_info.rem_qps[qp_index].rem_qp_num = attr.dest_qp_num;

    /* Setup attributes */
    attr.path_mtu = mtu;
    attr.rq_psn = endpoint->qps[qp_index].qp->lcl_psn;
    OPAL_OUTPUT((-1, "ib cm qp connect: setting rq psn: %d", attr.rq_psn));
    /* IBM CM does not set these values for us */
    attr.max_dest_rd_atomic = mca_btl_openib_component.ib_max_rdma_dst_ops;
    attr.min_rnr_timer = mca_btl_openib_component.ib_min_rnr_timer;
    
    if (0 != ibv_modify_qp(qp, &attr,
                           attr_mask |
                           IBV_QP_PATH_MTU |
                           IBV_QP_MAX_DEST_RD_ATOMIC |
                           IBV_QP_MIN_RNR_TIMER
                           )) {
        BTL_ERROR(("error modifing QP to RTR errno says %s",
                   strerror(errno)));
        return OMPI_ERROR; 
    }
    
    /* All done */
    return OMPI_SUCCESS;
}

/*
 * Move the QP state to RTS
 */
static int qp_to_rts(int qp_index, struct ib_cm_id *cm_id,
                     mca_btl_openib_endpoint_t *endpoint)
{
    int attr_mask;
    struct ibv_qp_attr attr;
    struct ibv_qp *qp = endpoint->qps[qp_index].qp->lcl_qp;

    /* Setup attributes */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    if (0 != ib_cm_init_qp_attr(cm_id, &attr, &attr_mask)) {
        BTL_ERROR(("error initializing IB CM qp attr RTS"));
        return OMPI_ERROR;
    }
    if (0 != ibv_modify_qp(qp, &attr, attr_mask)) {
        BTL_ERROR(("error modifing QP (index %d) to RTS errno says %s",
                   qp_index, strerror(errno)));
        return OMPI_ERROR; 
    }
    
    /* All done */
    OPAL_OUTPUT((-1, "successfully set RTS"));
    return OMPI_SUCCESS;
}

/*
 * Callback (from main thread) when an incoming IBCM request needs to
 * initiate a new connection in the other direction.
 */
static void *callback_set_endpoint_connecting(void *context)
{
    mca_btl_openib_endpoint_t *endpoint =
        (mca_btl_openib_endpoint_t *) context;

    OPAL_OUTPUT((-1, "ibcm scheduled callback: setting endpoint to CONNECTING"));
    endpoint->endpoint_state = MCA_BTL_IB_CONNECTING;

    return NULL;
}

/*
 * Callback (from main thread) when an incoming IBCM request needs to
 * initiate a new connection in the other direction.
 */
static void *callback_start_connect(void *context)
{
    callback_start_connect_data_t *cbdata = 
        (callback_start_connect_data_t *) context;

    OPAL_OUTPUT((-1, "ibcm scheduled callback: calling start_connect()"));
    OPAL_OUTPUT((-1, "ibcm scheduled callback: cbdata %p",
                 (void*)cbdata));
    OPAL_OUTPUT((-1, "ibcm scheduled callback: endpoint %p",
                 (void*)cbdata->cscd_endpoint));
    OPAL_OUTPUT((-1, "ibcm scheduled callback: ie %p",
                 (void*)cbdata->cscd_endpoint->endpoint_local_cpc_data));
    OPAL_OUTPUT((-1, "ibcm scheduled callback: msg %p",
                 (void*)cbdata->cscd_endpoint->endpoint_remote_cpc_data->cbm_modex_message));
    ibcm_module_start_connect(cbdata->cscd_cpc, cbdata->cscd_endpoint);
    free(cbdata);

    return NULL;
}

/*
 * Passive has received a connection request from a active
 */
static int request_received(ibcm_listen_cm_id_t *cmh, 
                            struct ib_cm_event *event)
{
    int i, rc = OMPI_ERROR;
    mca_btl_openib_proc_t *ib_proc = NULL;
    mca_btl_openib_endpoint_t *endpoint = NULL;
    ibcm_endpoint_t *ie = NULL;
    struct ib_cm_req_event_param *req = &(event->param.req_rcvd);
    modex_msg_t *msg = NULL;
    bool found, do_initiate;
    ibcm_reject_reason_t rej_reason = REJ_MAX;
    ibcm_req_data_t *active_private_data =
        (ibcm_req_data_t*) event->private_data;
    int qp_index = active_private_data->ireqd_qp_index;
    opal_list_item_t *item;
    ibcm_module_list_item_t *imli;
    ibcm_module_t *m;
    ibcm_reply_t *rep;

    OPAL_OUTPUT((-1, "ibcm req handler: remote qp index %d, remote guid %lx, remote qkey %u, remote qpn %d, remote psn %d",
                qp_index,
                ntoh64(req->primary_path->dgid.global.interface_id),
                req->remote_qkey, req->remote_qpn,
                 req->starting_psn));

    /* Find the ibcm module for this request: remember that IB CM
       events come in per *device*, not per *port*.  So we just got a
       device event, and have to find the ibcm_module_t (i.e., local
       port/openib BTL module ) that corresponds to it. */
    OPAL_OUTPUT((-1, "looking for ibcm module -- source port guid: 0x%lx (%p)",
                ntoh64(req->primary_path->sgid.global.interface_id), 
                 (void*)cmh));
    for (item = opal_list_get_first(&(cmh->ibcm_modules));
         item != opal_list_get_end(&(cmh->ibcm_modules));
         item = opal_list_get_next(item)) {
        modex_msg_t *msg;
        imli = (ibcm_module_list_item_t*) item;
        m = imli->ibcm_module;
        msg = imli->ibcm_module->cpc.data.cbm_modex_message;
        OPAL_OUTPUT((-1, "comparing ibcm module port guid: 0x%lx",
                     msg->mm_port_guid));
        if (msg->mm_port_guid ==
            ntoh64(req->primary_path->sgid.global.interface_id)) {
            break;
        }
    }
    assert (item != opal_list_get_end(&(cmh->ibcm_modules)));

    /* Find the endpoint corresponding to the remote peer who is
       calling.  First, cycle through all the openib procs. */
    /* JMS: optimization target -- can we send something in private
       data to find the proc directly instead of having to search
       through *all* procs? */
    for (found = false, ib_proc = (mca_btl_openib_proc_t*)
             opal_list_get_first(&mca_btl_openib_component.ib_procs);
         !found && 
             ib_proc != (mca_btl_openib_proc_t*)
             opal_list_get_end(&mca_btl_openib_component.ib_procs);
         ib_proc  = (mca_btl_openib_proc_t*)opal_list_get_next(ib_proc)) {
        OPAL_OUTPUT((-1, "ibcm req: checking ib_proc %p", (void*)ib_proc));
        /* Now cycle through all the endpoints on that proc */
        for (i = 0; !found && i < (int) ib_proc->proc_endpoint_count; ++i) {
            OPAL_OUTPUT((-1, "ibcm req: checking endpoint %d of %d (ep %p, cpc data %p)",
                        i, (int) ib_proc->proc_endpoint_count, 
                        (void*)ib_proc->proc_endpoints[i],
                         (void*)ib_proc->proc_endpoints[i]->endpoint_remote_cpc_data));
            if (NULL == ib_proc->proc_endpoints[i]->endpoint_remote_cpc_data) {
                OPAL_OUTPUT((-1, "NULL remote cpc data!"));
            }
            msg = ib_proc->proc_endpoints[i]->endpoint_remote_cpc_data->cbm_modex_message;
            OPAL_OUTPUT((-1, "ibcm req: my guid 0x%lx, remote guid 0x%lx",
                        msg->mm_port_guid,
                         ntoh64(req->primary_path->dgid.global.interface_id)));
            OPAL_OUTPUT((-1, "ibcm req: my LID %d, remote LID %d",
                         msg->mm_lid,
                         ntohs(req->primary_path->dlid)));
            if (msg->mm_port_guid == 
                ntoh64(req->primary_path->dgid.global.interface_id) &&
                msg->mm_service_id == active_private_data->ireqd_pid &&
                msg->mm_port_num == req->port &&
                msg->mm_lid == htons(req->primary_path->dlid)) {
                OPAL_OUTPUT((-1, "*** found matching endpoint!!!"));
                endpoint = ib_proc->proc_endpoints[i];
                found = true;
            }
        }
    }
    if (!found) {
        OPAL_OUTPUT((-1, "ibcm req: could not find match for calling endpoint!"));
        rc = OMPI_ERR_NOT_FOUND;
        rej_reason = REJ_PEER_NOT_FOUND;
        goto reject;
    }
    OPAL_OUTPUT((-1, "ibcm req: Found endpoint %p", (void*)endpoint));

    /* Get our CPC-local data on the endpoint */
    ie = (ibcm_endpoint_t*) endpoint->endpoint_local_cpc_data;

    /* Check initiation direction (see comment above i_initiate()
       function).  If the connection comes from the "wrong" direction,
       then the remote peer expects it to be rejected, and further
       expects us to initiate it in the "right" direction. */
    do_initiate = i_initiate(m, endpoint);

    /* See if there is any activity happening on this endpoint
       already.  There's likely little reason to have fine-grained
       rejection reasons; we have them here just to help with
       debugging. */
    opal_mutex_lock(&ie->ie_lock);
    if (do_initiate) {
        OPAL_OUTPUT((-1, "ibcm request: request came from wrong direction"));
        rc = OMPI_SUCCESS;
        rej_reason = REJ_WRONG_DIRECTION;
    } else if (ie->ie_connection_flags & CFLAGS_COMPLETED) {
        OPAL_OUTPUT((-1, "ibcm request: all QPs already connected"));
        rej_reason = REJ_ALREADY_CONNECTED;
        rc = OMPI_SUCCESS;
    } else if (ie->ie_connection_flags & CFLAGS_ONGOING) {
        /* See if the request for this QP already arrived */
        if (ie->ie_qps_created && 
            IBV_QPS_RESET != endpoint->qps[qp_index].qp->lcl_qp->state) {
            OPAL_OUTPUT((-1, "ibcm request: this QP (%d) already connected",
                         qp_index));
            rej_reason = REJ_QP_ALREADY_CONNECTED;
            rc = OMPI_SUCCESS;
        }
    } else {
        /* this is the first activity -- accept */
        OPAL_OUTPUT((-1, "ibcm request: first initiation request"));
        ie->ie_connection_flags |= CFLAGS_ONGOING;
    }
    opal_mutex_unlock(&ie->ie_lock);

    /* If logic above selected a rejection reason, reject this
       request.  Note that if the same request arrives again later,
       IBCM will trigger a new event and we'll just reject it
       again. */
    if (REJ_MAX != rej_reason) {
        OPAL_OUTPUT((-1, "arbitrartion failed -- reject"));
        goto reject;
    }

    OPAL_OUTPUT((-1, "ibcm req handler: initiation arbitration successful -- proceeding"));

    /* If this is the first request we have received for this
       endpoint, then make *all* the QP's (because we analyze all the
       local QP attributes when making them; it's easier this way).
       If this is not the first request, then assume that all the QPs
       have been created already and we can just lookup what we need. */
    if (!ie->ie_qps_created) {
        /* Schedule to set the endpoint_state to "CONNECTING" */
        ompi_btl_openib_fd_schedule(callback_set_endpoint_connecting, 
                                    endpoint);
        if (OMPI_SUCCESS != (rc = qp_create_all(endpoint, m))) {
            rej_reason = REJ_PASSIVE_SIDE_ERROR;
            OPAL_OUTPUT((-1, "qp_create_all failed -- reject"));
            goto reject;
        }
        ie->ie_qps_created = true;
        OPAL_OUTPUT((-1, "ibcm request: created qp's"));
    }

    /* Save these numbers on the endpoint for reference.  Other values
       are filled in during qp_to_rtr (because we don't get them until
       we call ib_cm_attr_init()).  We already have the remote LID,
       subnet ID, and MTU from the port's modex message. */
    endpoint->rem_info.rem_qps[qp_index].rem_psn = 
        event->param.req_rcvd.starting_psn;
    endpoint->rem_info.rem_index = active_private_data->ireqd_ep_index;

    /* Connect this QP to the peer */
    if (OMPI_SUCCESS != (rc = qp_to_rtr(qp_index,
                                        event->cm_id, endpoint))) {
        OPAL_OUTPUT((-1, "ib cm req handler: failed to connect qp"));
        rej_reason = REJ_PASSIVE_SIDE_ERROR;
        goto reject;
    }

    /* Post receive buffers.  Similar to QP creation, above, we post
       *all* receive buffers at once (for all QPs).  So ensure to only
       do this for the first request received.  If this is not the
       first request on this endpoint, then assume that all the
       receive buffers have been posted already. */
    if (!ie->ie_recv_buffers_posted) {
        if (OMPI_SUCCESS != 
            (rc = mca_btl_openib_endpoint_post_recvs(endpoint))) {
            /* JMS */
            OPAL_OUTPUT((-1, "ib cm req handler: failed to post recv buffers"));
            rej_reason = REJ_PASSIVE_SIDE_ERROR;
            goto reject;
        }
        ie->ie_recv_buffers_posted = true;

        /* Further, create an array to cache all the active CM ID's so
           that they can be destroyed when the endpoint is
           destroyed */
        ie->ie_cm_id_cache_size = mca_btl_openib_component.num_qps;
        ie->ie_cm_id_cache = calloc(ie->ie_cm_id_cache_size,
                                    sizeof(ibcm_base_cm_id_t*));
        if (NULL == ie->ie_cm_id_cache) {
            rej_reason = REJ_PASSIVE_SIDE_ERROR;
            OPAL_OUTPUT((-1, "malloc failed -- reject"));
            goto reject;
        }
    }

    /* Save the CM ID on the endpoint for destruction later */
    ie->ie_cm_id_cache[qp_index] = OBJ_NEW(ibcm_base_cm_id_t);
    if (NULL == ie->ie_cm_id_cache[qp_index]) {
        OPAL_OUTPUT((-1, "ib cm req handler: malloc failed"));
        rej_reason = REJ_PASSIVE_SIDE_ERROR;
        goto reject;
    }
    ie->ie_cm_id_cache[qp_index]->cm_id = event->cm_id;

    /* Send reply */
    rep = OBJ_NEW(ibcm_reply_t);
    if (NULL == req) {
        rej_reason = REJ_PASSIVE_SIDE_ERROR;
        rc = OMPI_ERR_OUT_OF_RESOURCE;
        OPAL_OUTPUT((-1, "OBJ_NEW failed -- reject"));
        goto reject;
    }
    rep->super.cm_id = event->cm_id;
    rep->endpoint = endpoint;

    rep->cm_rep.qp_num = endpoint->qps[qp_index].qp->lcl_qp->qp_num;
    rep->cm_rep.srq = BTL_OPENIB_QP_TYPE_SRQ(qp_index);
    rep->cm_rep.starting_psn = endpoint->qps[qp_index].qp->lcl_psn;
    OPAL_OUTPUT((-1, "ib cm reply: setting reply psn %d\n", 
                 rep->cm_rep.starting_psn));
    rep->cm_rep.responder_resources = req->responder_resources;
    rep->cm_rep.initiator_depth = req->initiator_depth;
    rep->cm_rep.target_ack_delay = 20;
    rep->cm_rep.flow_control = req->flow_control;
    rep->cm_rep.rnr_retry_count = req->rnr_retry_count;
    
    rep->private_data.irepd_request = active_private_data->ireqd_request;
    rep->private_data.irepd_reply = rep;
    rep->private_data.irepd_qp_index = qp_index;
    rep->private_data.irepd_ep_index = endpoint->index;

    if (0 != (rc = ib_cm_send_rep(event->cm_id, &(rep->cm_rep)))) {
        /* JMS */
        OPAL_OUTPUT((-1, "ibcm req handler: failed to send reply for qp index %d",
                     qp_index));
        OBJ_RELEASE(rep);
        rej_reason = REJ_PASSIVE_SIDE_ERROR;
        OPAL_OUTPUT((-1, "failed to send request -- reject"));
        goto reject;
    }
    opal_list_append(&ibcm_pending_replies, &(rep->super.super));
    
    OPAL_OUTPUT((-1, "ibcm req handler: sent reply for qp index %d",
                 qp_index));
    return OMPI_SUCCESS;

 reject:
    /* Reject the request */
    OPAL_OUTPUT((-1, "rejecting request"));
    ib_cm_send_rej(event->cm_id, IB_CM_REJ_CONSUMER_DEFINED, 
                   &rej_reason, sizeof(rej_reason),
                   event->private_data, sizeof(ibcm_req_data_t));
    
    /* If we rejected because of the wrong direction, then initiate a
       connection going the other direction. */
    if (REJ_WRONG_DIRECTION == rej_reason) {
        callback_start_connect_data_t *cbdata = malloc(sizeof(*cbdata));
        if (NULL == cbdata) {
            return OMPI_ERR_OUT_OF_RESOURCE;
        }
        cbdata->cscd_cpc = (ompi_btl_openib_connect_base_module_t *) m;
        cbdata->cscd_endpoint = endpoint;
        ompi_btl_openib_fd_schedule(callback_start_connect, cbdata);
        
        return OMPI_SUCCESS;
    }
    return rc;
}
 
/*
 * Callback (from main thread) when the endpoint has been connected
 */
static void *callback_set_endpoint_connected(void *context)
{
    mca_btl_openib_endpoint_t *endpoint = (mca_btl_openib_endpoint_t*) context;

    OPAL_OUTPUT((-1, "ibcm: calling endpoint_connected"));
    mca_btl_openib_endpoint_connected(endpoint);
    OPAL_OUTPUT((-1, "ibcm: *** CONNECTED endpoint_connected done!"));

    return NULL;
}

/*
 * Helper function to find a cached CM ID in a list
 */ 
static ibcm_base_cm_id_t *find_cm_id(struct ib_cm_id *cm_id, 
                                     opal_list_t *list)
{
   opal_list_item_t *item;
   ibcm_base_cm_id_t *req;

   for (item = opal_list_get_first(&ibcm_pending_requests);
         item != opal_list_get_end(&ibcm_pending_requests);
         item = opal_list_get_next(item)) {
        req = (ibcm_base_cm_id_t*) item;
        if (req->cm_id == cm_id) {
            return req;
        }
    }

   return NULL;
}

/*
 * Active has received the reply from the passive.
 */
static int reply_received(ibcm_listen_cm_id_t *cmh, struct ib_cm_event *event)
{
    int rc;
    ibcm_rep_data_t *p = (ibcm_rep_data_t*) event->private_data;
    ibcm_request_t *request = p->irepd_request;
    ibcm_reply_t *reply = p->irepd_reply;
    mca_btl_openib_endpoint_t *endpoint = request->endpoint;
    ibcm_endpoint_t *ie;
    ibcm_rtu_data_t rtu_data;

    OPAL_OUTPUT((-1, "ibcm handler: got reply! (qp index %d) endpoint: %p",
                 p->irepd_qp_index, (void*) endpoint));

    ie = (ibcm_endpoint_t*) endpoint->endpoint_local_cpc_data;
    endpoint->rem_info.rem_qps[p->irepd_qp_index].rem_psn = 
        event->param.rep_rcvd.starting_psn;
    endpoint->rem_info.rem_index = p->irepd_ep_index;

    /* Move the QP to RTR and RTS */
    if (OMPI_SUCCESS != (rc = qp_to_rtr(p->irepd_qp_index,
                                        event->cm_id, endpoint))) {
        OPAL_OUTPUT((-1, "ib cm req handler: failed move to RTR"));
        return rc;
    }

    if (OMPI_SUCCESS != (rc = qp_to_rts(p->irepd_qp_index,
                                        event->cm_id, endpoint))) {
        OPAL_OUTPUT((-1, "ib cm req handler: failed move to RTS"));
        return rc;
    }

    /* Now that all the qp's are created locally, post some receive
       buffers, setup credits, etc.  The openib posts the buffers for
       all QPs at once, so be sure to only do this for the *first*
       reply that is received on an endpoint.  For all other replies
       received on an endpoint, we can safely assume that the receive
       buffers have already been posted. */
    if (!ie->ie_recv_buffers_posted) {
        if (OMPI_SUCCESS != 
            (rc = mca_btl_openib_endpoint_post_recvs(endpoint))) {
            OPAL_OUTPUT((-1, "ib cm: failed to post recv buffers"));
            return rc;
        }
        ie->ie_recv_buffers_posted = true;
    }

    /* Send the RTU */
    rtu_data.irtud_reply = reply;
    rtu_data.irtud_qp_index = p->irepd_qp_index;
    if (0 != ib_cm_send_rtu(event->cm_id, &rtu_data, sizeof(rtu_data))) {
        OPAL_OUTPUT((-1, "ib cm rep handler: failed to send RTU"));
        return OMPI_ERR_IN_ERRNO;
    }

    /* Remove the pending request because we won't need to handle
       errors for it */
    OPAL_OUTPUT((-1, "reply received cm id %p -- original cached req %p",
                 (void*)cmh->listen_cm_id, (void*)request));
    opal_list_remove_item(&ibcm_pending_requests, &(request->super.super));
    OBJ_RELEASE(request);

    /* Have all the QP's been connected?  If so, tell the main BTL
       that we're done. */
    if (0 == --(ie->ie_qps_to_connect)) {
        OPAL_OUTPUT((-1, "ib cm rep handler: REPLY telling main BTL we're connected"));
        ompi_btl_openib_fd_schedule(callback_set_endpoint_connected, endpoint);
    }

    return OMPI_SUCCESS;
}

/*
 * Passive has received "ready to use" from the active
 */
static int ready_to_use_received(ibcm_listen_cm_id_t *h,
                                 struct ib_cm_event *event)
{
    int rc;
    ibcm_rtu_data_t *p = (ibcm_rtu_data_t*) event->private_data;
    ibcm_reply_t *reply = p->irtud_reply;
    mca_btl_openib_endpoint_t *endpoint = reply->endpoint;
    ibcm_endpoint_t *ie = (ibcm_endpoint_t*) endpoint->endpoint_local_cpc_data;

    OPAL_OUTPUT((-1, "ibcm handler: got RTU! (index %d)", p->irtud_qp_index));

    /* Move the QP to RTS */
    if (OMPI_SUCCESS != (rc = qp_to_rts(p->irtud_qp_index,
                                        event->cm_id, endpoint))) {
        OPAL_OUTPUT((-1, "ib cm rtu handler: failed move to RTS (index %d)",
                     p->irtud_qp_index));
        return rc;
    }

    /* Remove the pending reply because we won't need to handle errors
       for it */
    OPAL_OUTPUT((-1, "RTU received cm id %p -- original cached reply %p",
                 (void*)event->cm_id, (void*)reply));
    opal_list_remove_item(&ibcm_pending_replies, &(reply->super.super));
    OBJ_RELEASE(reply);

    /* Have all the QP's been connected?  If so, tell the main BTL
       that we're done. */
    if (0 == --(ie->ie_qps_to_connect)) {
        OPAL_OUTPUT((-1, "ib cm rtu handler: RTU telling main BTL we're connected"));
        ompi_btl_openib_fd_schedule(callback_set_endpoint_connected, endpoint);
    }

    OPAL_OUTPUT((-1, "ib cm rtu handler: all done"));
    return OMPI_SUCCESS;
}


static int disconnect_request_received(ibcm_listen_cm_id_t *cmh,
                                        struct ib_cm_event *event)
{
    OPAL_OUTPUT((-1, "ibcm handler: disconnect request received"));
    return OMPI_SUCCESS;
}


static int disconnect_reply_received(ibcm_listen_cm_id_t *cmd,
                                      struct ib_cm_event *event)
{
    OPAL_OUTPUT((-1, "ibcm handler: disconnect reply received"));
#if 0
    ib_cm_send_drep(event->cm_id, NULL, 0);
#endif
    return OMPI_SUCCESS;
}


static int reject_received(ibcm_listen_cm_id_t *cmh, struct ib_cm_event *event)
{
    enum ib_cm_rej_reason reason = event->param.rej_rcvd.reason;
    ibcm_reject_reason_t *rej_reason = 
        (ibcm_reject_reason_t *) event->param.rej_rcvd.ari;

    OPAL_OUTPUT((-1, "ibcm handler: reject received: reason %d, official reason: %d",
                 reason, *rej_reason));

    /* Determine if we expected this reject or not */

    if (IB_CM_REJ_CONSUMER_DEFINED == reason &&
        REJ_WRONG_DIRECTION == *rej_reason) {
        ibcm_req_data_t *my_private_data =
            (ibcm_req_data_t*) event->private_data;
        ibcm_request_t *request = my_private_data->ireqd_request;
        mca_btl_openib_endpoint_t *endpoint = request->endpoint;
        ibcm_endpoint_t *ie = (ibcm_endpoint_t*) 
            endpoint->endpoint_local_cpc_data;

        OPAL_OUTPUT((-1, "ibcm rej handler: got WRONG_DIRECTION reject, endpoint: %p, pid %d, ep_index %d, qp_index %d",
                    (void*)my_private_data->ireqd_request->endpoint,
                    my_private_data->ireqd_pid,
                    my_private_data->ireqd_ep_index,
                     my_private_data->ireqd_qp_index));
        if (NULL == ie->ie_bogus_qp) {
            OPAL_OUTPUT((-1, "ibcm rej handler: WRONG_DIRECTION unexpected!"));
        } else {

            /* Remove from the global pending_requests list because we
               no longer need to handle errors for it */
            OPAL_OUTPUT((-1, "reply received cm id %p -- original cached req %p",
                        (void*)cmh->listen_cm_id, 
                         (void*)request));
            opal_list_remove_item(&ibcm_pending_requests, 
                                  &(request->super.super));

            /* We ack the event and then destroy the CM ID (you *must*
               ACK it first -- the destroy will block until all
               outstand events on this ID are complete) */
            OPAL_OUTPUT((-1, "ibcm rej handler: destroying bogus CM ID: %p",
                         (void*)request->super.cm_id));
            ib_cm_ack_event(event);
            ib_cm_destroy_id(request->super.cm_id);

            /* Destroy the QP */
            OPAL_OUTPUT((-1, "ibcm rej handler: destroying bogus qp"));
            ibv_destroy_qp(ie->ie_bogus_qp);
            ie->ie_bogus_qp = NULL;

            /* Free the object */
            OBJ_RELEASE(request);
        }

        return OMPI_SUCCESS;
    }

    OPAL_OUTPUT((-1, "ibcm rej handler: got unexpected reject type: %d",
                 reason));
    return OMPI_ERR_NOT_FOUND;
}

static int request_error(ibcm_listen_cm_id_t *cmh, struct ib_cm_event *event)
{
    ibcm_request_t *req;
    OPAL_OUTPUT((-1, "ibcm handler: request error!"));

    if (IBV_WC_RESP_TIMEOUT_ERR != event->param.send_status) {
        orte_show_help("help-mpi-btl-openib-cpc-ibcm.txt",
                       "unhandled error", true,
                       "request", orte_process_info.nodename, 
                       event->param.send_status);
        return OMPI_ERROR;
    }

    OPAL_OUTPUT((-1, "Got timeout in IBCM request (CM ID: %p)", 
                 (void*)event->cm_id));
    req = (ibcm_request_t*) find_cm_id(event->cm_id, 
                                       &ibcm_pending_requests);
    if (NULL == req) {
        orte_show_help("help-mpi-btl-openib-cpc-ibcm.txt",
                       "timeout not found", true,
                       "request", orte_process_info.nodename);
        return OMPI_ERR_NOT_FOUND;
    }

    /* JMS need to barf this connection request appropriately */
    return OMPI_SUCCESS;
}


static int reply_error(ibcm_listen_cm_id_t *cmh, struct ib_cm_event *event)
{
    ibcm_reply_t *rep;
    OPAL_OUTPUT((-1, "ibcm handler: reply error!"));

    if (IBV_WC_RESP_TIMEOUT_ERR != event->param.send_status) {
        orte_show_help("help-mpi-btl-openib-cpc-ibcm.txt",
                       "unhandled error", true,
                       "reply", orte_process_info.nodename, 
                       event->param.send_status);
        return OMPI_ERROR;
    }

    OPAL_OUTPUT((-1, "Got timeout in IBCM reply (id: %p) -- aborting because resend is not written yet...",
                 (void*)event->cm_id));
    rep = (ibcm_reply_t*) find_cm_id(event->cm_id, 
                                     &ibcm_pending_replies);
    if (NULL == rep) {
        orte_show_help("help-mpi-btl-openib-cpc-ibcm.txt",
                       "timeout not found", true,
                       "reply", orte_process_info.nodename);
        return OMPI_ERR_NOT_FOUND;
    }

    /* JMS need to barf this connection request appropriately */
    return OMPI_SUCCESS;
}


static int disconnect_request_error(ibcm_listen_cm_id_t *cmh,
                                    struct ib_cm_event *e)
{
    OPAL_OUTPUT((-1, "ibcm handler: disconnect request error!"));
    return OMPI_SUCCESS;
}


static int unhandled_event(ibcm_listen_cm_id_t *cmh, struct ib_cm_event *e)
{
    OPAL_OUTPUT((-1, "ibcm handler: unhandled event error (%p, %d)", 
                 (void*) e, e->event));
    return OMPI_ERR_NOT_FOUND;
}


static void *ibcm_event_dispatch(int fd, int flags, void *context)
{
    bool want_ack;
    int rc;
    ibcm_listen_cm_id_t *cmh = (ibcm_listen_cm_id_t*) context;
    struct ib_cm_event *e = NULL;

    rc = ib_cm_get_event(cmh->cm_device, &e);
    if (0 == rc && NULL != e) {
        want_ack = true;
        switch (e->event) {
        case IB_CM_REQ_RECEIVED:
            /* Incoming request */
            rc = request_received(cmh, e);
            break;
            
        case IB_CM_REP_RECEIVED:
            /* Reply received */
            rc = reply_received(cmh, e);
            break;
            
        case IB_CM_RTU_RECEIVED:
            /* Ready to use! */
            rc = ready_to_use_received(cmh, e);
            break;
            
        case IB_CM_DREQ_RECEIVED:
            /* Disconnect request */
            rc = disconnect_request_received(cmh, e);
            break;
            
        case IB_CM_DREP_RECEIVED:
            /* Disconnect reply */
            rc = disconnect_reply_received(cmh, e);
            break;
            
        case IB_CM_REJ_RECEIVED:
            /* Rejected connection */
            rc = reject_received(cmh, e);
            /* reject_received() called ib_cm_ack_event so that the CM
               ID could be freed */
            want_ack = false;
            break;
            
        case IB_CM_REQ_ERROR:
            /* Request error */
            rc = request_error(cmh, e);
            break;
            
        case IB_CM_REP_ERROR:
            /* Reply error */
            rc = reply_error(cmh, e);
            break;
            
        case IB_CM_DREQ_ERROR:
            /* Disconnect request error */
            rc = disconnect_request_error(cmh, e);
            break;
            
        default:
            rc = unhandled_event(cmh, e);
            break;
        }

        if (want_ack) {
            ib_cm_ack_event(e);
        }

        if (OMPI_SUCCESS != rc) {
            OPAL_OUTPUT((-1, "An error occurred handling an IBCM event.  Bad things are likely to happen."));
        }
    }

    return NULL;
}
