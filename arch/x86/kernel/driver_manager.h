#ifndef OSITO_DRIVER_MANAGER_H
#define OSITO_DRIVER_MANAGER_H

enum driver_state {
    DRIVER_DETECTED, DRIVER_ABSENT, DRIVER_WAIT_FS, DRIVER_WAIT_FIRMWARE,
    DRIVER_WAIT_NETWORK, DRIVER_STARTING, DRIVER_WAIT_CONFIG, DRIVER_CONNECTING,
    DRIVER_READY, DRIVER_DISABLED, DRIVER_FAILED, DRIVER_QUARANTINED
};
typedef struct driver_service driver_service_t;
struct driver_service {
    const char *name;
    enum driver_state (*step)(driver_service_t *);
    void *context;
    enum driver_state state;
    const char *detail;
    unsigned retry_ms;
    unsigned long long next_ms;
    unsigned busy, paused;
    unsigned long long (*clock_ms)(void);
};
void driver_manager_tick(driver_service_t *, unsigned, unsigned long long);
int driver_service_claim(driver_service_t *);
void driver_service_release(driver_service_t *);
const char *driver_state_name(enum driver_state);

/* Platform registry: boot-owned storage/input/display are never reset here. */
void driver_services_start(void);
void driver_services_command(int, char **);
int driver_wlan_claim(void);
void driver_wlan_release(int pause);
int driver_ipv4_claim(void);
void driver_ipv4_release(void);
int driver_wlan_remember(const void *bss, const unsigned char *pmk);
int driver_wlan_command(int, char **);
int driver_sshd_autostart(int enabled, unsigned port);
#endif
