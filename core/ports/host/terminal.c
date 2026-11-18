// SPDX-License-Identifier: Apache-2.0 OR MIT
// The harness's terminal keeps what was typed ahead. MicroPython's unix port
// switches its stdin between raw and original modes around each raw REPL
// command with TCSAFLUSH, which discards input already waiting: a client
// that sends its next command as soon as the last one's reply ends (the
// rig, mpremote) loses it when it arrives before the switch, and the REPL
// waits for it forever. The rp2 port's USB serial never drops input, so
// here a flush of stdin is a plain switch (the link wraps tcsetattr).

#include <termios.h>
#include <unistd.h>

int __real_tcsetattr(int fd, int when, const struct termios *termios);

int __wrap_tcsetattr(int fd, int when, const struct termios *termios) {
    if (fd == STDIN_FILENO && when == TCSAFLUSH) {
        when = TCSANOW;
    }
    return __real_tcsetattr(fd, when, termios);
}
