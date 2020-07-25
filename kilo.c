/**************
*  includes  *
**************/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*************
*  defines  *
*************/

#define CTRL_KEY(x) ((x) & 0x1f)
#define KILO_VERSION "0.0.1"

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

/**********
*  data  *
**********/

struct editorConfig {
    int cx, cy;                     // Cursor position
    int screencols;
    int screenrows;
    struct termios orig_termios;
};

struct editorConfig E;

/*************
*  terminal  *
*************/

void die(const char* s)
{
    // Clear screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // Position cursor on top left, so we can render the screen
    write(STDOUT_FILENO, "\x1b[1;1H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void)
{
    if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode(void)
{
    // Get terminal attributes
    if ( tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    // Restore original attributes at exit
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON | ICRNL); 
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cflag |=  (CS8);
    raw.c_oflag &= ~(OPOST);
    // read() will return as soon as it reads 1 byte
    raw.c_cc[VMIN] = 0;
    // read() will wait 100ms
    raw.c_cc[VTIME] = 1;

    // Set terminal attributes
    if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey(void)
{
    int nread;
    char c;
    while (( nread = read(STDIN_FILENO, &c, 1) ) != 1) {
        // In cygwin, when read() times out it return -1 with an errno of EAGAIN
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    // If it is escape sequence
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '4': return END_KEY;
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                default: break;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        ++i;
    }
    buf[i] = 0;

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    /*if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {*/
    /*return -1;*/
    /*} else {*/
    /**cols = ws.ws_col;*/
    /**rows = ws.ws_row;*/
    /*return 0;*/
    /*}*/
}

/*******************
*  append buffer  *
*******************/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy((new + ab->len), s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/************
*  output  *
************/

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; ++y) {
        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor --- version %s", KILO_VERSION);
            if (welcomelen > E.screencols) welcomelen = E.screencols;
            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while(padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }

        // Erases rest of the line to the right of the cursor
        abAppend(ab, "\x1b[K", 3);

        if (y < E.screenrows - 1)
            abAppend(ab, "\r\n", 2);
    }
}

void editorRefreshScreen()
{
    struct abuf ab = ABUF_INIT;

    // Hide cursor while writing to screen
    abAppend(&ab, "\x1b[?25l", 6);
    // Clear screen
    abAppend(&ab, "\x1b[2J", 4);
    // Position cursor on top left, so we can render the screen
    abAppend(&ab, "\x1b[1;1H", 3);

    editorDrawRows(&ab);

    // Position the cursor
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, len);
    // Show cursor after finishing writing to screen
    abAppend(&ab, "\x1b[?25h", 6);

    // Draw screen
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
/***********
*  input  *
***********/

void editorMoveCursor(int key)
{
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0)
                E.cx--;
            break;
        case ARROW_UP:
            if (E.cy > 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.screencols - 1)
                E.cy++;
            break;
        case ARROW_RIGHT:
            if (E.cx < E.screenrows - 1)
            E.cx++;
            break;
        default:
            break;
    }
}

void editorProcessKeypress(void)
{
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            // Clear screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            // Position cursor on top left, so we can render the screen
            write(STDOUT_FILENO, "\x1b[1;1H", 6);
            exit(0);
            break;
        case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case PAGE_UP:
            E.cy = 0;
            break;
        case PAGE_DOWN:
            E.cy = E.screenrows - 1;
            break;
        /*case PAGE_UP: case PAGE_DOWN:*/
            /*// {} are needed because C doesn't allow for the declaration of variables inside labels*/
            /*{*/
                /*int times = E.screenrows;*/
                /*while (times--)*/
                    /*editorMoveCursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);*/
            /*}*/
            /*break;*/
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        case DEL_KEY:
            break;
        default:
            break;
    }
}

/**********
*  init  *
**********/

void initEditor(void)
{
    E.cx = 0;
    E.cy = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}