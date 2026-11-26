#include "terminalclient.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

namespace {

termios g_saved;
bool g_raw = false;
volatile sig_atomic_t g_winch = 1; // la taille est envoyée dès le départ

void restoreTerminal()
{
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = false;
    }
}

void onWinch(int)
{
    g_winch = 1;
}

bool writeAll(int fd, const char *p, size_t n)
{
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= size_t(w);
    }
    return true;
}

void writeOut(const std::string &s)
{
    writeAll(STDOUT_FILENO, s.data(), s.size());
}

bool sendFrame(int fd, char type, const std::string &payload)
{
    const uint32_t len = uint32_t(payload.size());
    std::string f;
    f.reserve(5 + payload.size());
    f += type;
    f += char(len >> 24);
    f += char(len >> 16);
    f += char(len >> 8);
    f += char(len);
    f += payload;
    return writeAll(fd, f.data(), f.size());
}

void notice(const std::string &msg)
{
    writeOut("\r\n\033[1;36m[P2P Chat]\033[0m " + msg + "\r\n");
}

void setTitle(const std::string &title)
{
    writeOut("\033]0;" + title + "\007");
}

} // namespace

int runTerminalClient(const char *socketPath, const char *title, const char *language)
{
    const bool en = language && std::strcmp(language, "en") == 0;
    // SOCK_CLOEXEC n'existe pas sur macOS : on positionne FD_CLOEXEC à part
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (fd < 0 || std::strlen(socketPath) >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, en ? "P2P Chat: invalid local socket\n" : "P2P Chat : socket local invalide\n");
        return 1;
    }
    std::strcpy(addr.sun_path, socketPath);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, en ? "P2P Chat: cannot connect to P2P Chat (%s)\n"
                                : "P2P Chat : connexion à P2P Chat impossible (%s)\n",
                     std::strerror(errno));
        ::sleep(5);
        return 1;
    }

    if (title && *title)
        setTitle(title);
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved) == 0) {
        termios raw = g_saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        g_raw = true;
        std::atexit(restoreTerminal);
    }
    struct sigaction sa{};
    sa.sa_handler = onWinch; // sans SA_RESTART : poll() est interrompu et la taille renvoyée
    sigaction(SIGWINCH, &sa, nullptr);

    std::string in, endMessage;
    bool ended = false;
    char buf[16384];
    for (;;) {
        if (g_winch) {
            g_winch = 0;
            winsize ws{};
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                const std::string p{char(ws.ws_col >> 8), char(ws.ws_col & 0xff), char(ws.ws_row >> 8),
                                    char(ws.ws_row & 0xff)};
                if (!sendFrame(fd, 'R', p))
                    break;
            }
        }
        // select() plutôt que poll() : sous macOS, poll() gère mal les terminaux
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        FD_SET(fd, &readable);
        if (::select(fd + 1, &readable, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (FD_ISSET(STDIN_FILENO, &readable)) {
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0 || !sendFrame(fd, 'D', std::string(buf, size_t(n))))
                break;
        }
        if (FD_ISSET(fd, &readable)) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            in.append(buf, size_t(n));
            while (in.size() >= 5) {
                const uint32_t len = (uint32_t(uint8_t(in[1])) << 24) | (uint32_t(uint8_t(in[2])) << 16)
                                     | (uint32_t(uint8_t(in[3])) << 8) | uint32_t(uint8_t(in[4]));
                if (in.size() < 5 + size_t(len))
                    break;
                const char type = in[0];
                const std::string payload = in.substr(5, len);
                in.erase(0, 5 + size_t(len));
                if (type == 'D')
                    writeOut(payload);
                else if (type == 'T')
                    setTitle(payload);
                else if (type == 'M')
                    notice(payload);
                else if (type == 'E') {
                    endMessage = payload;
                    ended = true;
                }
            }
            if (ended)
                break;
        }
    }
    ::close(fd);

    notice(ended ? endMessage : std::string(en ? "Lost connection to P2P Chat." : "Liaison avec P2P Chat perdue."));
    if (g_raw) {
        notice(en ? "Press any key to close this window." : "Appuyez sur une touche pour fermer cette fenêtre.");
        char c;
        while (::read(STDIN_FILENO, &c, 1) < 0 && errno == EINTR) {
        }
    }
    restoreTerminal();
    return 0;
}
