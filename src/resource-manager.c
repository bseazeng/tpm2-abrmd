/*
 * Copyright (c) 2017, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <glib.h>

#include <tss2/tss2_mu.h>

#include "connection.h"
#include "connection-manager.h"
#include "control-message.h"
#include "logging.h"
#include "message-queue.h"
#include "resource-manager.h"
#include "sink-interface.h"
#include "source-interface.h"
#include "tabrmd.h"
#include "tpm2-header.h"
#include "tpm2-command.h"
#include "tpm2-response.h"
#include "util.h"

#define MAX_ABANDONED 4

static void resource_manager_sink_interface_init   (gpointer g_iface);
static void resource_manager_source_interface_init (gpointer g_iface);

G_DEFINE_TYPE_WITH_CODE (
    ResourceManager,
    resource_manager,
    TYPE_THREAD,
    G_IMPLEMENT_INTERFACE (TYPE_SINK,
                           resource_manager_sink_interface_init);
    G_IMPLEMENT_INTERFACE (TYPE_SOURCE,
                           resource_manager_source_interface_init);
    );


enum {
    PROP_0,
    PROP_QUEUE_IN,
    PROP_SINK,
    PROP_ACCESS_BROKER,
    PROP_SESSION_LIST,
    N_PROPERTIES
};
static GParamSpec *obj_properties [N_PROPERTIES] = { NULL, };
/*
 * This is a helper function that does everything required to convert
 * a virtual handle to a physical one in a Tpm2Command object.
 * - load the context from the provided HandleMapEntry
 * - store the newly assigned TPM handle (physical handle) in the entry
 * - set this handle in the comamnd at the position indicated by
 *   'handle_number' (0-based index)
 */
TSS2_RC
resource_manager_virt_to_phys (ResourceManager *resmgr,
                               Tpm2Command     *command,
                               HandleMapEntry  *entry,
                               guint8           handle_number)
{
    TPM2_HANDLE    phandle = 0;
    TPMS_CONTEXT *context;
    TSS2_RC       rc = TSS2_RC_SUCCESS;

    context = handle_map_entry_get_context (entry);
    rc = access_broker_context_load (resmgr->access_broker, context, &phandle);
    g_debug ("phandle: 0x%" PRIx32, phandle);
    if (rc == TSS2_RC_SUCCESS) {
        handle_map_entry_set_phandle (entry, phandle);
        tpm2_command_set_handle (command, phandle, handle_number);
    } else {
        g_warning ("Failed to load context: 0x%" PRIx32, rc);
    }
    return rc;
}
TSS2_RC
resource_manager_load_transient (ResourceManager  *resmgr,
                                 Tpm2Command      *command,
                                 GSList          **entry_slist,
                                 TPM2_HANDLE        handle,
                                 guint8            handle_index)
{
    HandleMap    *map;
    HandleMapEntry *entry;
    Connection  *connection;
    TSS2_RC       rc = TSS2_RC_SUCCESS;

    g_debug ("processing TPM2_HT_TRANSIENT: 0x%" PRIx32, handle);
    connection = tpm2_command_get_connection (command);
    map = connection_get_trans_map (connection);
    g_object_unref (connection);
    g_debug ("handle 0x%" PRIx32 " is virtual TPM2_HT_TRANSIENT, "
             "loading", handle);
    /* we don't unref the entry since we're adding it to the entry_slist below */
    entry = handle_map_vlookup (map, handle);
    if (entry) {
        g_debug ("mapped virtual handle 0x%" PRIx32 " to entry 0x%"
                 PRIxPTR, handle, (uintptr_t)entry);
    } else {
        g_warning ("No HandleMapEntry for vhandle: 0x%" PRIx32, handle);
        goto out;
    }
    rc = resource_manager_virt_to_phys (resmgr, command, entry, handle_index);
    if (rc != TSS2_RC_SUCCESS) {
        goto out;
    }
    *entry_slist = g_slist_prepend (*entry_slist, entry);
out:
    g_object_unref (map);
    return rc;
}
/*
 * This is a somewhat generic function used to load session contexts into
 * the TPM2 device. The ResourceManager uses this function when loading
 * sessions in the handle or auth area of a command. It will refuse to load
 * sessions that:
 * - aren't tracked by the ResourceManager (must have SessionEntry in
 *   SessionList)
 * - that were last saved by the client instead of the RM
 * - aren't owned by the Connection object associated with the Tpm2Command
 */
TSS2_RC
resource_manager_load_session_from_handle (ResourceManager *resmgr,
                                           Connection      *command_conn,
                                           TPM2_HANDLE       handle,
                                           gboolean         will_flush)
{
    Connection   *entry_conn = NULL;
    SessionEntry *session_entry = NULL;
    Tpm2Command *command = NULL;
    Tpm2Response *response = NULL;
    SessionEntryStateEnum session_entry_state;
    TSS2_RC        rc = TSS2_RC_SUCCESS;
    size_buf_t    *size_buf;

    session_entry = session_list_lookup_handle (resmgr->session_list,
                                                handle);
    if (session_entry == NULL) {
        g_debug ("no session with handle 0x%08" PRIx32 " known to "
                 "ResourceManager.", handle);
        goto out;
    }
    g_debug ("mapped session handle 0x%08" PRIx32 " to "
             "SessionEntry: 0x%" PRIxPTR, handle,
             (uintptr_t)session_entry);
    session_entry_prettyprint (session_entry);
    entry_conn = session_entry_get_connection (session_entry);
    if (command_conn != entry_conn) {
        g_warning ("%s: Connection from Tpm2Command and SessionEntry do not "
                   "match. Refusing to load.", __func__);
        goto out;
    }
    session_entry_state = session_entry_get_state (session_entry);
    if (session_entry_state != SESSION_ENTRY_SAVED_RM) {
        g_warning ("%s: Handle in handle area references SessionEntry 0x%"
                   PRIxPTR " for session in state \"%s\". Must be in state: "
                   "SESSION_ENTRY_SAVED_RM for us manage it, ignorning.",
                   __func__, (uintptr_t)session_entry,
                   session_entry_state_to_str (session_entry_state));
        goto out;
    }
    size_buf = session_entry_get_context (session_entry);
    command = tpm2_command_new_context_load (size_buf->buf, size_buf->size);
    response = access_broker_send_command (resmgr->access_broker,
                                           command,
                                           &rc);
    if (rc != TSS2_RC_SUCCESS) {
        g_warning ("Failed to load context for session with handle "
                   "0x%08" PRIx32 " RC: 0x%" PRIx32, handle, rc);
        session_list_remove (resmgr->session_list, session_entry);
        goto out;

    }
    g_debug ("%s: successfully loaded context for session handle: 0x%08"
             PRIx32, __func__, handle);
    session_entry_set_state (session_entry, SESSION_ENTRY_LOADED);
    if (will_flush) {
        session_list_remove (resmgr->session_list, session_entry);
    }
out:
    g_clear_object (&command);
    g_clear_object (&entry_conn);
    g_clear_object (&response);
    g_clear_object (&session_entry);

    return rc;
}
typedef struct {
    ResourceManager *resmgr;
    Tpm2Command     *command;
} auth_callback_data_t;
void
resource_manager_load_auth_callback (gpointer auth_offset_ptr,
                                     gpointer user_data)
{
    Connection *connection = NULL;
    TPM2_HANDLE handle;
    auth_callback_data_t *data = (auth_callback_data_t*)user_data;
    TPMA_SESSION attrs;
    gboolean will_flush = TRUE;
    size_t auth_offset = *(size_t*)auth_offset_ptr;

    handle = tpm2_command_get_auth_handle (data->command, auth_offset);
    switch (handle >> TPM2_HR_SHIFT) {
    case TPM2_HT_HMAC_SESSION:
    case TPM2_HT_POLICY_SESSION:
        attrs = tpm2_command_get_auth_attrs (data->command, auth_offset);
        if (attrs & TPMA_SESSION_CONTINUESESSION) {
            will_flush = FALSE;
        }
        connection = tpm2_command_get_connection (data->command);
        resource_manager_load_session_from_handle (data->resmgr,
                                                   connection,
                                                   handle,
                                                   will_flush);
        break;
    default:
        g_debug ("not loading object with handle: 0x%08" PRIx32 " from "
                 "command auth area: not a session", handle);
        break;
    }
    g_clear_object (&connection);
}
/*
 * This function operates on the provided command. It iterates over each
 * handle in the commands handle area. For each relevant handle it loads
 * the related context and fixes up the handles in the command.
 */
