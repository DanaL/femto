
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// defines 

#define FEMTO_VERSION "0.0.1"

#define CRTL_KEY(k) ((k) & 0x1f)

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
  int screenrows;
  int screencols;
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

char editor_read_key(void)
{
  int nread;
  char c;
  while ((nread = read(STDERR_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  return c;
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

void editor_draw_rows(struct abuf *ab)
{
  int y;
  for (y = 0; y < ed_config.screenrows; y++) {
    if (y == ed_config.screenrows / 3) {
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

    abuf_append(ab, "\x1b[K", 3);
    if (y < ed_config.screenrows - 1) {
      abuf_append(ab, "\r\n", 2);
    }
  }
}

void editor_refresh_screen(void)
{
  struct abuf ab = ABUF_INIT;

  abuf_append(&ab, "\x1b[?25l", 6);
  abuf_append(&ab, "\x1b[H", 3);

  editor_draw_rows(&ab);

  abuf_append(&ab, "\x1b[H", 3);
  abuf_append(&ab, "\x1b[?25h", 6);

  write(STDERR_FILENO, ab.b, ab.len);
  abuf_free(&ab);
}

// Input
void editor_process_keypress(void)
{
  char c = editor_read_key();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

// Init

void init_editor()
{
  if (get_window_size(&ed_config.screenrows, &ed_config.screencols) == 1) {
    die("get_window_size");
  }
}

int main(void)
{
  enable_rawmode();
  init_editor();
  
  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
  
  printf("goodbye.\r\n");
}