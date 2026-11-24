/* Real TLS record decoder + AES-GCM; TCP fragmentation is the only transport
 * substitution. Small caller buffers must preserve every authenticated byte. */
#include "../kernel/tls.c"
extern int printf(const char *, ...);
static unsigned checks, failures, fragment;
static uint8_t wire[2][40000], source[2][22416];
static unsigned wire_size[2], wire_pos[2];
static tls_conn_t sessions[2];
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("TLS-RECV FAIL line=%d: %s\n",__LINE__,#x); } } while (0)
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
uint64_t idt_get_ticks(void) { return 0; }
int net_tcp_send(int fd, const void *data, uint32_t length)
{ (void)fd;(void)data;return (int)length; }
int net_tcp_recv_timeout(int fd, void *out, uint32_t capacity, uint32_t timeout)
{
    (void)timeout;
    if (fd < 0 || fd > 1 || wire_pos[fd] == wire_size[fd]) return -1;
    unsigned count = wire_size[fd] - wire_pos[fd];
    if (count > capacity) count = capacity;
    if (count > fragment) count = fragment;
    tmemcpy(out,wire[fd]+wire_pos[fd],count); wire_pos[fd] += count;
    return count;
}
static void tls_receive_record(unsigned fd, unsigned sequence, unsigned type,
                                const uint8_t *data, unsigned length, bool encrypted)
{
    uint8_t *p = wire[fd]+wire_size[fd];
    p[0] = type; put16(p+1,TLS_VER_12); put16(p+3,length+(encrypted?24:0));
    if (encrypted) {
        uint8_t nonce[12], aad[13];
        tmemset(nonce,0,sizeof(nonce)); nonce[11] = sequence;
        tmemset(aad,0,sizeof(aad)); aad[7] = sequence; aad[8] = type;
        put16(aad+9,TLS_VER_12); put16(aad+11,length);
        tmemcpy(p+5,nonce+4,8);
        CHECK(!aes128_gcm_encrypt(sessions[fd].server_write_key,nonce,aad,sizeof(aad),data,length,p+13,p+13+length));
    } else tmemcpy(p+5,data,length);
    wire_size[fd] += length+5+(encrypted?24:0);
}
static void tls_receive_case(unsigned capacity, bool encrypted)
{
    uint8_t out[TLS_MAX_RECORD+1];
    unsigned delivered[2] = {0,0};
    for (unsigned fd = 0; fd < 2; fd++) {
        tmemset(&sessions[fd],0,sizeof(sessions[fd]));
        sessions[fd].tcp_conn = fd; sessions[fd].state = TLS_STATE_ESTABLISHED;
        sessions[fd].server_cipher_active = encrypted;
        wire_size[fd] = wire_pos[fd] = 0;
        for (unsigned i = 0; i < sizeof(source[fd]); i++) source[fd][i] = (uint8_t)(i*17+fd*53);
        tls_receive_record(fd,0,TLS_APP_DATA,source[fd],16384,encrypted);
        tls_receive_record(fd,1,TLS_APP_DATA,source[fd]+16384,6001,encrypted);
        tls_receive_record(fd,2,TLS_APP_DATA,source[fd]+22385,31,encrypted);
        const uint8_t close[2] = {TLS_ALERT_WARNING,0};
        tls_receive_record(fd,3,TLS_ALERT,close,2,encrypted);
        CHECK(tls_recv(&sessions[fd],out,0,500) == 0 && wire_pos[fd] == 0);
        /* Isolate the small-read contract from the zero-length-read contract. */
        tmemset(&sessions[fd],0,sizeof(sessions[fd]));
        sessions[fd].tcp_conn = fd; sessions[fd].state = TLS_STATE_ESTABLISHED;
        sessions[fd].server_cipher_active = encrypted; wire_pos[fd] = 0;
    }
    for (unsigned turn = 0; turn < 50000; turn++) {
        bool unfinished = false;
        for (unsigned fd = 0; fd < 2; fd++) {
            if (delivered[fd] == sizeof(source[fd])) continue;
            unfinished = true;
            tmemset(out,0xa7,sizeof(out));
            int n = tls_recv(&sessions[fd],out,capacity,500);
            bool valid = n > 0 && (unsigned)n <= capacity &&
                (unsigned)n <= sizeof(source[fd])-delivered[fd] && out[capacity] == 0xa7;
            if (valid) for (int i = 0; i < n; i++)
                if (out[i] != source[fd][delivered[fd]+i]) { valid = false; break; }
            CHECK(valid);
            if (!valid) return;
            delivered[fd] += n;
        }
        if (!unfinished) break;
    }
    for (unsigned fd = 0; fd < 2; fd++) {
        CHECK(delivered[fd] == sizeof(source[fd]));
        CHECK(tls_recv(&sessions[fd],out,sizeof(out),500) < 0);
        CHECK(sessions[fd].state == TLS_STATE_CLOSED);
        CHECK(!encrypted || sessions[fd].server_seq == 4);
    }
}
int main(void)
{
    const unsigned capacities[] = {1,7,4095,4096,16384};
    const unsigned fragments[] = {1,17,4096,30000};
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned f = 0; f < sizeof(fragments)/sizeof(fragments[0]); f++) {
            fragment = fragments[f];
            for (unsigned c = 0; c < sizeof(capacities)/sizeof(capacities[0]); c++) tls_receive_case(capacities[c],mode!=0);
        }
    /* Authentication failure must not return decrypted but unauthenticated data. */
    tmemset(&sessions[0],0,sizeof(sessions[0]));
    sessions[0].state = TLS_STATE_ESTABLISHED; sessions[0].server_cipher_active = true;
    wire_pos[0] = wire_size[0] = 0; fragment = 30000;
    tls_receive_record(0,0,TLS_APP_DATA,source[0],100,true);
    wire[0][wire_size[0]-1] ^= 1;
    uint8_t out[17]; tmemset(out,0x55,sizeof(out));
    CHECK(tls_recv(&sessions[0],out,16,500) < 0);
    CHECK(out[0] == 0x55 && out[16] == 0x55 && sessions[0].server_seq == 0);
    printf("TLS-RECV DONE checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
