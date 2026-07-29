/* libnmea/serial.c -- raw serial port (or replay file) for an NMEA source.
 *
 * Split out of gps-monitor.c (M1).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "serial.h"

speed_t baud_of(int b)
{
    switch (b) {
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

int port_open(port_t *p)
{
    p->fd = open(p->path, p->replay ? O_RDONLY : (O_RDWR | O_NOCTTY | O_NONBLOCK));
    if (p->fd < 0)
        return -1;
    if (!p->replay && isatty(p->fd)) {
        struct termios t;
        if (tcgetattr(p->fd, &t) == 0) {
            cfmakeraw(&t);
            t.c_cflag |= CLOCAL | CREAD | CS8;
            t.c_cflag &= (unsigned)~CRTSCTS;
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 0;
            cfsetispeed(&t, baud_of(p->baud));
            cfsetospeed(&t, baud_of(p->baud));
            tcsetattr(p->fd, TCSANOW, &t);
        }
    }
    return 0;
}

void port_close(port_t *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}