TSS2_RC
resource_manager_load_handles (ResourceManager *resmgr,
                               Tpm2Command     *command,
                               GSList         **loaded_transients)
{
    Connection *connection = NULL;
    TSS2_RC       rc = TSS2_RC_SUCCESS;
    TPM2_HANDLE    handles[TPM2_COMMAND_MAX_HANDLES] = { 0, };
    size_t        i, handle_count = TPM2_COMMAND_MAX_HANDLES;
    gboolean      handle_ret;

    g_debug ("%s", __func__);
    if (!resmgr || !command) {
        g_warning ("%s: received NULL parameter.", __func__);
        return RM_RC (TSS2_BASE_RC_GENERAL_FAILURE);
    }
    handle_ret = tpm2_command_get_handles (command, handles, &handle_count);
    if (handle_ret == FALSE) {
        g_error ("Unable to get handles from command 0x%" PRIxPTR,
                 (uintptr_t)command);
    }
    g_debug ("%s: for %zu handles in command handle area",
             __func__, handle_count);
    for (i = 0; i < handle_count; ++i) {
        switch (handles [i] >> TPM2_HR_SHIFT) {
        case TPM2_HT_TRANSIENT:
            g_debug ("processing TPM2_HT_TRANSIENT: 0x%" PRIx32, handles [i]);
            rc = resource_manager_load_transient (resmgr,
                                                  command,
                                                  loaded_transients,
                                                  handles [i],
                                                  i);
            break;
        case TPM2_HT_HMAC_SESSION:
        case TPM2_HT_POLICY_SESSION:
            g_debug ("processing TPM2_HT_HMAC_SESSION or "
                     "TPM2_HT_POLICY_SESSION: 0x%" PRIx32, handles [i]);
            connection = tpm2_command_get_connection (command);
            rc = resource_manager_load_session_from_handle (resmgr,
                                                            connection,
                                                            handles [i],
                                                            FALSE);
            break;
        default:
            break;
        }
    }
    g_debug ("%s: end", __func__);
    g_clear_object (&connection);

    return rc;
}
/*
 * Remove the context associated with the provided HandleMapEntry
 * from the TPM. Only handles in the TRANSIENT range will be flushed.
 * Any entry with a context that's flushed will have the physical handle
 * to 0.
 */
void
resource_manager_flushsave_context (gpointer data_entry,
                                    gpointer data_resmgr)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (data_resmgr);
    HandleMapEntry  *entry  = HANDLE_MAP_ENTRY (data_entry);
    TPMS_CONTEXT   *context;
    TPM2_HANDLE      phandle;
    TSS2_RC         rc = TSS2_RC_SUCCESS;

    g_debug ("resource_manager_flushsave_context for entry: 0x%" PRIxPTR,
             (uintptr_t)entry);
    if (resmgr == NULL || entry == NULL)
        g_error ("resource_manager_flushsave_context passed NULL parameter");
    phandle = handle_map_entry_get_phandle (entry);
    g_debug ("resource_manager_save_context phandle: 0x%" PRIx32, phandle);
    switch (phandle >> TPM2_HR_SHIFT) {
    case TPM2_HT_TRANSIENT:
        g_debug ("handle is transient, saving context");
        context = handle_map_entry_get_context (entry);
        rc = access_broker_context_saveflush (resmgr->access_broker,
                                              phandle,
                                              context);
        if (rc == TSS2_RC_SUCCESS) {
            handle_map_entry_set_phandle (entry, 0);
        } else {
            g_warning ("access_broker_context_save failed for handle: 0x%"
                       PRIx32 " rc: 0x%" PRIx32, phandle, rc);
        }
        break;
    default:
        break;
    }
}
/*
 * Remove the context associated with the provided SessionEntry from the
 * TPM. Only session objects should be saved by this function.
 */
void
resource_manager_save_session_context (gpointer data_entry,
                                       gpointer data_resmgr)
{
    Tpm2Command *cmd = NULL;
    Tpm2Response *resp = NULL;
    ResourceManager *resmgr = RESOURCE_MANAGER (data_resmgr);
    SessionEntry    *entry  = SESSION_ENTRY (data_entry);
    TSS2_RC          rc = TSS2_RC_SUCCESS;

    g_debug ("resource_manager_save_session_context");
    if (resmgr == NULL || entry == NULL) {
        g_error ("resource_manager_save_session_context passed NULL parameter");
    }
    if (session_entry_get_state (entry) != SESSION_ENTRY_LOADED) {
        g_info ("SessionEntry not loaded in TPM, skipping");
        goto out;
    }
    cmd = tpm2_command_new_context_save (session_entry_get_handle (entry));
    if (cmd == NULL) {
        goto err_out;
    }
    resp = access_broker_send_command (resmgr->access_broker, cmd, &rc);
    if (rc != TSS2_RC_SUCCESS) {
        goto err_out;
    }
    session_entry_set_context (entry,
                               &tpm2_response_get_buffer (resp)[TPM_HEADER_SIZE],
                               tpm2_response_get_size (resp) - TPM_HEADER_SIZE);
    session_entry_set_state (entry, SESSION_ENTRY_SAVED_RM);
    goto out;
err_out:
    access_broker_context_flush (resmgr->access_broker,
                                 session_entry_get_handle (entry));
    session_list_remove (resmgr->session_list, entry);
out:
    g_clear_object (&cmd);
    g_clear_object (&resp);
}
static void
dump_command (Tpm2Command *command)
{
    g_assert (command != NULL);
    g_debug ("Tpm2Command: 0x%" PRIxPTR, (uintptr_t)command);
    g_debug_bytes (tpm2_command_get_buffer (command),
                   tpm2_command_get_size (command),
                   16,
                   4);
    g_debug_tpma_cc (tpm2_command_get_attributes (command));
}
static void
dump_response (Tpm2Response *response)
{
    g_assert (response != NULL);
    g_debug ("Tpm2Response: 0x%" PRIxPTR, (uintptr_t)response);
    g_debug_bytes (tpm2_response_get_buffer (response),
                   tpm2_response_get_size (response),
                   16,
                   4);
    g_debug_tpma_cc (tpm2_response_get_attributes (response));
}
/*
 * This function performs the special processing required when a client
 * attempts to save a session context. In short: handling the context gap /
 * contextCounter roll over is the only reason we have to get involved. To do
 * this we must be able to load and save every active session from oldest to
 * newest. This is discussed in detail in section 30.5 from part 1 of the TPM2
 * spec.
 *
 * The recommended algorithm for doing this is documented in one of the TSS2 specs.
 */
