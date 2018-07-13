#include <micrortps/client/core/session/session.h>
#include <micrortps/client/core/session/submessage.h>
#include <micrortps/client/core/util/time.h>
#include <micrortps/client/core/communication/communication.h>
#include <micrortps/client/core/serialization/xrce_protocol.h>
#include "log/message.h"

// Autogenerate these defines by the protocol generator tool?
#define HEARTBEAT_MAX_MSG_SIZE 16
#define ACKNACK_MAX_MSG_SIZE 16
#define CREATE_SESSION_MAX_MSG_SIZE 38
#define DELETE_SESSION_MAX_MSG_SIZE 16

#define MIN_SESSION_STATUS_WAITING         1 //ms
#define MAX_CONNECTION_ATTEMPS            10

#define INITIAL_REQUEST_ID 0x0010

const uint8_t MR_STATUS_OK = STATUS_OK;
const uint8_t MR_STATUS_OK_MATCHED = STATUS_OK_MATCHED;
const uint8_t MR_STATUS_ERR_DDS_ERROR = STATUS_ERR_DDS_ERROR;
const uint8_t MR_STATUS_ERR_MISMATCH = STATUS_ERR_MISMATCH;
const uint8_t MR_STATUS_ERR_ALREADY_EXISTS = STATUS_ERR_ALREADY_EXISTS;
const uint8_t MR_STATUS_ERR_DENIED = STATUS_ERR_DENIED;
const uint8_t MR_STATUS_ERR_UNKNOWN_REFERENCE = STATUS_ERR_UNKNOWN_REFERENCE;
const uint8_t MR_STATUS_ERR_INVALID_DATA = STATUS_ERR_INVALID_DATA;
const uint8_t MR_STATUS_ERR_INCOMPATIBLE = STATUS_ERR_INCOMPATIBLE;
const uint8_t MR_STATUS_ERR_RESOURCES = STATUS_ERR_RESOURCES;
const uint8_t MR_STATUS_NONE = STATUS_NONE;

static uint16_t generate_request_id(mrSession* session);

static void flash_output_streams(mrSession* session);
static bool listen_message(mrSession* session, int poll_ms);
static bool listen_message_reliably(mrSession* session, int poll_ms);

static bool wait_session_status(mrSession* session, uint8_t* buffer, size_t length, size_t attempts);

static void send_message(const mrSession* session, uint8_t* buffer, size_t length);
static bool recv_message(const mrSession* session, uint8_t**buffer, size_t* length, int poll_ms);

static void write_submessage_heartbeat(const mrSession* session, mrStreamId stream);
static void write_submessage_acknack(const mrSession* session, mrStreamId stream);

static void read_message(mrSession* session, MicroBuffer* message);
static void read_stream(mrSession* session, MicroBuffer* message, mrStreamId id, uint16_t seq_num);
static void read_submessage_list(mrSession* session, MicroBuffer* submessages, mrStreamId stream_id);
static void read_submessage(mrSession* session, MicroBuffer* submessage,
                            uint8_t submessage_id, mrStreamId stream_id, uint16_t length, uint8_t flags);

static void read_submessage_fragment(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id, bool last_fragment);
static void read_submessage_status(mrSession* session, MicroBuffer* submessage);
static void read_submessage_data(mrSession* session, MicroBuffer* submessage, uint16_t length, mrStreamId stream_id, uint8_t format);
static void read_submessage_heartbeat(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id);
static void read_submessage_acknack(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id);

static void process_status(mrSession* session, mrObjectId object_id, int16_t request_id, uint8_t status);

//==================================================================
//                             PUBLIC
//==================================================================

void mr_init_session(mrSession* session, uint8_t session_id, uint32_t key, mrCommunication* comm)
{
    session->comm = comm;
    session->last_request_id = INITIAL_REQUEST_ID;
    session->reset_streams = true;

    session->request_list = NULL;
    session->status_list = NULL;
    session->request_status_list_size = 0;

    session->on_status = NULL;
    session->on_status_args = NULL;
    session->on_topic = NULL;
    session->on_topic_args = NULL;

    init_session_info(&session->info, session_id, key);
    init_stream_storage(&session->streams);
}

