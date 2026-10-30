#include "msi_format.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
#define FORMAT_LIMIT 1048576U

typedef struct { uint16_t *text; uint32_t length, capacity; } format_buffer_t;

static uint32_t format_append(format_buffer_t *buffer, const uint16_t *text, uint32_t count)
{
    if ((uint64_t)buffer->length + count >= FORMAT_LIMIT) return 8;
    uint32_t needed = buffer->length + count + 1;
    if (needed > buffer->capacity) {
        uint32_t capacity = buffer->capacity ? buffer->capacity : 64;
        while (capacity < needed) capacity *= 2;
        uint16_t *copy = kmalloc((uint64_t)capacity * 2);
        if (!copy) return 8;
        if (buffer->length) memcpy(copy, buffer->text, buffer->length * 2U);
        if (buffer->text) kfree(buffer->text);
        buffer->text = copy; buffer->capacity = capacity;
    }
    if (count) memcpy(buffer->text + buffer->length, text, count * 2U);
    buffer->length += count; buffer->text[buffer->length] = 0;
    return 0;
}

/* An escaped '[' or ']' is data, so it does not change bracket depth. */
static uint32_t format_closing(const uint16_t *text, uint32_t start, uint32_t end)
{
    unsigned depth = 1;
    for (uint32_t i = start + 1; i < end; i++) {
        if (text[i - 1] == '[' && text[i] == '\\' && i + 2 < end) {
            i += 2;
            while (i < end && text[i] != ']') i++;
            if (i == end) return end;
            if (!--depth) return i;
        } else if (text[i] == '[') depth++;
        else if (text[i] == ']' && !--depth) return i;
    }
    return end;
}

static uint32_t format_part(msi_package_t *package, const uint16_t *input, uint32_t start,
                            uint32_t end, format_buffer_t *output, unsigned depth)
{
    if (depth > 64) return 8;
    for (uint32_t i = start; i < end;) {
        if (input[i] == '{' || input[i] == '}') return 120;
        if (input[i] != '[') {
            uint32_t error = format_append(output, input + i++, 1);
            if (error) return error;
            continue;
        }
        uint32_t closing = format_closing(input, i, end);
        if (closing == end) return format_append(output, input + i, end - i);
        uint32_t count = closing - i - 1;
        const uint16_t *token = input + i + 1;
        uint32_t error = 0;
        if (count && token[0] == '\\') {
            error = count == 1 ? format_append(output, input + i, count + 2) : format_append(output, token + 1, 1);
        } else if (count == 1 && token[0] == '~') {
            const uint16_t nul = 0; error = format_append(output, &nul, 1);
        } else {
            format_buffer_t name = {0};
            error = format_part(package, input, i + 1, closing, &name, depth + 1);
            if (!error && name.length) {
                uint16_t first = name.text[0];
                if (first == '%' || first == '#' || first == '$' || first == '!' ||
                    (first >= '0' && first <= '9')) error = 120;
                else {
                    const uint16_t *value = msi_package_get(package, name.text);
                    uint32_t length = 0;
                    while (length < FORMAT_LIMIT && value[length]) length++;
                    if (length == FORMAT_LIMIT) error = 8;
                    else error = format_append(output, value, length);
                }
            }
            if (name.text) kfree(name.text);
        }
        if (error) return error;
        i = closing + 1;
    }
    return 0;
}

uint32_t msi_format_properties(msi_package_t *package, const uint16_t *input,
                               uint16_t **output, uint32_t *length)
{
    if (output) *output = NULL;
    if (length) *length = 0;
    if (!package || !input || !output || !length) return 87;
    uint32_t count = 0;
    while (count < FORMAT_LIMIT && input[count]) count++;
    if (count == FORMAT_LIMIT) return 8;
    format_buffer_t buffer = {0};
    uint32_t error = format_part(package, input, 0, count, &buffer, 0);
    if (!error) error = format_append(&buffer, NULL, 0);
    if (error) { if (buffer.text) kfree(buffer.text); return error; }
    *output = buffer.text; *length = buffer.length;
    return 0;
}