Tpm2Response*
resource_manager_save_context_session (ResourceManager *resmgr,
                                       Tpm2Command *command)
{
    Connection *conn_cmd = NULL, *conn_entry = NULL;
    SessionEntry *entry = NULL;
    Tpm2Response *response = NULL;
    TPM2_HANDLE handle = 0;

    handle = tpm2_command_get_handle (command, 0);
    g_debug ("save_context for session handle: 0x%" PRIx32, handle);
    entry = session_list_lookup_handle (resmgr->session_list, handle);
    if (entry == NULL) {
        g_warning ("Client attempting to save unknown session.");
        goto out;
    }
    /* the lookup function should check this for us? */
    conn_cmd = tpm2_command_get_connection (command);
    conn_entry = session_entry_get_connection (entry);
    if (conn_cmd != conn_entry) {
        g_warning ("%s: session belongs to a different connection", __func__);
        goto out;
    }
    session_entry_set_state (entry, SESSION_ENTRY_SAVED_CLIENT);
    response = tpm2_response_new_context_save (conn_cmd, entry);
    g_debug ("%s: Tpm2Response 0x%" PRIxPTR " in reponse to TPM2_ContextSave",
             __func__, (uintptr_t)response);
    g_debug_bytes (tpm2_response_get_buffer (response),
                   tpm2_response_get_size (response),
                   16, 4);
out:
    g_clear_object (&conn_cmd);
    g_clear_object (&conn_entry);
    g_clear_object (&entry);
    return response;
}
/*
 * This function performs the special processing associated with the
 * TPM2_ContextSave command. How much we can "virtualize of this command
 * depends on the parameters / handle type as well as how much work we
 * actually *want* to do.
 *
 * Transient objects that are tracked by the RM require no special handling.
 * It's possible that the whole of this command could be virtualized: When a
 * command is received all transient objects have been saved and flushed. The
 * saved context held by the RM could very well be returned to the caller
 * with no interaction with the TPM. This would however require that the RM
 * marshal the context object into the response buffer. This is less easy
 * than it should be and so we suffer the inefficiency to keep the RM code
 * more simple. Simple in this case means no special handling.
 *
 * Session objects are handled much in the same way with a specific caveat:
 * A session can be either loaded or saved. Unlike a transient object saving
 * it changes its state. And so for the caller to save the context it must
 * be loaded first. This is exactly what we do for transient objects, but
 * knowing that the caller wants to save the context we also know that the RM
 * can't have a saved copy as well. And so we must load the context and
 * destroy the mapping maintained by the RM. Since this function is called
 * after the session contexts are loaded we just need to drop the mapping.
 */
Tpm2Response*
resource_manager_save_context (ResourceManager *resmgr,
                               Tpm2Command     *command)
{
    TPM2_HANDLE handle = tpm2_command_get_handle (command, 0);

    g_debug ("resource_manager_save_context: resmgr: 0x%" PRIxPTR
             " command: 0x%" PRIxPTR, (uintptr_t)resmgr, (uintptr_t)command);
    switch (handle >> TPM2_HR_SHIFT) {
    case TPM2_HT_HMAC_SESSION:
    case TPM2_HT_POLICY_SESSION:
        return resource_manager_save_context_session (resmgr, command);
    default:
        g_debug ("save_context: not virtualizing TPM2_CC_ContextSave for "
                 "handles: 0x%08" PRIx32, handle);
        break;
    }

    return NULL;
}
/*
 * This function performs the special processing required when handling a
 * TPM2_CC_ContextLoad command:
 * - First we look at the TPMS_CONTEXT provided in the command body. If we've
 *   never see the context then we do nothing and let the command be processed
 *   in its current form. If the context is valid and the TPM loads it the
 *   ResourceManager will intercept the response and begin tracking the
 *   session.
 * - Second we check to be sure the session can be loaded by the connection
 *   over which we received the command. This requires that it's either:
 *   - the same connection associated with the SessionEntry
 *   or
 *   - that the SessionEntry has been abandoned
 * - If the access control check pacsses we return the handle to the caller
 *   in a Tpm2Response that we craft.
 */
Tpm2Response*
resource_manager_load_context_session (ResourceManager *resmgr,
                                       Tpm2Command *command)
{
    Connection *conn_cmd = NULL, *conn_entry = NULL;
    SessionEntry *entry = NULL;
    Tpm2Response *response = NULL;

    g_debug ("%s: ResourceManager: 0x%" PRIxPTR ", Tpm2Command: 0x%" PRIxPTR,
             __func__, (uintptr_t)resmgr, (uintptr_t)command);

    entry = session_list_lookup_context_client (resmgr->session_list,
                                                &tpm2_command_get_buffer (command) [TPM_HEADER_SIZE],
                                                tpm2_command_get_size (command) - TPM_HEADER_SIZE);
    if (entry == NULL) {
        g_debug ("%s: Tpm2Command 0x%" PRIxPTR " contains unknown "
                 "TPMS_CONTEXT.", __func__, (uintptr_t)command);
        goto out;
    }
    conn_cmd = tpm2_command_get_connection (command);
    conn_entry = session_entry_get_connection (entry);
    if (conn_cmd != conn_entry) {
        if (!session_list_claim (resmgr->session_list, entry, conn_cmd)) {
            g_debug ("%s: Connection 0x%" PRIxPTR " attempting to load context"
                     " belonging to Connection 0x%" PRIxPTR, __func__,
                     (uintptr_t)conn_cmd, (uintptr_t)conn_entry);
            goto out;
        }
    }
    session_entry_set_state (entry, SESSION_ENTRY_SAVED_RM);
    g_debug ("%s: SessionEntry context savedHandle: 0x%08" PRIx32, __func__,
             session_entry_get_handle (entry));
    response = tpm2_response_new_context_load (conn_cmd, entry);
out:
    g_debug ("%s: returning Tpm2Response 0x%" PRIxPTR,
             __func__, (uintptr_t)response);
    g_clear_object (&conn_cmd);
    g_clear_object (&conn_entry);
    g_clear_object (&entry);
    return response;
}
/*
 * This function performs the special processing associated with the
 * TPM2_ContextLoad command.
 */
Tpm2Response*
resource_manager_load_context (ResourceManager *resmgr,
                               Tpm2Command *command)
{
    /* why all the marshalling */
    uint8_t *buf = tpm2_command_get_buffer (command);
    TPMS_CONTEXT tpms_context;
    TSS2_RC rc;
    size_t offset = TPM_HEADER_SIZE;

    g_debug ("%s: resmgr: 0x%" PRIxPTR " command: 0x%" PRIxPTR,
             __func__, (uintptr_t)resmgr, (uintptr_t)command);
    /* Need to be able to get handle from Tpm2Command */
    rc = Tss2_MU_TPMS_CONTEXT_Unmarshal (buf,
                                         tpm2_command_get_size (command),
                                         &offset,
                                         &tpms_context);
    if (rc != TSS2_RC_SUCCESS) {
        g_warning ("%s: Failed to unmarshal TPMS_CONTEXT from Tpm2Command "
                   "0x%" PRIxPTR ", rc: 0x%" PRIx32,
                   __func__, (uintptr_t)command, rc);
        /* Generate Tpm2Response with "appropriate" RC */
    }
    switch (tpms_context.savedHandle >> TPM2_HR_SHIFT) {
    case TPM2_HT_HMAC_SESSION:
    case TPM2_HT_POLICY_SESSION:
        return resource_manager_load_context_session (resmgr, command);
    default:
        g_debug ("%s: not virtualizing TPM2_ContextLoad for "
                 "handles: 0x%08" PRIx32, __func__, tpms_context.savedHandle);
        break;
    }

    return NULL;
}
/*
 * This function performs the special processing associated with the
 * TPM2_FlushContext command. How much we can "virtualize" of this command
 * depends on the parameters / handle type.
 *
 * Transient objects that are tracked by the RM (stored in the transient
 * HandleMap in the Connection object) then we can simply delete the mapping
 * since transient objects are always saved / flushed after each command is
 * processed. So for this handle type we just delete the mapping, create a
 * Tpm2Response object and return it to the caller.
 *
 * Session objects are not so simple. Sessions cannot be flushed after each
 * use. The TPM will only allow us to save the context as it must maintain
 * some internal tracking information. So when the caller wishes to flush a
 * session context we must remove the entry tracking the mapping between
 * session and connection from the session_slist (if any exists). The comman
 * must then be passed to the TPM as well. We return NULL here to let the
 * caller know.
 */
