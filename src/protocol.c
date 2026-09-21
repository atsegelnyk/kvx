#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PROTO_MAX_TOKENS 6
#define PROTO_MAX_KEY_LENGTH 250

typedef struct proto_token_t
{
    const uint8_t *data;
    size_t len;
} proto_token_t;

typedef struct proto_command_spec_t
{
    command_type_t type;
    size_t token_count; // Includes command name, excludes noreply.
    bool allows_noreply;
} proto_command_spec_t;

static const proto_command_spec_t proto_commands[] = {
    [CMD_GET] = {CMD_GET, 2, false},
    [CMD_SET] = {CMD_SET, 5, true},
    [CMD_DELETE] = {CMD_DELETE, 2, true},
    [CMD_TOUCH] = {CMD_TOUCH, 3, true},
    [CMD_VERSION] = {CMD_VERSION, 1, false},
    [CMD_QUIT] = {CMD_QUIT, 1, false},
};

static bool proto_is_separator(uint8_t ch)
{
    return ch == ' ' || ch == '\t';
}

static bool proto_parse_uint(
    proto_token_t token,
    uint64_t limit,
    uint64_t *out)
{
    if (token.len == 0)
        return false;

    uint64_t value = 0;

    for (size_t i = 0; i < token.len; i++)
    {
        uint8_t ch = token.data[i];

        if (ch < '0' || ch > '9')
            return false;

        uint64_t digit = ch - '0';

        if (value > limit / 10 ||
            (value == limit / 10 && digit > limit % 10))
        {
            return false;
        }

        value = value * 10 + digit;
    }

    *out = value;
    return true;
}

static const proto_command_spec_t *proto_find_command(proto_token_t name)
{
    switch (name.len)
    {
    case 3:
        if (memcmp(name.data, "get", 3) == 0)
            return &proto_commands[CMD_GET];

        if (memcmp(name.data, "set", 3) == 0)
            return &proto_commands[CMD_SET];

        break;

    case 4:
        if (memcmp(name.data, "quit", 4) == 0)
            return &proto_commands[CMD_QUIT];

        break;

    case 5:
        if (memcmp(name.data, "touch", 5) == 0)
            return &proto_commands[CMD_TOUCH];

        break;

    case 6:
        if (memcmp(name.data, "delete", 6) == 0)
            return &proto_commands[CMD_DELETE];

        break;

    case 7:
        if (memcmp(name.data, "version", 7) == 0)
            return &proto_commands[CMD_VERSION];

        break;
    }

    return NULL;
}

static proto_error_t proto_find_line(
    const uint8_t *b,
    size_t len,
    size_t *line_len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (b[i] == '\n')
            return PROTO_ERR_EXPECTED_CRLF;

        if (b[i] != '\r')
            continue;

        if (len - i < 2)
            return PROTO_ERR_INCOMPLETE;

        if (b[i + 1] != '\n')
            return PROTO_ERR_EXPECTED_CRLF;

        *line_len = i;
        return PROTO_OK;
    }

    return PROTO_ERR_INCOMPLETE;
}

static proto_error_t proto_tokenize(
    const uint8_t *line,
    size_t len,
    proto_token_t *tokens,
    size_t capacity,
    size_t *count_out)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < len)
    {
        while (pos < len && proto_is_separator(line[pos]))
            pos++;

        if (pos == len)
            break;

        if (count == capacity)
            return PROTO_ERR_INVALID_ARGUMENTS;

        size_t start = pos;

        while (pos < len && !proto_is_separator(line[pos]))
            pos++;

        tokens[count++] = (proto_token_t){
            .data = line + start,
            .len = pos - start,
        };
    }

    *count_out = count;
    return PROTO_OK;
}

static proto_error_t proto_parse_noreply(
    const proto_command_spec_t *spec,
    const proto_token_t *tokens,
    size_t count,
    command_t *cmd)
{
    if (count == spec->token_count)
        return PROTO_OK;

    if (!spec->allows_noreply ||
        count != spec->token_count + 1 ||
        tokens[count - 1].len != 7 ||
        memcmp(tokens[count - 1].data, "noreply", 7) != 0)
    {
        return PROTO_ERR_INVALID_ARGUMENTS;
    }

    cmd->noreply = true;
    return PROTO_OK;
}