bool mr_create_session(mrSession* session)
{
    uint8_t create_session_buffer[CREATE_SESSION_MAX_MSG_SIZE];
    MicroBuffer mb;
    init_micro_buffer_offset(&mb, create_session_buffer, CREATE_SESSION_MAX_MSG_SIZE, session_header_offset(&session->info));

    write_create_session(&session->info, &mb, get_milli_time(), session->reset_streams);
    stamp_create_session_header(&session->info, mb.init);
    set_session_info_request(&session->info, MR_REQUEST_LOGIN);

    bool received = wait_session_status(session, create_session_buffer, micro_buffer_length(&mb), MAX_CONNECTION_ATTEMPS);
    bool created = received && STATUS_OK == session->info.last_requested_status;
    session->reset_streams = !created;
    return created;
}

bool mr_delete_session(mrSession* session)
{
    uint8_t delete_session_buffer[DELETE_SESSION_MAX_MSG_SIZE];
    MicroBuffer mb;
    init_micro_buffer_offset(&mb, delete_session_buffer, DELETE_SESSION_MAX_MSG_SIZE, session_header_offset(&session->info));

    write_delete_session(&session->info, &mb);
    stamp_session_header(&session->info, 0, 0, mb.init);
    set_session_info_request(&session->info, MR_REQUEST_LOGOUT);

    bool received = wait_session_status(session, delete_session_buffer, micro_buffer_length(&mb), MAX_CONNECTION_ATTEMPS);
    bool deleted = received && STATUS_OK == session->info.last_requested_status;
    if(deleted)
    {
        reset_stream_storage(&session->streams);
    }
    return deleted;
}

void mr_set_status_callback(mrSession* session, mrOnStatusFunc on_status_func, void* args)
{
    session->on_status = on_status_func;
    session->on_status_args = args;
}

void mr_set_topic_callback(mrSession* session, mrOnTopicFunc on_topic_func, void* args)
{
    session->on_topic = on_topic_func;
    session->on_topic_args = args;
}

mrStreamId mr_create_output_best_effort_stream(mrSession* session, uint8_t* buffer, size_t size)
{
    uint8_t header_offset = session_header_offset(&session->info);
    return add_output_best_effort_buffer(&session->streams, buffer, size, header_offset);
}

mrStreamId mr_create_output_reliable_stream(mrSession* session, uint8_t* buffer, size_t size, size_t history)
{
    uint8_t header_offset = session_header_offset(&session->info);
    return add_output_reliable_buffer(&session->streams, buffer, size, history, header_offset);
}

mrStreamId mr_create_input_best_effort_stream(mrSession* session)
{
    return add_input_best_effort_buffer(&session->streams);
}

mrStreamId mr_create_input_reliable_stream(mrSession* session, uint8_t* buffer, size_t size, size_t history)
{
    return add_input_reliable_buffer(&session->streams, buffer, size, history);
}

uint16_t mr_init_base_object_request(mrSession* session, mrObjectId object_id, BaseObjectRequest* base)
{
    uint16_t request_id = generate_request_id(session);

    base->request_id.data[0] = (uint8_t) (request_id >> 8);
    base->request_id.data[1] = (uint8_t) request_id;
    object_id_to_raw(object_id, base->object_id.data);

    return request_id;
}

void mr_run_session_until_timeout(mrSession* session, int timeout_ms)
{
    flash_output_streams(session);

    bool timeout = false;
    while(!timeout)
    {
         timeout = !listen_message_reliably(session, timeout_ms);
    }
}

bool mr_run_session_until_confirm_delivery(mrSession* session, int timeout_ms)
{
    flash_output_streams(session);

    bool timeout = false;
    while(!output_streams_confirmed(&session->streams) && !timeout)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
    }

    return output_streams_confirmed(&session->streams);
}

bool mr_run_session_until_status(mrSession* session, int timeout_ms, const uint16_t* request_list, uint8_t* status_list, size_t list_size)
{
    flash_output_streams(session);

    for(unsigned i = 0; i < list_size; ++i)
    {
        status_list[i] = MR_STATUS_NONE;
    }

    session->request_list = request_list;
    session->status_list = status_list;
    session->request_status_list_size = list_size;

    bool timeout = false;
    bool status_confirmed = false;
    while(!timeout && !status_confirmed)
    {
        timeout = !listen_message_reliably(session, timeout_ms);
        status_confirmed = true;
        for(unsigned i = 0; i < list_size && status_confirmed; ++i)
        {
            status_confirmed = status_list[i] != MR_STATUS_NONE
                            || request_list[i] == MR_INVALID_REQUEST_ID; //CHECK: better give an error? an assert?
        }
    }

    session->request_status_list_size = 0;

    return mr_check_status_list_ok(status_list, list_size);
}