Tpm2Response*
resource_manager_flush_context (ResourceManager *resmgr,
                                Tpm2Command     *command)
{
    Connection     *connection;
    HandleMap      *map;
    HandleMapEntry *entry;
    Tpm2Response   *response = NULL;
    TPM2_HANDLE      handle;
    TPM2_HT          handle_type;
    TSS2_RC         rc;

    if (tpm2_command_get_code (command) != TPM2_CC_FlushContext) {
        g_warning ("resource_manager_flush_context with wrong command");
        return NULL;
    }
    rc = tpm2_command_get_flush_handle (command, &handle);
    if (rc != TSS2_RC_SUCCESS) {
        connection = tpm2_command_get_connection (command);
        response = tpm2_response_new_rc (connection, rc);
        g_object_unref (connection);
        goto out;
    }
    g_debug ("resource_manager_flush_context handle: 0x%" PRIx32, handle);
    handle_type = handle >> TPM2_HR_SHIFT;
    switch (handle_type) {
    case TPM2_HT_TRANSIENT:
        g_debug ("handle is TPM2_HT_TRANSIENT, virtualizing");
        connection = tpm2_command_get_connection (command);
        map = connection_get_trans_map (connection);
        entry = handle_map_vlookup (map, handle);
        if (entry != NULL) {
            handle_map_remove (map, handle);
            g_object_unref (entry);
            rc = TSS2_RC_SUCCESS;
        } else {
            /*
             * If the handle doesn't map to a HandleMapEntry then it's not one
             * that we're managing and so we can't flush it. Return an error
             * indicating that error is related to a handle, that it's a parameter
             * and that it's the first parameter.
             */
            rc = RM_RC (TPM2_RC_HANDLE + TPM2_RC_P + TPM2_RC_1);
        }
        g_object_unref (map);
        response = tpm2_response_new_rc (connection, rc);
        g_object_unref (connection);
        break;
    case TPM2_HT_HMAC_SESSION:
    case TPM2_HT_POLICY_SESSION:
        g_debug ("handle is TPM2_HT_HMAC_SESSION or TPM2_HT_POLICY_SESSION");
        g_info ("f");
        session_list_remove_handle (resmgr->session_list, handle);
        /*
        response = access_broker_send_command (resmgr->access_broker,
                                               command,
                                               &rc);
        */
        break;
    }

out:
    return response;
}
/*
 * Ensure that executing the provided command will not exceed any of the
 * per-connection quotas enforced by the RM. This is currently limited to
 * transient objects and sessions.
 */
TSS2_RC
resource_manager_quota_check (ResourceManager *resmgr,
                              Tpm2Command     *command)
{
    HandleMap   *handle_map = NULL;
    Connection  *connection = NULL;
    TSS2_RC      rc = TSS2_RC_SUCCESS;

    switch (tpm2_command_get_code (command)) {
    /* These commands load transient objects. */
    case TPM2_CC_CreatePrimary:
    case TPM2_CC_Load:
    case TPM2_CC_LoadExternal:
        connection = tpm2_command_get_connection (command);
        handle_map = connection_get_trans_map (connection);
        if (handle_map_is_full (handle_map)) {
            g_info ("Connection 0x%" PRIxPTR " has exceeded transient object "
                    "limit", (uintptr_t)connection);
            rc = TSS2_RESMGR_RC_OBJECT_MEMORY;
        }
        break;
    /* These commands create sessions. */
    case TPM2_CC_StartAuthSession:
        connection = tpm2_command_get_connection (command);
        if (session_list_is_full (resmgr->session_list, connection)) {
            g_info ("Connection 0x%" PRIxPTR " has exceeded session limit",
                    (uintptr_t)connection);
            rc = TSS2_RESMGR_RC_SESSION_MEMORY;
        }
        break;
    }
    g_clear_object (&connection);
    g_clear_object (&handle_map);

    return rc;
}
/*
 * This is a callback function invoked by the GSList foreach function. It is
 * called when the object associated with a HandleMapEntry is no longer valid
 * (usually when it is flushed) and the HandleMap shouldn't be tracking it.
 */
void
remove_entry_from_handle_map (gpointer data_entry,
                              gpointer data_connection)
{
    HandleMapEntry  *entry       = HANDLE_MAP_ENTRY (data_entry);
    Connection      *connection  = CONNECTION (data_connection);
    HandleMap       *map         = connection_get_trans_map (connection);
    TPM2_HANDLE       handle      = handle_map_entry_get_vhandle (entry);
    TPM2_HT           handle_type = 0;

    handle_type = handle >> TPM2_HR_SHIFT;
    g_debug ("remove_entry_from_handle_map");
    switch (handle_type) {
    case TPM2_HT_TRANSIENT:
        g_debug ("entry 0x%" PRIxPTR " is transient, removing from map",
                 (uintptr_t)entry);
        handle_map_remove (map, handle);
        break;
    default:
        g_debug ("entry 0x%" PRIxPTR " not transient, leaving entry alone",
                 (uintptr_t)entry);
        break;
    }
}
/*
 * This function handles the required post-processing on the HandleMapEntry
 * objects in the GSList that represent objects loaded into the TPM as part of
 * executing a command.
 */
void
post_process_loaded_transients (ResourceManager  *resmgr,
                                GSList          **transient_slist,
                                Connection       *connection,
                                TPMA_CC           command_attrs)
{
    /* if flushed bit is clear we need to flush & save contexts */
    if (!(command_attrs & TPMA_CC_FLUSHED)) {
        g_debug ("flushsave_context for %" PRIu32 " entries",
                 g_slist_length (*transient_slist));
        g_slist_foreach (*transient_slist,
                        resource_manager_flushsave_context,
                        resmgr);
    } else {
        /*
         * if flushed bit is set the transient object entry has been flushed
         * and so we just remove it
         */
        g_debug ("TPMA_CC flushed bit set");
        g_slist_foreach (*transient_slist,
                         remove_entry_from_handle_map,
                         connection);
    }
    g_slist_free_full (*transient_slist, g_object_unref);
}
/*
 * This structure is used to keep state while iterating over a list of
 * TPM2_HANDLES.
 * 'cap_data'    : this parameter is used to collect the list of handles that
 *   we want as well as the number of handles in the structure
 * 'max_count'   : is the maximum number of handles to return
 * 'more_data'   : once max_count handles have been collected this variable
 *   tells the caller whether additional handles would have been returned had
 *   max_count been larger
 * 'start_handle': is the numerically smallest handle that should be collected
 *   into cap_data.
 */
typedef struct {
    TPMS_CAPABILITY_DATA *cap_data;
    size_t                max_count;
    gboolean              more_data;
    TPM2_HANDLE            start_handle;
} vhandle_iterator_state_t;
/*
 * This callback function is invoked as part of iterating over a list of
 * handles. The first parameter is an entry from the collection being
 * traversed. The second is a reference to a vhandle_iterator_state_t
 * structure.
 * This structure is used to maintain state while iterating over the
 * collection.
 */
void
vhandle_iterator_callback (gpointer entry,
                           gpointer data)
{
    TPM2_HANDLE                vhandle  = (uintptr_t)entry;
    vhandle_iterator_state_t *state    = (vhandle_iterator_state_t*)data;
    TPMS_CAPABILITY_DATA     *cap_data = state->cap_data;

    /* if vhandle is numerically smaller than the start value just return */
    if (vhandle < state->start_handle) {
        return;
    }
    g_debug ("vhandle_iterator_callback with max_count: %zu and count: %"
             PRIu32, state->max_count, cap_data->data.handles.count);
    /* if we've collected max_count handles set 'more_data' and return */
    if (!(cap_data->data.handles.count < state->max_count)) {
        state->more_data = TRUE;
        return;
    }
    cap_data->data.handles.handle [cap_data->data.handles.count] = vhandle;
    ++cap_data->data.handles.count;
}
/*
 * This is a GCompareFunc used to sort a list of TPM2_HANDLES.
 */