static proto_error_t proto_parse_key(proto_token_t token, command_t *cmd)
{
    if (token.len == 0)
        return PROTO_ERR_INVALID_ARGUMENTS;

    if (token.len > PROTO_MAX_KEY_LENGTH)
        return PROTO_ERR_KEY_TOO_LONG;

    for (size_t i = 0; i < token.len; i++)
    {
        if (token.data[i] <= 0x20 || token.data[i] == 0x7f)
            return PROTO_ERR_INVALID_ARGUMENTS;
    }

    cmd->key = token.data;
    cmd->key_len = token.len;
    cmd->keys_end = token.data + token.len;

    return PROTO_OK;
}

static proto_error_t proto_parse_exptime(proto_token_t token, command_t *cmd)
{
    uint64_t value;

    if (!proto_parse_uint(token, UINT32_MAX, &value))
        return PROTO_ERR_INVALID_EXPTIME;

    cmd->exptime = (uint32_t)value;
    return PROTO_OK;
}

static proto_error_t proto_parse_set_fields(
    const proto_token_t *tokens,
    command_t *cmd)
{
    uint64_t value;

    if (!proto_parse_uint(tokens[2], UINT32_MAX, &value))
        return PROTO_ERR_INVALID_FLAGS;

    cmd->flags = (uint32_t)value;

    proto_error_t err = proto_parse_exptime(tokens[3], cmd);

    if (err != PROTO_OK)
        return err;

    if (!proto_parse_uint(tokens[4], SIZE_MAX, &value))
        return PROTO_ERR_INVALID_VALUE_LENGTH;

    cmd->value_len = (size_t)value;
    return PROTO_OK;
}

static proto_error_t proto_parse_get_keys(
    const uint8_t *b, size_t len, command_t *cmd)
{
    size_t pos = 0;

    while (pos < len)
    {
        while (pos < len && proto_is_separator(b[pos]))
            pos++;

        if (pos == len)
            break;

        size_t start = pos;

        while (pos < len && !proto_is_separator(b[pos]))
            pos++;

        command_t current = {0};
        proto_token_t token = {b + start, pos - start};
        proto_error_t err = proto_parse_key(token, &current);

        if (err != PROTO_OK)
            return err;

        if (cmd->key == NULL)
        {
            cmd->key = current.key;
            cmd->key_len = current.key_len;
        }
    }

    if (cmd->key == NULL)
        return PROTO_ERR_INVALID_ARGUMENTS;

    cmd->type = CMD_GET;
    cmd->keys_end = b + len;
    return PROTO_OK;
}

static proto_error_t proto_parse_header(
    const uint8_t *line,
    size_t len,
    command_t *cmd)
{
    // GET has an arbitrary number of keys, so bypass the fixed token array.
    size_t start = 0;
    while (start < len && proto_is_separator(line[start]))
        start++;

    if (len - start >= 3 && memcmp(line + start, "get", 3) == 0 &&
        (len - start == 3 || proto_is_separator(line[start + 3])))
    {
        return proto_parse_get_keys(line + start + 3, len - start - 3, cmd);
    }

    proto_token_t tokens[PROTO_MAX_TOKENS];
    size_t count;

    proto_error_t err = proto_tokenize(
        line, len, tokens, PROTO_MAX_TOKENS, &count);

    if (err != PROTO_OK)
        return err;

    if (count == 0)
        return PROTO_ERR_INVALID_COMMAND;

    const proto_command_spec_t *spec = proto_find_command(tokens[0]);

    if (spec == NULL)
        return PROTO_ERR_INVALID_COMMAND;

    cmd->type = spec->type;

    err = proto_parse_noreply(spec, tokens, count, cmd);

    if (err != PROTO_OK)
        return err;

    if (cmd->type == CMD_VERSION || cmd->type == CMD_QUIT)
        return PROTO_OK;

    err = proto_parse_key(tokens[1], cmd);

    if (err != PROTO_OK)
        return err;

    switch (cmd->type)
    {
    case CMD_SET:
        return proto_parse_set_fields(tokens, cmd);

    case CMD_TOUCH:
        return proto_parse_exptime(tokens[2], cmd);

    default:
        return PROTO_OK;
    }
}

static proto_error_t proto_parse_body(
    const uint8_t *b,
    size_t len,
    command_t *cmd,
    size_t *consumed)
{
    size_t offset = *consumed;
    size_t value_len = cmd->value_len;

    // Ensure the complete frame length is representable.
    if (SIZE_MAX - offset < 2 ||
        value_len > SIZE_MAX - offset - 2)
    {
        return PROTO_ERR_INVALID_VALUE_LENGTH;
    }

    size_t remaining = len - offset;

    if (remaining < value_len)
        return PROTO_ERR_INCOMPLETE;

    size_t value_end = offset + value_len;
    size_t trailing = remaining - value_len;

    if (trailing == 0)
        return PROTO_ERR_INCOMPLETE;

    if (b[value_end] != '\r')
        return PROTO_ERR_INVALID_DATA_BLOCK;

    if (trailing == 1)
        return PROTO_ERR_INCOMPLETE;

    if (b[value_end + 1] != '\n')
        return PROTO_ERR_INVALID_DATA_BLOCK;

    cmd->value = b + offset;
    *consumed = value_end + 2;

    return PROTO_OK;
}