bool mr_check_status_list_ok(uint8_t* status_list, size_t size)
{
    bool all_status_ok = true;
    for(unsigned i = 0; i < size && all_status_ok; ++i)
    {
        all_status_ok = status_list[i] == MR_STATUS_OK;
    }

    return all_status_ok;
}

//==================================================================
//                             PRIVATE
//==================================================================
inline uint16_t generate_request_id(mrSession* session)
{
    uint16_t last_request_id = (UINT16_MAX == session->last_request_id)
        ? INITIAL_REQUEST_ID
        : session->last_request_id;

    session->last_request_id = last_request_id + 1;
    return last_request_id;
}

void flash_output_streams(mrSession* session)
{
    for(unsigned i = 0; i < session->streams.output_best_effort_size; ++i)
    {
        mrOutputBestEffortStream* stream = &session->streams.output_best_effort[i];
        mrStreamId id = mr_stream_id(i, MR_BEST_EFFORT_STREAM, MR_OUTPUT_STREAM);

        uint8_t* buffer; size_t length; mrSeqNum seq_num;
        if(prepare_best_effort_buffer_to_send(stream, &buffer, &length, &seq_num))
        {
            stamp_session_header(&session->info, id.raw, seq_num, buffer);
            send_message(session, buffer, length);
        }
    }

    for(unsigned i = 0; i < session->streams.output_reliable_size; ++i)
    {
        mrOutputReliableStream* stream = &session->streams.output_reliable[i];
        mrStreamId id = mr_stream_id(i, MR_RELIABLE_STREAM, MR_OUTPUT_STREAM);

        uint8_t* buffer; size_t length; mrSeqNum seq_num;
        while(prepare_next_reliable_buffer_to_send(stream, &buffer, &length, &seq_num))
        {
            stamp_session_header(&session->info, id.raw, seq_num, buffer);
            send_message(session, buffer, length);
        }
    }
}

bool listen_message(mrSession* session, int poll_ms)
{
    uint8_t* data; size_t length;
    bool must_be_read = recv_message(session, &data, &length, poll_ms);
    if(must_be_read)
    {
        MicroBuffer mb;
        init_micro_buffer(&mb, data, length);
        read_message(session, &mb);
    }

    return must_be_read;
}

bool listen_message_reliably(mrSession* session, int poll_ms)
{
    bool received = false;
    int32_t poll = (poll_ms >= 0) ? poll_ms : INT32_MAX;
    do
    {
        int64_t next_heartbeat_timestamp = INT64_MAX;
        int64_t timestamp = get_milli_time();
        for(unsigned i = 0; i < session->streams.output_reliable_size; ++i)
        {
            mrOutputReliableStream* stream = &session->streams.output_reliable[i];
            mrStreamId id = mr_stream_id(i, MR_RELIABLE_STREAM, MR_OUTPUT_STREAM);

            if(update_output_stream_heartbeat_timestamp(stream, timestamp))
            {
                write_submessage_heartbeat(session, id);
            }

            if(stream->next_heartbeat_timestamp < next_heartbeat_timestamp)
            {
                next_heartbeat_timestamp = stream->next_heartbeat_timestamp;
            }
        }

        int32_t poll_to_next_heartbeat = (next_heartbeat_timestamp != INT64_MAX) ? (int32_t)(next_heartbeat_timestamp - timestamp) : poll;
        if(0 == poll_to_next_heartbeat)
        {
            poll_to_next_heartbeat = 1;
        }

        int poll_choosen = (poll_to_next_heartbeat < poll) ? (int)poll_to_next_heartbeat : (int)poll;
        received = listen_message(session, poll_choosen);
        if(!received)
        {
            poll -= poll_choosen;
        }
    }
    while(!received && poll > 0);

    return received;
}