int
handle_compare (gconstpointer a,
                gconstpointer b)
{
    TPM2_HANDLE handle_a = (uintptr_t)a;
    TPM2_HANDLE handle_b = (uintptr_t)b;

    if (handle_a < handle_b) {
        return -1;
    } else if (handle_a > handle_b) {
        return 1;
    } else {
        return 0;
    }
}
/*
 * The get_cap_transient function populates a TPMS_CAPABILITY_DATA structure
 * with the handles in the provided HandleMap 'map'. The 'prop' parameter
 * is the lowest numerical handle to return. The 'count' parameter is the
 * maximum number of handles to return in the capability data structure.
 * Returns:
 *   TRUE when more handles are present
 *   FALSE when there are no more handles
 */
gboolean
get_cap_handles (HandleMap            *map,
                 TPM2_HANDLE            prop,
                 UINT32                count,
                 TPMS_CAPABILITY_DATA *cap_data)
{
    GList *vhandle_list;
    vhandle_iterator_state_t state = {
        .cap_data     = cap_data,
        .max_count    = count,
        .more_data    = FALSE,
        .start_handle = prop,
    };

    cap_data->capability = TPM2_CAP_HANDLES;
    cap_data->data.handles.count = 0;

    vhandle_list = handle_map_get_keys (map);
    vhandle_list = g_list_sort (vhandle_list, handle_compare);
    g_list_foreach (vhandle_list, vhandle_iterator_callback, &state);

    g_debug ("iterating over %" PRIu32 " vhandles from g_list_foreach",
             cap_data->data.handles.count);
    size_t i;
    for (i = 0; i < cap_data->data.handles.count; ++i) {
        g_debug ("  vhandle: 0x%" PRIx32, cap_data->data.handles.handle [i]);
    }

    return state.more_data;
}
/*
 * These macros are used to set fields in a Tpm2Response buffer that we
 * create in response to the TPM2 GetCapability command. They are very
 * specifically tailored and should not be used elsewhere.
 */
#define YES_NO_OFFSET TPM_HEADER_SIZE
#define YES_NO_SET(buffer, value) \
    (*(TPMI_YES_NO*)(buffer + YES_NO_OFFSET) = value)
#define CAP_OFFSET (TPM_HEADER_SIZE + sizeof (TPMI_YES_NO))
#define CAP_SET(buffer, value) \
    (*(TPM2_CAP*)(buffer + CAP_OFFSET) = htobe32 (value))
#define HANDLE_COUNT_OFFSET (CAP_OFFSET + sizeof (TPM2_CAP))
#define HANDLE_COUNT_SET(buffer, value) \
    (*(UINT32*)(buffer + HANDLE_COUNT_OFFSET) = htobe32 (value))
#define HANDLE_OFFSET (HANDLE_COUNT_OFFSET + sizeof (UINT32))
#define HANDLE_INDEX(i) (sizeof (TPM2_HANDLE) * i)
#define HANDLE_SET(buffer, i, value) \
    (*(TPM2_HANDLE*)(buffer + HANDLE_OFFSET + HANDLE_INDEX (i)) = \
        htobe32 (value))
#define CAP_RESP_SIZE(value) \
    (TPM_HEADER_SIZE + \
     sizeof (TPMI_YES_NO) + \
     sizeof ((value)->capability) + \
     sizeof ((value)->data.handles.count) + \
     ((value)->data.handles.count * \
      sizeof ((value)->data.handles.handle [0])))
/*
 * This function is used to build a response buffer that contains the provided
 * TPMS_CAPABILITY_DATA and TPMI_YES_NO. These are the two response parameters
 * to the TPM2_GetCapability function.
 * The 'cap_data' parameter *must* have the TPMU_CAPABILITY union populated
 * with the TPM2_CAP_HANDLES selector.
 */
uint8_t*
build_cap_handles_response (TPMS_CAPABILITY_DATA *cap_data,
                            TPMI_YES_NO           more_data)
{
    size_t i;
    uint8_t *buf;

    buf = calloc (1, CAP_RESP_SIZE (cap_data));
    if (buf == NULL) {
        tabrmd_critical ("failed to allocate buffer for handle capability "
                         "response");
    }
    set_response_tag (buf, TPM2_ST_NO_SESSIONS);
    set_response_size (buf, CAP_RESP_SIZE (cap_data));
    set_response_code (buf, TSS2_RC_SUCCESS);
    YES_NO_SET (buf, more_data);
    CAP_SET (buf, cap_data->capability);
    HANDLE_COUNT_SET (buf, cap_data->data.handles.count);
    for (i = 0; i < cap_data->data.handles.count; ++i) {
        HANDLE_SET (buf, i, cap_data->data.handles.handle [i]);
    }

    return buf;
}
/*
 * This function takes a Tpm2Command and the associated connection object
 * as parameters. The Tpm2Command *must* be a GetCapability command. If it's
 * a GetCapability command that we "virtualize" then we'll build a
 * Tpm2Response object and return it. If not we return NULL.
 */
Tpm2Response*
get_cap_handles_response (Tpm2Command *command,
                          Connection *connection)
{
    TPM2_CAP  cap         = tpm2_command_get_cap (command);
    UINT32   prop        = tpm2_command_get_prop (command);
    UINT32   prop_count  = tpm2_command_get_prop_count (command);
    TPM2_HT   handle_type = prop >> TPM2_HR_SHIFT;
    HandleMap *map;
    TPMS_CAPABILITY_DATA cap_data = { .capability = cap };
    gboolean more_data = FALSE;
    uint8_t *resp_buf;
    Tpm2Response *response = NULL;

    g_debug ("processing TPM2_CC_GetCapability with cap: 0x%" PRIx32
             " prop: 0x%" PRIx32 " prop_count: 0x%" PRIx32,
             cap, prop, prop_count);
    if (cap == TPM2_CAP_HANDLES && handle_type == TPM2_HT_TRANSIENT) {
        g_debug ("TPM2_CAP_HANDLES && TPM2_HT_TRANSIENT");
        map = connection_get_trans_map (connection);
        more_data = get_cap_handles (map,  prop, prop_count, &cap_data);
        g_object_unref (map);
        resp_buf = build_cap_handles_response (&cap_data, more_data);
        response = tpm2_response_new (connection,
                                      resp_buf,
                                      CAP_RESP_SIZE (&cap_data),
                                      tpm2_command_get_attributes (command));
    }

    return response;
}
/*
 * If the provided command is something that the ResourceManager "virtualizes"
 * then this function will do so and return a Tpm2Response object that will be
 * returned to the same connection. If the command isn't something that we
 * virtualize then we just return NULL.
 */
Tpm2Response*
command_special_processing (ResourceManager *resmgr,
                            Tpm2Command     *command)
{
    Connection   *connection = NULL;
    Tpm2Response *response   = NULL;

    switch (tpm2_command_get_code (command)) {
    case TPM2_CC_FlushContext:
        g_debug ("processing TPM2_CC_FlushContext");
        response = resource_manager_flush_context (resmgr, command);
        break;
    case TPM2_CC_ContextSave:
        g_debug ("processing TPM2_CC_ContextSave");
        response = resource_manager_save_context (resmgr, command);
        break;
    case TPM2_CC_ContextLoad:
        g_debug ("%s: processing TPM2_CC_ContextLoad", __func__);
        response = resource_manager_load_context (resmgr, command);
        break;
    case TPM2_CC_GetCapability:
        g_debug ("processing TPM2_CC_GetCapability");
        connection = tpm2_command_get_connection (command);
        response = get_cap_handles_response (command, connection);
        g_object_unref (connection);
        break;
    default:
        break;
    }

    return response;
}
/*
 * This function creates a mapping from the transient physical to a virtual
 * handle in the provided response object. This mapping is then added to
 * the transient HandleMap for the associated connection, as well as the
 * list of currently loaded transient objects.
 */
