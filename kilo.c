/**************
*  includes  *
**************/

// Makes code more portable, especially getline, they are added before includes to decide what
// features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*************
*  defines  *
*************/

#define CTRL_KEY(x) ((x) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

enum editorKey {
    BACKSPACE = 127,    /* Backspace doesn't have a representation in C like 
                           \r or \t so we add it to the enum */
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

typedef struct {
    int size;       // Size of row
    int rsize;      // Size of rendered row
    char *chars;    // Row
    char *render;   // Rendered row
} erow;

struct editorConfig {
    int cx, cy;                  // Cursor position
    int rx;                      // Cursor horizontal position on render
    int rowoff, coloff;          // Scroll
    int screencols, screenrows;  // Terminal size
    int numrows;                 // Num of rows of opened file
    erow *row;                   // Rows of opened file
    int dirty;
    char *filename;              // Name of the opened file
    char statusmsg[80];         // Status message
    time_t statusmsg_time;       // Status time
    struct termios orig_termios;
};

struct editorConfig E;

/****************
*  prototypes  *
****************/

void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
}

/********************
*  row operations  *
********************/

int editorRowCxToRx(erow *row, int cx)
{
    int j, rx = 0;
    for (j = 0; j < cx; ++j) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row)
{
    // Count how many tabs in line
    int j, tabs = 0;
    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') tabs++;
    }

    // Reserve maximum memory necessary
    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP-1) + 1);
    // Handle error
    if (!row->render) die("malloc");

    // Convert chars to render (handle tabs, ...)
    int idx = 0;
    for (j = 0; j < row->size; ++j) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows) return;

    // Fetch memory to save new line
    E.row = realloc(E.row, sizeof(erow) * ( E.numrows + 1 ));
    if (!E.row) die("realloc");

    memmove(&E.row[E.cy+1], &E.row[E.cy], sizeof(erow)*(E.numrows - at));

    // Allocate new line
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    if (!E.row[at].chars) die("malloc");
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    // Initialize render row
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    
    // Increase count
    E.numrows++;

    /* The file has changed */
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows) return;

    /* Free memory of the current row */
    editorFreeRow(&E.row[at]);
    /* Move all rows, only pointers need to be moved */
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));

    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, char c)
{
    // Handle out of bounds
    if (at < 0 || at > row->size) at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1); // TODO while loop
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);

    /* The file has changed */
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    /* Reserve memory and append */
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';

    editorUpdateRow(row);

    E.dirty++;
}

