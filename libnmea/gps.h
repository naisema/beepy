/* libnmea/gps.h -- GNSS receiver state: fix, DOP, satellite sets. */
#ifndef LIBNMEA_GPS_H
#define LIBNMEA_GPS_H

#include <time.h>

#define SNR_MIN 10
#define SNR_MAX 50

#define MAX_SATS 64
#define NSYS 5 /* G R E B other */

typedef struct {
    char sys; /* G R E B */
    int prn, elev, azim, snr, used;
} sat_t;

typedef struct {
    sat_t s[MAX_SATS];
    int n;
} satset_t;

typedef struct {
    int quality, mode, sats_used;
    double hdop, pdop, vdop;
    double lat, lon, alt_m, speed_kmh, course_deg;
    char utc[9], date[11];

    satset_t live;
    satset_t stage[NSYS];

    /* GSA can arrive before the GSV cycle that creates the satellites, so the
     * PRN lists are stored and re-applied whenever either changes. */
    int gsa_prn[NSYS][12], gsa_n[NSYS];

    unsigned long lines, bad_crc, unknown;
    time_t last_data;
    int connected;
} gps_t;

void gps_init(gps_t *g);
int sys_idx(char s);
char sys_from_prn(int prn);
void gps_apply_used(gps_t *g);
void gps_publish(gps_t *g, char sys);
int gps_order(const gps_t *g, int by_snr, int *idx, int max);
int gps_count(const gps_t *g, char sys, int *used);
void load_demo(gps_t *g);
void print_state(const gps_t *g);

#endif /* LIBNMEA_GPS_H */
