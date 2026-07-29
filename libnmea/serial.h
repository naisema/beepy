/* libnmea/serial.h -- raw serial port (or replay file) for an NMEA source.
 *
 * Device-only: serial.c needs full termios and does not compile on the Mac.
 */
#ifndef LIBNMEA_SERIAL_H
#define LIBNMEA_SERIAL_H

#include <termios.h>
#include <time.h>

typedef struct {
    int fd;
    char path[128];
    int baud, replay;
    time_t retry_at;
} port_t;

speed_t baud_of(int b);
int port_open(port_t *p);
void port_close(port_t *p);

#endif /* LIBNMEA_SERIAL_H */