bool wait_session_status(mrSession* session, uint8_t* buffer, size_t length, size_t attempts)
{
    int poll_ms = MIN_SESSION_STATUS_WAITING;
    for(size_t i = 0; i < attempts && session_info_pending_request(&session->info); ++i)
    {
        send_message(session, buffer, length);
        poll_ms = listen_message(session, poll_ms) ? MIN_SESSION_STATUS_WAITING : poll_ms * 2;
    }

    return !session_info_pending_request(&session->info);
}

inline void send_message(const mrSession* session, uint8_t* buffer, size_t length)
{
    (void) session->comm->send_msg(session->comm->instance, buffer, length);
    DEBUG_PRINT_MESSAGE(SEND, buffer, length);
}

inline bool recv_message(const mrSession* session, uint8_t**buffer, size_t* length, int poll_ms)
{
    bool received = session->comm->recv_msg(session->comm->instance, buffer, length, poll_ms);
    if(received)
    {
        DEBUG_PRINT_MESSAGE(RECV, *buffer, *length);
    }
    return received;
}

void write_submessage_heartbeat(const mrSession* session, mrStreamId id)
{
    uint8_t heartbeat_buffer[HEARTBEAT_MAX_MSG_SIZE];
    MicroBuffer mb;
    init_micro_buffer_offset(&mb, heartbeat_buffer, HEARTBEAT_MAX_MSG_SIZE, session_header_offset(&session->info));

    const mrOutputReliableStream* stream = &session->streams.output_reliable[id.index];

    write_heartbeat(stream, &mb);
    stamp_session_header(&session->info, 0, id.raw, mb.init);
    send_message(session, heartbeat_buffer, micro_buffer_length(&mb));
}

void write_submessage_acknack(const mrSession* session, mrStreamId id)
{
    uint8_t acknack_buffer[ACKNACK_MAX_MSG_SIZE];
    MicroBuffer mb;
    init_micro_buffer_offset(&mb, acknack_buffer, ACKNACK_MAX_MSG_SIZE, session_header_offset(&session->info));

    const mrInputReliableStream* stream = &session->streams.input_reliable[id.index];

    write_acknack(stream, &mb);
    stamp_session_header(&session->info, 0, id.raw, mb.init);
    send_message(session, acknack_buffer, micro_buffer_length(&mb));
}

void read_message(mrSession* session, MicroBuffer* mb)
{
    uint8_t stream_id_raw; uint16_t seq_num;
    if(read_session_header(&session->info, mb, &stream_id_raw, &seq_num))
    {
        mrStreamId id = mr_stream_id_from_raw(stream_id_raw, MR_INPUT_STREAM);
        read_stream(session, mb, id, seq_num);
    }
}

void read_stream(mrSession* session, MicroBuffer* mb, mrStreamId stream_id, uint16_t seq_num)
{
    switch(stream_id.type)
    {
        case MR_NONE_STREAM:
        {
            stream_id = mr_stream_id_from_raw(seq_num, MR_INPUT_STREAM); // The real stream_id is into seq_num
            read_submessage_list(session, mb, stream_id);
            break;
        }
        case MR_BEST_EFFORT_STREAM:
        {
            mrInputBestEffortStream* stream = get_input_best_effort_stream(&session->streams, stream_id.index);
            if(stream && receive_best_effort_message(stream, seq_num))
            {
                read_submessage_list(session, mb, stream_id);
            }
            break;
        }
        case MR_RELIABLE_STREAM:
        {
            mrInputReliableStream* stream = get_input_reliable_stream(&session->streams, stream_id.index);
            if(stream && receive_reliable_message(stream, seq_num, mb->iterator, micro_buffer_length(mb)))
            {
                read_submessage_list(session, mb, stream_id);
                MicroBuffer next_mb;
                while(next_input_reliable_buffer_available(stream, &next_mb))
                {
                    read_submessage_list(session, &next_mb, stream_id);
                }
                write_submessage_acknack(session, stream_id);
            }
            break;
        }
        default:
            break;
    }
}

void read_submessage_list(mrSession* session, MicroBuffer* submessages, mrStreamId stream_id)
{
    uint8_t id; uint16_t length; uint8_t flags; uint8_t* payload_it = NULL;
    while(read_submessage_header(submessages, &id, &length, &flags, &payload_it))
    {
        read_submessage(session, submessages, id, stream_id, length, flags);
    }
}

