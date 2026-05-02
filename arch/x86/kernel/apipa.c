/*
 * OsitoK x86-64 — IPv4 Link-Local autoconfiguration (RFC 3927).
 *
 * Cuando DHCP falla (o no hay DHCP server: cable directo Mac↔OsitoK,
 * router muerto, etc.), elegimos una IP del bloque 169.254.0.0/16
 * pseudo-aleatoriamente, ARP-probeamos para asegurar que nadie más la
 * tenga, y la asignamos.  Mac/Linux/Windows hacen lo mismo por default,
 * así que terminamos en el mismo /16 sin coordinación → ping y servicios
 * funcionan en cable directo sin tocar config en ninguno de los dos lados.
 *
 * Algoritmo (simplificado):
 *   1. Pick candidato 169.254.X.Y derivado del MAC + tick counter.
 *   2. ARP probe: 3 requests "who-has X.Y? tell 0.0.0.0" con pausa.
 *      Si vemos REPLY o REQUEST con spa=X.Y → conflict, pick another.
 *   3. ARP announce: 2 requests "who-has X.Y? tell X.Y" como "hola
 *      mundo, soy yo".
 *   4. net_set_ip + net_set_netmask(255.255.0.0).  No gateway (link-local
 *      no enruta fuera del /16).
 *
 * Ref: RFC 3927 §2 + §3.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void net_get_mac(uint8_t mac_out[6]);
extern void net_set_ip(const uint8_t ip[4]);
extern void net_set_netmask(const uint8_t mask[4]);
extern uint64_t idt_get_ticks(void);

/* APIs internas a net.c — exponemos lo que necesitamos para el probe.    */
extern void net_poll_wait(void);
extern int  net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac_out[6]);

/* Probe especial: ARP "who-has X.Y? tell 0.0.0.0".  net.c tiene un
 * arp_send_request estático; expone una versión pública para APIPA.    */
extern void net_arp_probe(const uint8_t target_ip[4]);

/* RFC 3927 timing (en ticks de 100 Hz):
 *   PROBE_WAIT      1 segundo antes de empezar
 *   PROBE_NUM       3 probes
 *   PROBE_MIN .. PROBE_MAX  1..2 segundos entre probes
 *   ANNOUNCE_WAIT   2 segundos entre probes y announces
 *   ANNOUNCE_NUM    2 announces
 *   ANNOUNCE_INT    2 segundos entre announces                          */
#define APIPA_PROBE_WAIT_TICKS     100
#define APIPA_PROBE_NUM            3
#define APIPA_PROBE_INT_TICKS      150
#define APIPA_ANNOUNCE_WAIT_TICKS  200
#define APIPA_ANNOUNCE_NUM         2
#define APIPA_ANNOUNCE_INT_TICKS   200

/* PRNG simple — sembrado con MAC + tick para que dos OsitoKs en la
 * misma red converjan a IPs distintas.                                  */
static uint64_t apipa_seed;

static uint32_t apipa_rand(void)
{
    apipa_seed = apipa_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(apipa_seed >> 32);
}

static void apipa_pick(uint8_t out[4])
{
    /* RFC 3927 §2.1: elegir host bits del rango 169.254.1.0 .. 169.254.254.255
     * (excluir 169.254.0.0/24 y 169.254.255.0/24 que son reservados).    */
    out[0] = 169;
    out[1] = 254;
    /* X en [1, 254], Y en [0, 255].  256 valores de Y * 254 valores de
     * X = 65024 IPs únicas — colisiones en la red son muy improbables.   */
    out[2] = (uint8_t)(1 + (apipa_rand() % 254));
    out[3] = (uint8_t)(apipa_rand() & 0xFF);
}

/* Espera ticks ms con net_poll_wait para no bloquear el sistema. */
static void apipa_sleep(uint64_t ticks)
{
    uint64_t start = idt_get_ticks();
    while (idt_get_ticks() - start < ticks) {
        net_poll_wait();
    }
}

/* Comprobar si vimos algún paquete ARP que indique que candidate_ip ya
 * está en uso.  Usamos net_arp_lookup_nowait — si retorna OK con un MAC
 * para la IP candidata, alguien la respondió → conflicto.                */
static bool apipa_in_use(const uint8_t candidate_ip[4])
{
    uint8_t mac[6];
    return net_arp_lookup_nowait(candidate_ip, mac) == 0;
}

/* ── Public ────────────────────────────────────────────────────────── */

int apipa_assign(void)
{
    uint8_t mac[6];
    net_get_mac(mac);
    apipa_seed = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
                 ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
                 ((uint64_t)mac[4] <<  8) |  (uint64_t)mac[5];
    apipa_seed ^= idt_get_ticks() * 0x9E3779B97F4A7C15ULL;

    serial_puts("[APIPA] starting link-local autoconfig (RFC 3927)\n");
    fb_puts(" Net: link-local autoconfig...\n");

    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t cand[4];
        apipa_pick(cand);

        serial_puts("[APIPA] candidate ");
        for (int i = 0; i < 4; i++) {
            serial_putdec(cand[i]); if (i < 3) serial_puts(".");
        }
        serial_puts("\n");

        /* PROBE_WAIT antes de empezar — RFC exige 0..1s para evitar
         * tormentas si todo el segmento se enciende a la vez.            */
        apipa_sleep(APIPA_PROBE_WAIT_TICKS);

        /* 3 ARP probes con spaciado de ~1.5s.                             */
        bool conflict = false;
        for (int p = 0; p < APIPA_PROBE_NUM; p++) {
            net_arp_probe(cand);
            apipa_sleep(APIPA_PROBE_INT_TICKS);
            if (apipa_in_use(cand)) { conflict = true; break; }
        }
        if (conflict) {
            serial_puts("[APIPA] conflict — picking another\n");
            continue;
        }

        /* No conflict — esperar antes de announce.                       */
        apipa_sleep(APIPA_ANNOUNCE_WAIT_TICKS);

        /* Setear IP localmente ANTES del announce (announce dice
         * "spa=X.Y" → necesita que our_ip sea X.Y).                      */
        uint8_t mask[4] = { 255, 255, 0, 0 };
        net_set_ip(cand);
        net_set_netmask(mask);

        /* 2 announces — gratuitous ARP "who-has X.Y? tell X.Y".          */
        for (int a = 0; a < APIPA_ANNOUNCE_NUM; a++) {
            net_arp_probe(cand);
            if (a < APIPA_ANNOUNCE_NUM - 1)
                apipa_sleep(APIPA_ANNOUNCE_INT_TICKS);
        }

        serial_puts("[APIPA] assigned ");
        for (int i = 0; i < 4; i++) {
            serial_putdec(cand[i]); if (i < 3) serial_puts(".");
        }
        serial_puts("/16\n");
        fb_puts(" Net: ");
        for (int i = 0; i < 4; i++) {
            fb_putdec(cand[i]); if (i < 3) fb_puts(".");
        }
        fb_puts("/16 (link-local)\n");
        return 0;
    }

    serial_puts("[APIPA] gave up after 8 attempts\n");
    return -1;
}