/*
 * Parse one command without modifying the input buffer.
 *
 * On success, cmd contains borrowed slices into b and consumed_out includes
 * all framing bytes. On failure, cmd is unchanged and consumed_out is zero.
 * INCOMPLETE means the caller should retain the bytes and receive more data.
 * Other errors do not provide a stream recovery offset.
 *
 * cmd and consumed_out must be valid, non-overlapping output pointers.
 * b must reference b_len readable bytes and must not overlap the outputs.
 * b_len is the number of valid unread bytes, not the allocated capacity.
 * No NUL terminator is required. Keep b alive while using cmd's slices.
 *
 * GET supports multiple keys, all validated before success; expiration values
 * are unsigned. Keep the command's input bytes until key iteration finishes.
 * Expiration conversion and configured input/item size limits belong to the
 * caller. CAS is unused because the supported command set has no cas command.
 */
proto_error_t protocol_parse_command(
    uint8_t *b,
    size_t b_len,
    command_t *cmd,
    size_t *consumed_out)
{
    *consumed_out = 0;

    size_t line_len;
    proto_error_t err = proto_find_line(b, b_len, &line_len);

    if (err != PROTO_OK)
        return err;

    command_t parsed = {0};

    err = proto_parse_header(b, line_len, &parsed);

    if (err != PROTO_OK)
        return err;

    size_t consumed = line_len + 2;

    if (parsed.type == CMD_SET)
    {
        err = proto_parse_body(b, b_len, &parsed, &consumed);

        if (err != PROTO_OK)
            return err;
    }

    *cmd = parsed;
    *consumed_out = consumed;

    return PROTO_OK;
}

bool protocol_next_key(command_t *cmd)
{
    if (cmd->key == NULL || cmd->keys_end == NULL)
        return false;

    const uint8_t *p = cmd->key + cmd->key_len;

    while (p < cmd->keys_end && proto_is_separator(*p))
        p++;

    if (p == cmd->keys_end)
        return false;

    const uint8_t *start = p;

    while (p < cmd->keys_end && !proto_is_separator(*p))
        p++;

    cmd->key = start;
    cmd->key_len = (size_t)(p - start);
    return true;
}