void
create_context_mapping_transient (ResourceManager  *resmgr,
                                  Tpm2Response     *response,
                                  GSList          **loaded_transient_slist)
{
    HandleMap      *handle_map;
    HandleMapEntry *handle_entry;
    TPM2_HANDLE      phandle, vhandle;
    Connection     *connection;
    UNUSED_PARAM(resmgr);

    g_debug ("create_context_mapping_transient");
    phandle = tpm2_response_get_handle (response);
    g_debug ("  physical handle: 0x%08" PRIx32, phandle);
    connection = tpm2_response_get_connection (response);
    handle_map = connection_get_trans_map (connection);
    g_object_unref (connection);
    vhandle = handle_map_next_vhandle (handle_map);
    if (vhandle == 0) {
        g_error ("vhandle rolled over!");
    }
    g_debug ("  vhandle:0x%08" PRIx32, vhandle);
    handle_entry = handle_map_entry_new (phandle, vhandle);
    if (handle_entry == NULL) {
        g_warning ("failed to create new HandleMapEntry for handle 0x%"
                   PRIx32, phandle);
    }
    g_debug ("handle map entry: 0x%" PRIxPTR, (uintptr_t)handle_entry);
    *loaded_transient_slist = g_slist_prepend (*loaded_transient_slist,
                                               handle_entry);
    handle_map_insert (handle_map, vhandle, handle_entry);
    g_object_unref (handle_map);
    tpm2_response_set_handle (response, vhandle);
    g_object_ref (handle_entry);
    *loaded_transient_slist = g_slist_prepend (*loaded_transient_slist,
                                                   handle_entry);
}
/*
 * This function after a Tpm2Command is sent to the TPM and:
 * - we receive a Tpm2Response object with a handle in the response buffers
 *   handle area
 * - the handle is a session handle
 * Since there's a handle in the response handle area the caller is being
 * returned a new handle after a context was successfully created or loaded.
 * So we know that the response is to one of two possible commands:
 * - a session being loaded by a call to LoadContext
 * - a session was newly created by a call to StartAuthSession
 * We differentiate between these two situations as follows:
 * - A call to 'LoadContext' implies that the session already exists and so
 *   it must already be in the session_list.
 *   - If it's in the session_list *AND NOT* in the abandoned_session_queue
 *     then the caller is just loading a context they already own and so we
 *     set the session state to SAVED_RM and add the session to the list of
 *     loaded sessions.
 *   - If it's *NOT* in the session_list *AND* in the abandoned_session_queue
 *     then the caller is loading a context saved by a different connection
 *     and so we make the current connection the owner of the session, set
 *     the session state to SAVED_RM and add the session to the list of loaded
 *     sessions.
 * - A call to 'StartAuthSession' will return a handle for a session object
 *   that is not in either the session_list or the abandoned_session_queue.
 *   In this case we just create a new SessionEntry and add it to the
 *   session_list and the list of loaded sessions.
 * NOTE: If the response doesn't indicate 'success' then we just ignore it
 * since there's nothing useful that we can do.
 */
void
create_context_mapping_session (ResourceManager *resmgr,
                                Tpm2Response    *response,
                                TPM2_HANDLE      handle)
{
    SessionEntry *entry = NULL;
    Connection   *conn_resp = NULL, *conn_entry = NULL;

    entry = session_list_lookup_handle (resmgr->session_list, handle);
    conn_resp = tpm2_response_get_connection (response);
    if (entry != NULL) {
        g_debug ("%s: got SessionEntry 0x%" PRIxPTR " that's in the "
                 "SessionList", __func__, (uintptr_t)entry);
        conn_entry = session_entry_get_connection (entry);
        if (conn_resp != conn_entry) {
            g_warning ("%s: connections do not match!", __func__);
        }
    } else {
        g_debug ("%s: handle is a session, creating entry for SessionList "
                 "and SessionList", __func__);
        entry = session_entry_new (conn_resp, handle);
        session_entry_set_state (entry, SESSION_ENTRY_LOADED);
        session_list_insert (resmgr->session_list, entry);
    }
    g_clear_object (&conn_resp);
    g_clear_object (&conn_entry);
    g_clear_object (&entry);
    g_debug ("dumping resmgr->session_list:");
    session_list_prettyprint (resmgr->session_list);
}
/*
 * Each Tpm2Response object can have at most one handle in it.
 * This function assumes that the handle in the parameter Tpm2Response
 * object is the handle assigned by the TPM. Depending on the type of the
 * handle we do to possible things:
 * For TPM2_HT_TRANSIENT handles we create a new virtual handle and
 * allocates a new HandleMapEntry to map the virtual handle to a
 * TPMS_CONTEXT structure when processing future commands associated
 * with the same connection. This HandleMapEntry is inserted into the
 * handle map for the connection. It is also added to the list of loaded
 * transient objects so that it can be saved / flushed by the typical code
 * path.
 * For TPM2_HT_HMAC_SESSION or TPM2_HT_POLICY_SESSION handles we create a
 * new session_entry_t object, populate the connection field with the
 * connection associated with the response object, and set the savedHandle
 * field. We then add this entry to the list of sessions we're tracking
 * (session_slist) and the list of loaded sessions (loaded_session_slist).
 */
void
resource_manager_create_context_mapping (ResourceManager  *resmgr,
                                         Tpm2Response     *response,
                                         GSList          **loaded_transient_slist)
{
    TPM2_HANDLE       handle;

    g_debug ("resource_manager_create_context_mapping");
    if (!tpm2_response_has_handle (response)) {
        g_debug ("response 0x%" PRIxPTR " has no handles", (uintptr_t)response);
        return;
    }
    handle = tpm2_response_get_handle (response);
    switch (handle >> TPM2_HR_SHIFT) {
    case TPM2_HT_TRANSIENT:
        create_context_mapping_transient (resmgr, response, loaded_transient_slist);
        break;
    case TPM2_HT_HMAC_SESSION:
    case TPM2_HT_POLICY_SESSION:
        create_context_mapping_session (resmgr, response, handle);
        break;
    default:
        g_debug ("  not creating context for handle: 0x%08" PRIx32, handle);
        break;
    }
}
/**
 * This function is invoked in response to the receipt of a Tpm2Command.
 * This is the place where we send the command buffer out to the TPM
 * through the AccessBroker which will eventually get it to the TPM for us.
 * The AccessBroker will send us back a Tpm2Response that we send back to
 * the client by way of our Sink object. The flow is roughly:
 * - Receive the Tpm2Command as a parameter
 * - Load all virtualized objects required by the command.
 * - Send the Tpm2Command out through the AccessBroker.
 * - Receive the response from the AccessBroker.
 * - Virtualize the new objects created by the command & referenced in the
 *   response.
 * - Enqueue the response back out to the processing pipeline through the
 *   Sink object.
 * - Flush all objects loaded for the command or as part of executing the
 *   command..
 */