void editorRowDelChar(erow *row, int at) 
{
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/***********************
*  editor operations  *
***********************/

void editorInsertChar(char c)
{
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline(void)
{
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[E.cx] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

void editorDelChar(void)
{
    /* Do nothing if at end of file */
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/**************
*  file i/o  *
**************/

/* The caller is expected to free the memory */
char *editorRowsToString(int *buflen)
{
    int totlen = 0, j;

    /* Count total size */
    for (j = 0; j < E.numrows; ++j) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    /* Create string */
    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; ++j) {
        /* Copy line */
        memcpy(p, E.row[j].chars, E.row[j].size);

        /* Insert end of line */
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    // Save filename
    free(E.filename);
    E.filename = strdup(filename);  // strdup automatically performs a malloc

    // Open file
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line;
    ssize_t linelen = 0;
    size_t linecap = 0;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        // Erase CR a LF at the end of the line, since erow are already rows
        while (linelen > 0 && (line[linelen-1] == '\r' || line[linelen - 1] == '\n'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }

    // Free and close file
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(void)
{
    if (!E.filename) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (!E.filename) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    /* fcntl.h */
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);  /* 644 owner read/write, everyone else only read */
    if (fd != -1) {
        /* unistd.h, set file size */
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                /* unistd.h */
                close(fd);
                free(buf);
                editorSetStatusMessage("%d bytes written to disk", len);
                E.dirty = 0;
                return;
            }
        }

        close(fd);
    }

    free(buf);
    /* strerror is like perror, but it takes errno and produces a string */
    editorSetStatusMessage("Can't save! I/O Error: %s", strerror(errno));
}

/**********
*  find  *
**********/

void editorFindCallback(char *query, int key)
{
    static int last_match = -1; /* line of last match, -1 if none */
    static int direction = 1;   /* -1 = backward; 1 = forward */

    /*Leaving search mode*/
    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_UP || ARROW_LEFT) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    /*current is the index of the current row we are searching. If there was a last match,
     * it starts on the line after (or before, if we’re searching backwards). If there 
     * wasn’t a last match, it starts at the top of the file and searches in the forward 
     * direction to find the first match.*/
    if (last_match == -1) direction = 1;
    int current = last_match;
    /*Search in every row*/
    int i;
    for (i = 0; i < E.numrows; ++i) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        /*strstr finds first ocurrence of substring in string*/
        char *match = strstr(row->render, query);

        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            /*Scroll to the end of the file, so when editorScroll() is called, 
             * it will scroll upwards to the found sequence*/
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorFind(void)
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_rowof = E.rowoff;
    int saved_coloff = E.coloff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query)
        free(query);
    else {
        /*Restore previous position*/
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.rowoff = saved_rowof;
        E.coloff = saved_coloff;
    }
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

void editorScroll(void)
{
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy > E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    } 
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx > E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; ++y) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // If no file is opened, show welcome screen
            if (E.numrows == 0 && y == E.screenrows / 3) {
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
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        // Erases rest of the line to the right of the cursor
        abAppend(ab, "\x1b[K", 3);
        
        // Status bar
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    // Invert colors
    abAppend(ab, "\x1b[7m", 4);

    // Display file and number of lines
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
            E.filename ? E.filename : "[No Name]", E.numrows,
            E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy+1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    // Fill the rest of status
    while (len < E.screencols) {
        if (len == E.screencols - rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    // Undo color inversion
    abAppend(ab, "\x1b[m", 3);
    // CRLN for status message
    abAppend(ab, "\r\n", 2);
}

void editorDrawStatusMessage(struct abuf *ab)
{
    // Clear status message
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}


void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // Hide cursor while writing to screen
    abAppend(&ab, "\x1b[?25l", 6);
    // Clear screen
    abAppend(&ab, "\x1b[2J", 4);
    // Position cursor on top left, so we can render the screen
    abAppend(&ab, "\x1b[1;1H", 6);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawStatusMessage(&ab);

    // Position the cursor
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
            E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
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

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || BACKSPACE || CTRL_KEY('h')) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                free(buf);
                return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy <= E.numrows) ? &E.row[E.cy] : NULL;
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
            if (E.cy < E.numrows)
                E.cy++;
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size)
                E.cx++;
            break;
        default:
            break;
    }

    // Correct horizontal position if line is too short
    row = (E.cy <= E.numrows) ? &E.row[E.cy] : NULL;    // Recover row since it could've been moved
    int rowlen = (row) ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void editorProcessKeypress(void)
{
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!! File has unsaved changes. "
                        "Press CTRL-Q %d more times to quit", quit_times);
                quit_times--;
                return;
            }
            // Clear screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            // Position cursor on top left, so we can render the screen
            write(STDOUT_FILENO, "\x1b[1;1H", 6);
            exit(0);
            break;
        case ARROW_UP:          /* Fallthrough */
        case ARROW_DOWN: 
        case ARROW_LEFT: 
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case PAGE_UP:           /* Fallthrough */
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--) editorMoveCursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
            break;
        case DEL_KEY:           /* Fallthrough */
            editorMoveCursor(ARROW_RIGHT);
        case BACKSPACE:         
        case CTRL_KEY('h'):
            editorDelChar();
            break;
        case CTRL_KEY('l'):     /* Fallthrough */
        case '\x1b':
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case CTRL_KEY('f'):
            editorFind();
            break;
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/**********
*  init  *
**********/

void initEditor(void)
{
    // Init global data
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.numrows = 0;
    E.coloff = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = 0;
    E.statusmsg_time = 0;

    // Get window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    
    // Make room for status bar
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q to quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
