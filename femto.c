
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// defines 

#define FEMTO_VERSION "0.0.1"

#define CRTL_KEY(k) ((k) & 0x1f)

// data structures

struct erow {
  int size;
  char *chars;
};

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

// append buffer
struct abuf {
  char *b;
  size_t len;
};

#define ABUF_INIT { NULL, 0 }

void abuf_append(struct abuf *ab, const char *s, int len)
{
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abuf_free(struct abuf *ab)
{
  free(ab->b);
}

// Defines
#define CTRL_KEY(k) ((k) & 0x1f)

// Data
struct editor_config {
  uint32_t cx, cy;
  int row_offset;
  int screenrows;
  int screencols;
  int numrows;
  struct erow *rows;
  struct termios orig_termios;
};

struct editor_config ed_config;

// Terminal
void die(const char *s)
{
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disable_rawmode(void)
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ed_config.orig_termios) == -1)
    die("tcsetattr");
}

void enable_rawmode(void)
{
  if (tcgetattr(STDIN_FILENO, &ed_config.orig_termios) == -1)
    die("tcgetattr");

  atexit(disable_rawmode);

  struct termios raw;

  tcgetattr(STDIN_FILENO, &raw);
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int editor_read_key(void)
{
  int nread;
  char c;
  while ((nread = read(STDERR_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDERR_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDERR_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDERR_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }          
        }
      }
      else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    }
    else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  }
  
  return c;
}

// row operations

void editor_append_row(char *s, size_t len)
{
  ed_config.rows = realloc(ed_config.rows, sizeof(struct erow) * (ed_config.numrows + 1));

  int at = ed_config.numrows;
  ed_config.rows[at].size = len;
  ed_config.rows[at].chars = malloc(len + 1);
  memcpy(ed_config.rows[at].chars, s, len);
  ed_config.rows[at].chars[len] = '\0';
  ed_config.numrows++;
}

// file i/o
void editor_open(char *filename)
{
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t line_len;
  while ((line_len = getline(&line, &linecap, fp)) != -1) {
    while (line_len > 0 && (line[line_len - 1] == '\n' ||
                           line[line_len - 1] == '\r'))
      line_len--;

    editor_append_row(line, line_len);
  }

  free(line);
  fclose(fp);
}

int get_cursor_position(int *rows, int *cols)
{
  char buf[32];
  uint32_t i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;
  
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;
  printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);

  return 0;
}

int get_window_size(int *rows, int *cols)
{
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return get_cursor_position(rows, cols);
  }
  else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
  }

  return 0;
}

// Output
void editor_scroll(void)
{
  if (ed_config.cy < ed_config.row_offset) {
    ed_config.row_offset = ed_config.cy;
  }
  if (ed_config.cy >= ed_config.row_offset + ed_config.screenrows) {
    ed_config.row_offset = ed_config.cy - ed_config.screenrows + 1;
  }
}

void editor_draw_rows(struct abuf *ab)
{
  int y;
  for (y = 0; y < ed_config.screenrows; y++) {
    int file_row = y + ed_config.row_offset;
    if (y >= ed_config.numrows) {
      if (ed_config.numrows == 0 && y == ed_config.screenrows / 3) {
        char welcome[80];
        int welcome_len = snprintf(welcome, sizeof(welcome), 
          "Femto editor -- version %s", FEMTO_VERSION);
        if (welcome_len > ed_config.screencols)
          welcome_len = ed_config.screencols;

        int padding = (ed_config.screencols - welcome_len) / 2;
        if (padding) {
          abuf_append(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abuf_append(ab, " ", 1);

        abuf_append(ab, welcome, welcome_len);
      }
      else {
        abuf_append(ab, "~", 1);
      }
    }
    else {
      int len = ed_config.rows[file_row].size;
      if (len > ed_config.screencols) 
        len = ed_config.screencols;
      abuf_append(ab, ed_config.rows[file_row].chars, len);
    }

    abuf_append(ab, "\x1b[K", 3);
    if (y < ed_config.screenrows - 1) {
      abuf_append(ab, "\r\n", 2);
    }
  }
}

void editor_refresh_screen(void)
{
  editor_scroll();

  struct abuf ab = ABUF_INIT;

  abuf_append(&ab, "\x1b[?25l", 6);
  abuf_append(&ab, "\x1b[H", 3);

  editor_draw_rows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
    (ed_config.cy - ed_config.row_offset), ed_config.cx+1);
    
  abuf_append(&ab, buf, strlen(buf));

  abuf_append(&ab, "\x1b[?25h", 6);

  write(STDERR_FILENO, ab.b, ab.len);
  abuf_free(&ab);
}

// Input

void editor_move_cursor(int key)
{
  switch (key) {
    case 'h':
    case ARROW_LEFT:
      if (ed_config.cx > 0)
        ed_config.cx--;
      break;
    case 'l':
    case ARROW_RIGHT:
      if (ed_config.cx < ed_config.screencols - 1)
        ed_config.cx++;
      break;
    case 'j':
    case ARROW_DOWN:
      if (ed_config.cy < ed_config.numrows)
        ed_config.cy++;
      break;
    case 'k':
    case ARROW_UP:
      if (ed_config.cy > 0)
      ed_config.cy--;
      break;
  }  
}

void editor_process_keypress(void)
{
  int c = editor_read_key();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case HOME_KEY:
      ed_config.cx = 0;
      break;
    case END_KEY:
      ed_config.cx = ed_config.screencols - 1;
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = ed_config.screenrows;
        while (times--)
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case 'h':
    case 'j':
    case 'k':
    case 'l':
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;
  }
}

// Init
void editor_init(void)
{
  ed_config.cx = 0;
  ed_config.cy = 0;
  ed_config.row_offset = 0;
  ed_config.numrows = 0;
  ed_config.rows = NULL;

  if (get_window_size(&ed_config.screenrows, &ed_config.screencols) == -1)
    die("get_window_size");
}

int main(int argc, char **argv)
{
  enable_rawmode();
  editor_init();

  if (argc >= 2) {
    editor_open(argv[1]);
  }
  
  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
  
  printf("goodbye.\r\n");
}