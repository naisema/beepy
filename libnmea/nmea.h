/* libnmea/nmea.h -- NMEA 0183 sentence framing and parsing into gps_t. */
#ifndef LIBNMEA_NMEA_H
#define LIBNMEA_NMEA_H

#include <stddef.h>

#include "gps.h"

typedef struct {
    char buf[160];
    size_t len;
    int overflow;
} nmea_rx_t;

int csum_ok(const char *l, size_t n);
int split(char *l, char *f[], int maxf);
double fnum(char *f[], int nf, int i);
int fint(char *f[], int nf, int i);
double coord(char *f[], int nf, int vi, int hi);
void set_utc(gps_t *g, char *f[], int nf, int i);
void set_date(gps_t *g, char *f[], int nf, int i);
void nmea_apply(gps_t *g, char *f[], int nf);
void nmea_line(gps_t *g, char *l, size_t n);
void nmea_feed(nmea_rx_t *rx, gps_t *g, const char *data, size_t n);

#endif /* LIBNMEA_NMEA_H */