void
resource_manager_process_tpm2_command (ResourceManager   *resmgr,
                                       Tpm2Command       *command)
{
    Connection    *connection;
    Tpm2Response   *response;
    TSS2_RC         rc = TSS2_RC_SUCCESS;
    GSList         *transient_slist = NULL;
    TPMA_CC         command_attrs;

    command_attrs = tpm2_command_get_attributes (command);
    g_debug ("resource_manager_process_tpm2_command: resmgr: 0x%" PRIxPTR
             ", cmd: 0x%" PRIxPTR, (uintptr_t)resmgr, (uintptr_t)command);
    dump_command (command);
    connection = tpm2_command_get_connection (command);
    /* If executing the command would exceed a per connection quota */
    rc = resource_manager_quota_check (resmgr, command);
    if (rc != TSS2_RC_SUCCESS) {
        response = tpm2_response_new_rc (connection, rc);
        goto send_response;
    }
    /* Do command-specific processing. */
    response = command_special_processing (resmgr, command);
    if (response != NULL) {
        goto send_response;
    }
    /* Load objects associated with the handles in the command handle area. */
    if (tpm2_command_get_handle_count (command) > 0) {
        resource_manager_load_handles (resmgr,
                                       command,
                                       &transient_slist);
    }
    /* Load objets associated with the authorizations in the command. */
    if (tpm2_command_has_auths (command)) {
        g_info ("%s, Processing auths for command: 0x%" PRIxPTR, __func__,
                (uintptr_t)command);
        auth_callback_data_t auth_callback_data = {
            .resmgr = resmgr,
            .command = command,
        };
        tpm2_command_foreach_auth (command,
                                   resource_manager_load_auth_callback,
                                   &auth_callback_data);
    }
    /* Send command and create response object. */
    response = access_broker_send_command (resmgr->access_broker,
                                           command,
                                           &rc);
    if (response == NULL) {
        g_warning ("access_broker_send_command returned error: 0x%x", rc);
        response = tpm2_response_new_rc (connection, rc);
    }
    dump_response (response);
    /* transform virtualized handles in Tpm2Response if necessary */
    resource_manager_create_context_mapping (resmgr,
                                             response,
                                             &transient_slist);
send_response:
    sink_enqueue (resmgr->sink, G_OBJECT (response));
    g_object_unref (response);
    /* save contexts that were previously loaded */
    session_list_foreach (resmgr->session_list,
                          resource_manager_save_session_context,
                          resmgr);
    post_process_loaded_transients (resmgr, &transient_slist, connection, command_attrs);
    g_object_unref (connection);
    return;
}
/*
 * Return FALSE to terminate main thread.
 */
gboolean
resource_manager_process_control (ResourceManager *resmgr,
                                  ControlMessage *msg)
{
    ControlCode code = control_message_get_code (msg);
    Connection *conn;

    g_debug ("%s", __func__);
    switch (code) {
    case CHECK_CANCEL:
        sink_enqueue (resmgr->sink, G_OBJECT (msg));
        return FALSE;
    case CONNECTION_REMOVED:
        conn = CONNECTION (control_message_get_object (msg));
        g_debug ("%s: received CONNECTION_REMOVED message for connection: 0x%"
                 PRIxPTR, __func__, (uintptr_t)conn);
        resource_manager_remove_connection (resmgr, conn);
        sink_enqueue (resmgr->sink, G_OBJECT (msg));
        return TRUE;
    default:
        g_warning ("%s: Unknown control code: %d ... ignoring",
                   __func__, code);
        return TRUE;
    }
}
/**
 * This function acts as a thread. It simply:
 * - Blocks on the in_queue. Then wakes up and
 * - Dequeues a message from the in_queue.
 * - Processes the message (depending on TYPE)
 * - Does it all over again.
 */
gpointer
resource_manager_thread (gpointer data)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (data);
    GObject         *obj = NULL;
    gboolean done = FALSE;

    g_debug ("resource_manager_thread start");
    while (!done) {
        obj = message_queue_dequeue (resmgr->in_queue);
        g_debug ("resource_manager_thread: message_queue_dequeue got obj: "
                 "0x%" PRIxPTR, (uintptr_t)obj);
        if (obj == NULL) {
            g_debug ("resource_manager_thread: dequeued a null object");
            break;
        }
        if (IS_TPM2_COMMAND (obj)) {
            resource_manager_process_tpm2_command (resmgr, TPM2_COMMAND (obj));
        } else if (IS_CONTROL_MESSAGE (obj)) {
            gboolean ret =
                resource_manager_process_control (resmgr, CONTROL_MESSAGE (obj));
            if (ret == FALSE) {
                done = TRUE;
            }
        }
        g_object_unref (obj);
    }

    return NULL;
}
static void
resource_manager_unblock (Thread *self)
{
    ControlMessage *msg;
    ResourceManager *resmgr = RESOURCE_MANAGER (self);

    if (resmgr == NULL)
        g_error ("resource_manager_cancel passed NULL ResourceManager");
    msg = control_message_new (CHECK_CANCEL);
    g_debug ("resource_manager_cancel: enqueuing ControlMessage: 0x%" PRIxPTR,
             (uintptr_t)msg);
    message_queue_enqueue (resmgr->in_queue, G_OBJECT (msg));
    g_object_unref (msg);
}
/**
 * Implement the 'enqueue' function from the Sink interface. This is how
 * new messages / commands get into the AccessBroker.
 */
void
resource_manager_enqueue (Sink        *sink,
                          GObject     *obj)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (sink);

    g_debug ("resource_manager_enqueue: ResourceManager: 0x%" PRIxPTR " obj: "
             "0x%" PRIxPTR, (uintptr_t)resmgr, (uintptr_t)obj);
    message_queue_enqueue (resmgr->in_queue, obj);
}
/**
 * Implement the 'add_sink' function from the SourceInterface. This adds a
 * reference to an object that implements the SinkInterface to this objects
 * internal structure. We pass it data.
 */
void
resource_manager_add_sink (Source *self,
                           Sink   *sink)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (self);
    GValue value = G_VALUE_INIT;

    g_debug ("resource_manager_add_sink: ResourceManager: 0x%" PRIxPTR
             ", Sink: 0x%" PRIxPTR, (uintptr_t)resmgr, (uintptr_t)sink);
    g_value_init (&value, G_TYPE_OBJECT);
    g_value_set_object (&value, sink);
    g_object_set_property (G_OBJECT (resmgr), "sink", &value);
    g_value_unset (&value);
}
/**
 * GObject property setter.
 */
