#ifndef KVX_PROTOCOL_H
#define KVX_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum proto_error_t
{
    PROTO_OK,

    PROTO_ERR_INCOMPLETE,
    PROTO_ERR_INVALID_COMMAND,
    PROTO_ERR_INVALID_ARGUMENTS,

    PROTO_ERR_KEY_TOO_LONG,
    PROTO_ERR_INVALID_FLAGS,
    PROTO_ERR_INVALID_EXPTIME,
    PROTO_ERR_INVALID_VALUE_LENGTH,
    PROTO_ERR_INVALID_CAS,

    PROTO_ERR_EXPECTED_CRLF,
    PROTO_ERR_INVALID_DATA_BLOCK,

    // Response writers only: the supplied destination slice is too small.
    PROTO_ERR_BUFFER_TOO_SMALL,
} proto_error_t;

typedef enum command_type_t
{
    CMD_GET,
    CMD_SET,
    CMD_DELETE,
    CMD_TOUCH,
    CMD_VERSION,
    CMD_QUIT,
} command_type_t;

typedef struct command_t
{
    const uint8_t *key;
    size_t key_len;
    const uint8_t *keys_end;

    const uint8_t *value;
    size_t value_len;

    uint32_t flags;
    uint32_t exptime;
    uint64_t cas;

    bool noreply;
    command_type_t type;
} command_t;

/*
 * Parse one command without modifying b or allocating memory.
 * b_len: valid unread bytes available starting at b, not allocated capacity.
 *
 * PROTO_OK: cmd contains borrowed slices into b; consumed_out is frame size.
 * PROTO_ERR_INCOMPLETE: retain bytes and receive more data.
 * All failures leave cmd unchanged and set consumed_out to zero.
 *
 * b must provide b_len readable bytes. Outputs must be valid and non-overlapping
 * with each other and b. Keep b alive and unchanged while using cmd's slices.
 * Call with b + offset and rpos - offset, where 0 <= offset <= rpos.
 * The connection layer handles buffer growth/compaction and size limits.
 */
proto_error_t protocol_parse_command(uint8_t *b, size_t b_len, command_t *cmd, size_t *consumed_out);
bool protocol_next_key(command_t *cmd);

typedef enum response_type_t
{
    RESP_STORED,
    RESP_NOT_STORED,
    RESP_DELETED,
    RESP_NOT_FOUND,
    RESP_TOUCHED,
    RESP_ERROR,
    RESP_CLIENT_ERROR,
    RESP_SERVER_ERROR,
} response_type_t;

/*
 * Response writers allocate nothing and append no NUL terminator.
 * b_cap is the writable capacity starting at b, usually w_cap - w_pos.
 * On success, written_out includes all response bytes and CRLF framing.
 * On any error, the destination is unchanged and written_out is zero.
 * All input slices must remain valid during the call and must not overlap the
 * destination or written_out. written_out must be a valid separate pointer.
 * The worker handles noreply, flushing, resizing, and command ordering.
 */

// VALUE <key> <flags> <bytes>\r\n<value>\r\n.
proto_error_t protocol_write_get_result(
    uint8_t *b, size_t b_cap,
    const uint8_t *key, size_t key_len,
    uint32_t flags,
    const uint8_t *value, size_t value_len,
    size_t *written_out);

proto_error_t protocol_write_get_end(
    uint8_t *b, size_t b_cap, size_t *written_out);

// STORED, NOT_STORED, DELETED, NOT_FOUND, TOUCHED, or ERROR.
proto_error_t protocol_write_response(
    uint8_t *b, size_t b_cap,
    response_type_t type, size_t *written_out);

// VERSION <version>\r\n
proto_error_t protocol_write_version(
    uint8_t *b, size_t b_cap,
    const uint8_t *version, size_t version_len,
    size_t *written_out);

// type must be RESP_CLIENT_ERROR or RESP_SERVER_ERROR.
proto_error_t protocol_write_error(
    uint8_t *b, size_t b_cap,
    response_type_t type,
    const uint8_t *message, size_t message_len,
    size_t *written_out);

#endif
