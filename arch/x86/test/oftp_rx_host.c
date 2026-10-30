#include "../kernel/oftp_rx.h"
extern int printf(const char *, ...);

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); failures++; \
} } while (0)

static uint8_t buffer[8192];
static uint8_t packet[16 + 1400];

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint8_t expected(uint32_t offset)
{
    return (uint8_t)(offset * 37 + offset / 251 + 11);
}

static oftp_rx_t start(uint32_t capacity)
{
    memset(buffer, 0xa5, sizeof(buffer));
    oftp_rx_t rx = { .buf = buffer, .capacity = capacity };
    return rx;
}

static void make_packet(uint32_t total, uint32_t offset, uint32_t chunk)
{
    memcpy(packet, "OFTD", 4);
    be32(packet + 4, total);
    be32(packet + 8, offset);
    be32(packet + 12, chunk);
    for (uint32_t i = 0; i < chunk && i < 1400; i++)
        packet[16 + i] = expected(offset + i);
}

static bool receive(oftp_rx_t *rx, uint32_t total, uint32_t offset, uint32_t chunk)
{
    make_packet(total, offset, chunk);
    return oftp_rx_packet(rx, packet, 16 + chunk);
}

static void verify(const oftp_rx_t *rx)
{
    CHECK(!rx->error && rx->done && rx->received == rx->total);
    for (uint32_t i = 0; i < rx->total; i++) CHECK(buffer[i] == expected(i));
    for (uint32_t i = rx->total; i < sizeof(buffer); i++) CHECK(buffer[i] == 0xa5);
}

static void loss_and_reorder(void)
{
    /* Lose each possible datagram, including the first and final chunk. */
    for (unsigned missing = 0; missing < 4; missing++) {
        oftp_rx_t rx = start(sizeof(buffer));
        for (unsigned i = 0; i < 4; i++) {
            if (i == missing) continue;
            receive(&rx, 4217, i * 1400, i == 3 ? 17 : 1400);
        }
        CHECK(!rx.done && !rx.error);
        CHECK(rx.received == missing * 1400);
        CHECK(!receive(&rx, 4217, 4217, 0));
        CHECK(!rx.done);
        /* Server response to OFTN at the first missing byte. */
        for (uint32_t off = rx.received; off < 4217; off += 1400)
            CHECK(receive(&rx, 4217, off, off == 4200 ? 17 : 1400));
        verify(&rx);
    }

    /* All permutations, plus an EOF marker arriving before the first byte. */
    for (unsigned a = 0; a < 3; a++) {
        for (unsigned b = 0; b < 3; b++) {
            if (a == b) continue;
            unsigned order[3] = { a, b, 3 - a - b };
            oftp_rx_t rx = start(sizeof(buffer));
            CHECK(receive(&rx, 4200, 4200, 0));
            CHECK(!rx.done && rx.received == 0);
            CHECK(!receive(&rx, 4200, 4200, 0));
            for (unsigned i = 0; i < 3; i++)
                receive(&rx, 4200, order[i] * 1400, 1400);
            uint32_t resume = rx.received;
            for (uint32_t off = resume; off < 4200; off += 1400)
                CHECK(receive(&rx, 4200, off, 1400));
            verify(&rx);
            CHECK(!receive(&rx, 4200, 4200, 0));
        }
    }
}

static void duplicates_and_overlap(void)
{
    oftp_rx_t rx = start(sizeof(buffer));
    CHECK(receive(&rx, 3200, 0, 1000));
    CHECK(!receive(&rx, 3200, 0, 1000));
    CHECK(rx.received == 1000 && !rx.done);
    CHECK(receive(&rx, 3200, 600, 1400));
    CHECK(rx.received == 2000);
    CHECK(!receive(&rx, 3200, 2500, 700));
    CHECK(!receive(&rx, 3200, 3200, 0));
    CHECK(rx.received == 2000 && !rx.done);
    CHECK(receive(&rx, 3200, 1900, 1300));
    verify(&rx);
    CHECK(!oftp_rx_packet(&rx, "OFTE", 4));
    CHECK(!rx.error);
}

static void malformed(void)
{
    oftp_rx_t rx = start(64);
    for (uint32_t len = 0; len < 16; len++) {
        make_packet(64, 0, 16);
        CHECK(!oftp_rx_packet(&rx, packet, len));
    }
    make_packet(64, 0, 32);
    CHECK(!oftp_rx_packet(&rx, packet, 47));  /* truncated payload */
    CHECK(!oftp_rx_packet(&rx, packet, 49));  /* trailing garbage */
    make_packet(64, 60, 5);
    CHECK(!oftp_rx_packet(&rx, packet, 21));
    make_packet(64, 65, 0);
    CHECK(!oftp_rx_packet(&rx, packet, 16));
    make_packet(64, 0, 0);
    CHECK(!oftp_rx_packet(&rx, packet, 16));
    make_packet(64, UINT32_MAX, 2);
    CHECK(!oftp_rx_packet(&rx, packet, 18));
    make_packet(64, 1, UINT32_MAX);
    CHECK(!oftp_rx_packet(&rx, packet, 16));
    CHECK(!rx.done && !rx.error && !rx.total && !rx.received);
    for (unsigned i = 0; i < sizeof(buffer); i++) CHECK(buffer[i] == 0xa5);

    CHECK(receive(&rx, 65, 0, 1));
    CHECK(rx.error && !rx.done && !rx.received);
    CHECK(!receive(&rx, 64, 0, 64));  /* fatal error remains sticky */

    rx = start(64);
    CHECK(receive(&rx, 64, 0, 16));
    CHECK(receive(&rx, 63, 16, 16)); /* file size changed during transfer */
    CHECK(rx.error && rx.received == 16 && !rx.done);

    rx = start(64);
    CHECK(oftp_rx_packet(&rx, "OFTE not found", 14));
    CHECK(rx.error && !rx.done);
    rx = start(64);
    CHECK(receive(&rx, 0, 0, 0));
    CHECK(rx.error && !rx.done);
    rx = start(64);
    rx.buf = NULL;
    CHECK(!receive(&rx, 64, 0, 64));
}

static void varied_chunks(void)
{
    uint32_t seed = 1729;
    for (unsigned run = 0; run < 100; run++) {
        oftp_rx_t rx = start(sizeof(buffer));
        uint32_t total = 4096 + run;
        while (!rx.done) {
            seed = seed * 1664525 + 1013904223;
            uint32_t chunk = 1 + (seed % 1400);
            if (chunk > total - rx.received) chunk = total - rx.received;
            CHECK(receive(&rx, total, rx.received, chunk));
        }
        verify(&rx);
    }
}

int main(void)
{
    loss_and_reorder();
    duplicates_and_overlap();
    malformed();
    varied_chunks();
    printf("OFTP receiver: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