// Format backwards into caller-provided scratch space; at least one digit.
static uint8_t *proto_format_uint(uint8_t *end, uintmax_t value)
{
    do
    {
        *--end = (uint8_t)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    return end;
}

proto_error_t protocol_write_get_result(
    uint8_t *b, size_t b_cap,
    const uint8_t *key, size_t key_len,
    uint32_t flags,
    const uint8_t *value, size_t value_len,
    size_t *written_out)
{
    *written_out = 0;

    if (key == NULL || key_len == 0 || (value == NULL && value_len != 0))
        return PROTO_ERR_INVALID_ARGUMENTS;

    if (key_len > PROTO_MAX_KEY_LENGTH)
        return PROTO_ERR_KEY_TOO_LONG;

    uint8_t flags_buf[10];
    uint8_t length_buf[sizeof(size_t) * 3];
    uint8_t *flags_end = flags_buf + sizeof(flags_buf);
    uint8_t *length_end = length_buf + sizeof(length_buf);
    uint8_t *flags_start = proto_format_uint(flags_end, flags);
    uint8_t *length_start = proto_format_uint(length_end, value_len);
    size_t flags_len = (size_t)(flags_end - flags_start);
    size_t length_len = (size_t)(length_end - length_start);

    // VALUE + space + key + space + flags + space + length + CRLF.
    size_t header_len = 6 + key_len + 1 + flags_len + 1 + length_len + 2;

    if (value_len > SIZE_MAX - header_len - 2)
        return PROTO_ERR_INVALID_VALUE_LENGTH;

    size_t total = header_len + value_len + 2;

    if (b_cap < total)
        return PROTO_ERR_BUFFER_TOO_SMALL;

    if (b == NULL)
        return PROTO_ERR_INVALID_ARGUMENTS;

    uint8_t *p = b;
    memcpy(p, "VALUE ", 6);
    p += 6;
    memcpy(p, key, key_len);
    p += key_len;
    *p++ = ' ';
    memcpy(p, flags_start, flags_len);
    p += flags_len;
    *p++ = ' ';
    memcpy(p, length_start, length_len);
    p += length_len;
    memcpy(p, "\r\n", 2);
    p += 2;

    if (value_len != 0)
    {
        memcpy(p, value, value_len);
        p += value_len;
    }

    memcpy(p, "\r\n", 2);
    *written_out = total;
    return PROTO_OK;
}

static proto_error_t proto_write_bytes(
    uint8_t *b, size_t b_cap,
    const uint8_t *bytes, size_t len,
    size_t *written_out)
{
    *written_out = 0;

    if (b_cap < len)
        return PROTO_ERR_BUFFER_TOO_SMALL;

    if (b == NULL)
        return PROTO_ERR_INVALID_ARGUMENTS;

    memcpy(b, bytes, len);
    *written_out = len;
    return PROTO_OK;
}

proto_error_t protocol_write_get_end(
    uint8_t *b, size_t b_cap, size_t *written_out)
{
    static const uint8_t response[] = "END\r\n";
    return proto_write_bytes(b, b_cap, response, sizeof(response) - 1, written_out);
}

proto_error_t protocol_write_response(
    uint8_t *b, size_t b_cap,
    response_type_t type, size_t *written_out)
{
    static const uint8_t responses[][13] = {
        [RESP_STORED] = "STORED\r\n",
        [RESP_NOT_STORED] = "NOT_STORED\r\n",
        [RESP_DELETED] = "DELETED\r\n",
        [RESP_NOT_FOUND] = "NOT_FOUND\r\n",
        [RESP_TOUCHED] = "TOUCHED\r\n",
        [RESP_ERROR] = "ERROR\r\n",
    };
    static const size_t lengths[] = {
        [RESP_STORED] = 8,
        [RESP_NOT_STORED] = 12,
        [RESP_DELETED] = 9,
        [RESP_NOT_FOUND] = 11,
        [RESP_TOUCHED] = 9,
        [RESP_ERROR] = 7,
    };

    *written_out = 0;

    if ((unsigned)type >= sizeof(responses) / sizeof(responses[0]))
        return PROTO_ERR_INVALID_ARGUMENTS;

    return proto_write_bytes(b, b_cap, responses[type], lengths[type], written_out);
}

static proto_error_t proto_write_text_line(
    uint8_t *b, size_t b_cap,
    const uint8_t *prefix, size_t prefix_len,
    const uint8_t *text, size_t text_len,
    size_t *written_out)
{
    *written_out = 0;

    if (text == NULL || text_len == 0 || text_len > SIZE_MAX - prefix_len - 2)
        return PROTO_ERR_INVALID_ARGUMENTS;

    size_t total = prefix_len + text_len + 2;

    if (b_cap < total)
        return PROTO_ERR_BUFFER_TOO_SMALL;

    if (b == NULL)
        return PROTO_ERR_INVALID_ARGUMENTS;

    for (size_t i = 0; i < text_len; i++)
    {
        if (text[i] == '\r' || text[i] == '\n' || text[i] == '\0')
            return PROTO_ERR_INVALID_ARGUMENTS;
    }

    memcpy(b, prefix, prefix_len);
    memcpy(b + prefix_len, text, text_len);
    memcpy(b + prefix_len + text_len, "\r\n", 2);
    *written_out = total;
    return PROTO_OK;
}

proto_error_t protocol_write_version(
    uint8_t *b, size_t b_cap,
    const uint8_t *version, size_t version_len,
    size_t *written_out)
{
    static const uint8_t prefix[] = "VERSION ";
    return proto_write_text_line(b, b_cap, prefix, sizeof(prefix) - 1,
                                 version, version_len, written_out);
}

proto_error_t protocol_write_error(
    uint8_t *b, size_t b_cap,
    response_type_t type,
    const uint8_t *message, size_t message_len,
    size_t *written_out)
{
    static const uint8_t client_prefix[] = "CLIENT_ERROR ";
    static const uint8_t server_prefix[] = "SERVER_ERROR ";
    *written_out = 0;

    if (type == RESP_CLIENT_ERROR)
        return proto_write_text_line(b, b_cap, client_prefix, sizeof(client_prefix) - 1,
                                     message, message_len, written_out);

    if (type == RESP_SERVER_ERROR)
        return proto_write_text_line(b, b_cap, server_prefix, sizeof(server_prefix) - 1,
                                     message, message_len, written_out);

    return PROTO_ERR_INVALID_ARGUMENTS;
}