static void
resource_manager_set_property (GObject        *object,
                               guint           property_id,
                               GValue const   *value,
                               GParamSpec     *pspec)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (object);

    g_debug ("resource_manager_set_property: 0x%" PRIxPTR,
             (uintptr_t)resmgr);
    switch (property_id) {
    case PROP_QUEUE_IN:
        resmgr->in_queue = g_value_get_object (value);
        g_debug ("  in_queue: 0x%" PRIxPTR, (uintptr_t)resmgr->in_queue);
        break;
    case PROP_SINK:
        if (resmgr->sink != NULL) {
            g_warning ("  sink already set");
            break;
        }
        resmgr->sink = SINK (g_value_get_object (value));
        g_object_ref (resmgr->sink);
        g_debug ("  sink: 0x%" PRIxPTR, (uintptr_t)resmgr->sink);
        break;
    case PROP_ACCESS_BROKER:
        if (resmgr->access_broker != NULL) {
            g_warning ("  access_broker already set");
            break;
        }
        resmgr->access_broker = g_value_get_object (value);
        g_object_ref (resmgr->access_broker);
        g_debug ("  access_broker: 0x%" PRIxPTR, (uintptr_t)resmgr->access_broker);
        break;
    case PROP_SESSION_LIST:
        resmgr->session_list = SESSION_LIST (g_value_dup_object (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}
/**
 * GObject property getter.
 */
static void
resource_manager_get_property (GObject     *object,
                               guint        property_id,
                               GValue      *value,
                               GParamSpec  *pspec)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (object);

    g_debug ("resource_manager_get_property: 0x%" PRIxPTR, (uintptr_t)resmgr);
    switch (property_id) {
    case PROP_QUEUE_IN:
        g_value_set_object (value, resmgr->in_queue);
        break;
    case PROP_SINK:
        g_value_set_object (value, resmgr->sink);
        break;
    case PROP_ACCESS_BROKER:
        g_value_set_object (value, resmgr->access_broker);
        break;
    case PROP_SESSION_LIST:
        g_value_set_object (value, resmgr->session_list);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}
/**
 * Bring down the ResourceManager as gracefully as we can.
 */
static void
resource_manager_dispose (GObject *obj)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (obj);
    Thread *thread = THREAD (obj);

    g_debug ("%s: 0x%" PRIxPTR, __func__, (uintptr_t)resmgr);
    if (resmgr == NULL)
        g_error ("%s: passed NULL parameter", __func__);
    if (thread->thread_id != 0)
        g_error ("%s: thread running, cancel thread first", __func__);
    g_clear_object (&resmgr->in_queue);
    g_clear_object (&resmgr->sink);
    g_clear_object (&resmgr->access_broker);
    g_clear_object (&resmgr->session_list);
    G_OBJECT_CLASS (resource_manager_parent_class)->dispose (obj);
}
static void
resource_manager_init (ResourceManager *manager)
{
    UNUSED_PARAM (manager);
}
/**
 * GObject class initialization function. This function boils down to:
 * - Setting up the parent class.
 * - Set dispose, property get/set.
 * - Install properties.
 */
static void
resource_manager_class_init (ResourceManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    ThreadClass  *thread_class = THREAD_CLASS (klass);

    if (resource_manager_parent_class == NULL)
        resource_manager_parent_class = g_type_class_peek_parent (klass);
    object_class->dispose = resource_manager_dispose;
    object_class->get_property = resource_manager_get_property;
    object_class->set_property = resource_manager_set_property;
    thread_class->thread_run     = resource_manager_thread;
    thread_class->thread_unblock = resource_manager_unblock;

    obj_properties [PROP_QUEUE_IN] =
        g_param_spec_object ("queue-in",
                             "input queue",
                             "Input queue for messages.",
                             G_TYPE_OBJECT,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    obj_properties [PROP_SINK] =
        g_param_spec_object ("sink",
                             "Sink",
                             "Reference to a Sink object that we pass messages to.",
                             G_TYPE_OBJECT,
                             G_PARAM_READWRITE);
    obj_properties [PROP_ACCESS_BROKER] =
        g_param_spec_object ("access-broker",
                             "AccessBroker object",
                             "TPM Access Broker for communication with TPM",
                             TYPE_ACCESS_BROKER,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    obj_properties [PROP_SESSION_LIST] =
        g_param_spec_object ("session-list",
                             "SessionList object",
                             "Data structure to hold session tracking data",
                             TYPE_SESSION_LIST,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       obj_properties);
}
/**
 * Boilerplate code to register functions with the SourceInterface.
 */
static void
resource_manager_source_interface_init (gpointer g_iface)
{
    SourceInterface *source = (SourceInterface*)g_iface;
    source->add_sink = resource_manager_add_sink;
}
/**
 * Boilerplate code to register function with the SinkInterface.
 */
static void
resource_manager_sink_interface_init (gpointer g_iface)
{
    SinkInterface *sink = (SinkInterface*)g_iface;
    sink->enqueue = resource_manager_enqueue;
}
/*
 * This function prunes old sessions that have been abandoned by their creator.
 * When the upper bound on the number of abandoned sessions is exceeded this
 * function causes the older sessions to be removed from the internal queue
 * and flushed from the TPM.
 */
gboolean
flush_session_callback (SessionEntry *entry,
                        gpointer data)
{
    ResourceManager *resmgr = RESOURCE_MANAGER (data);
    TSS2_RC rc;
    TPM2_HANDLE handle = session_entry_get_handle (entry);

    g_debug ("%s: flushing stale abandoned SessionEntry: 0x%" PRIxPTR " with "
             "handle: 0x%08" PRIx32,
             __func__, (uintptr_t)entry, handle);
    rc = access_broker_context_flush (resmgr->access_broker, handle);
    if (rc != TSS2_RC_SUCCESS) {
        g_warning ("%s: failed to flush abandoned session context with "
                   "handle 0x%" PRIx32 ": 0x%" PRIx32,
                   __func__, handle, rc);
        return FALSE;
    }
    return TRUE;
}
/*
 * This structure is used to pass required data into the
 * connection_close_session_callback function.
 */
typedef struct {
    Connection *connection;
    ResourceManager *resource_manager;
} connection_close_data_t;
/*
 * This is a callback function invoked foreach SessionEntry in the SessionList
 * when a client Connection is closed. As it iterates over each SessionEntry,
 * it identifies sessions associated with the connection being closed and,
 * performs some task depending on its state.
 *
 * If session is in state SESSION_ENTRY_SAVED_CLIENT:
 * - take a reference to the SessionEntry
 * - remove SessionEntry from session list
 * - change state to SESSION_ENTRY_SAVED_CLIENT_CLOSED
 * - "prune" other abandoned sessions
 * - add SessionEntry to queue of abandoned sessions
 * If session is in state SESSION_ENTRY_SAVED_RM:
 * - flush session from TPM
 * - remove SessionEntry from session list
 * If session is in any other state
 * - panic
 */
void
connection_close_session_callback (gpointer data,
                                   gpointer user_data)
{
    SessionEntry *session_entry = SESSION_ENTRY (data);
    SessionEntryStateEnum session_state = session_entry_get_state (session_entry);
    connection_close_data_t *callback_data = (connection_close_data_t*)user_data;
    Connection *connection = callback_data->connection;
    ResourceManager *resource_manager = callback_data->resource_manager;
    TPM2_HANDLE handle;
    TSS2_RC rc;

    g_debug ("%s", __func__);
    if (session_entry->connection != connection) {
        g_debug ("%s: connection mismatch", __func__);
        return;
    }
    g_debug ("%s: processing SessionEntry 0x%" PRIxPTR " associated with "
             "Connection 0x%" PRIxPTR, __func__, (uintptr_t)session_entry,
             (uintptr_t)connection);
    handle = session_entry_get_handle (session_entry);
    switch (session_state) {
    case SESSION_ENTRY_SAVED_CLIENT:
        g_debug ("%s: Connection 0x%" PRIxPTR ", TPM2_HANDLE 0x%08" PRIx32,
                 __func__, (uintptr_t)connection, handle);
        session_list_abandon_handle (resource_manager->session_list,
                                     connection,
                                     handle);
        session_list_prune_abandoned (resource_manager->session_list,
                                      flush_session_callback,
                                      resource_manager);
        break;
    case SESSION_ENTRY_SAVED_RM:
        g_debug ("%s: SessionEntry 0x%" PRIxPTR " is in state "
                 "%s: flushing.", __func__, (uintptr_t)session_entry,
                 session_entry_state_to_str (session_state));
        rc = access_broker_context_flush (resource_manager->access_broker,
                                          handle);
        if (rc != TSS2_RC_SUCCESS) {
            g_warning ("failed to flush context associated with "
                       "connection: 0x%" PRIxPTR, (uintptr_t)connection);
        }
        session_list_remove (resource_manager->session_list,
                             session_entry);
        break;
    default:
        /* This is a situation that should never happen */
        g_error ("Connection closed with session in unexpected state: %s",
                 session_entry_state_to_str (session_state));
        break;
    }
}
/*
 * This function is invoked when a connection is removed from the
 * ConnectionManager. This is if how we know a connection has been closed.
 * When a connection is removed, we need to remove all associated sessions
 * from the TPM.
 */
void
resource_manager_remove_connection (ResourceManager *resource_manager,
                                    Connection *connection)
{
    connection_close_data_t connection_close_data = {
        .connection = connection,
        .resource_manager = resource_manager,
    };

    g_info ("%s: flushing session contexts associated with connection 0x%"
            PRIxPTR, __func__, (uintptr_t)connection);
    session_list_foreach (resource_manager->session_list,
                          connection_close_session_callback,
                          &connection_close_data);
    g_debug ("%s: done", __func__);
}
/**
 * Create new ResourceManager object.
 */
ResourceManager*
resource_manager_new (AccessBroker    *broker,
                      SessionList     *session_list)
{
    if (broker == NULL)
        g_error ("resource_manager_new passed NULL AccessBroker");
    MessageQueue *queue = message_queue_new ();
    return RESOURCE_MANAGER (g_object_new (TYPE_RESOURCE_MANAGER,
                                           "queue-in",        queue,
                                           "access-broker",   broker,
                                           "session-list",    session_list,
                                           NULL));
}