void read_submessage(mrSession* session, MicroBuffer* submessage, uint8_t submessage_id, mrStreamId stream_id, uint16_t length, uint8_t flags)
{
    switch(submessage_id)
    {
        case SUBMESSAGE_ID_STATUS_AGENT:
            if(stream_id.type == MR_NONE_STREAM)
            {
                read_create_session_status(&session->info, submessage);
            }
            break;

        case SUBMESSAGE_ID_STATUS:
            if(stream_id.type == MR_NONE_STREAM)
            {
                read_delete_session_status(&session->info, submessage);
            }
            else
            {
                read_submessage_status(session, submessage);
            }
            break;

        case SUBMESSAGE_ID_DATA:
            read_submessage_data(session, submessage, length, stream_id, flags & FORMAT_MASK);
            break;

        case SUBMESSAGE_ID_FRAGMENT:
            read_submessage_fragment(session, submessage, stream_id, 0 != (flags & FLAG_LAST_FRAGMENT));
            break;

        case SUBMESSAGE_ID_HEARTBEAT:
            read_submessage_heartbeat(session, submessage, stream_id);
            break;

        case SUBMESSAGE_ID_ACKNACK:
            read_submessage_acknack(session, submessage, stream_id);
            break;

        default:
            break;
    }
}

void read_submessage_status(mrSession* session, MicroBuffer* submessage)
{
    STATUS_Payload payload;
    deserialize_STATUS_Payload(submessage, &payload);

    mrObjectId object_id = mr_object_id_from_raw(payload.base.related_request.object_id.data);
    uint16_t request_id = (((uint16_t) payload.base.related_request.request_id.data[0]) << 8)
                            + payload.base.related_request.request_id.data[1];

    uint8_t status = payload.base.result.status;
    process_status(session, object_id, request_id, status);
}


extern void read_submessage_format(mrSession* session, MicroBuffer* data, uint16_t length, uint8_t format,
                                   mrStreamId stream_id, mrObjectId object_id, uint16_t request_id);

void read_submessage_data(mrSession* session, MicroBuffer* submessage, uint16_t length, mrStreamId stream_id, uint8_t format)
{
#ifdef PROFILE_READ_ACCESS
    BaseObjectRequest base;
    deserialize_BaseObjectRequest(submessage, &base);
    length -= 4; //CHANGE: by a future size_of_BaseObjectRequest

    mrObjectId object_id = mr_object_id_from_raw(base.object_id.data);
    uint16_t request_id = (((uint16_t) base.request_id.data[0]) << 8) + base.request_id.data[1];

    process_status(session, object_id, request_id, STATUS_OK);

    if(session->on_topic != NULL)
    {
        read_submessage_format(session, submessage, length, format, stream_id, object_id, request_id);
    }
#endif
}

void read_submessage_fragment(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id, bool last_fragment)
{
    (void) session; (void) submessage; (void) stream_id; (void) last_fragment;
    //TODO
}

void read_submessage_heartbeat(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id)
{
    mrInputReliableStream* stream = get_input_reliable_stream(&session->streams, stream_id.index);
    if(stream)
    {
        read_heartbeat(stream, submessage);
        write_submessage_acknack(session, stream_id);
    }
}

void read_submessage_acknack(mrSession* session, MicroBuffer* submessage, mrStreamId stream_id)
{
    mrOutputReliableStream* stream = get_output_reliable_stream(&session->streams, stream_id.index);
    if(stream)
    {
        read_acknack(stream, submessage);

        uint8_t* buffer; size_t length;
        mrSeqNum seq_num_it = begin_output_nack_buffer_it(stream);
        if(next_reliable_nack_buffer_to_send(stream, &buffer, &length, &seq_num_it))
        {
            send_message(session, buffer, length);
        }
    }
}

void process_status(mrSession* session, mrObjectId object_id, int16_t request_id, uint8_t status)
{
    if(session->on_status != NULL)
    {
        session->on_status(session, object_id, request_id, status, session->on_status_args);
    }

    for(unsigned i = 0; i < session->request_status_list_size; ++i)
    {
        if(request_id == session->request_list[i])
        {
            session->status_list[i] = status;
            break;
        }
    }
}