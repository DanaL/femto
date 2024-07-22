#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// defines 

#define FEMTO_VERSION "0.1.0"
#define TAB_STOP 2
#define FEMTO_QUIT_TIMES 3

#define CRTL_KEY(k) ((k) & 0x1f)

// data structures

struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
};

enum editor_key {
  BACKSPACE = 127,
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
  int cx, cy;
  int rx;
  int row_offset;
  int col_offset;
  int screenrows;
  int screencols;
  int numrows;
  int left_margin;
  struct erow *rows;
  bool dirty;
  char *filename;
  char status_msg[80];
  time_t status_msg_time;
  struct termios orig_termios;
};

struct editor_config ed_cfg;

// prototypes 

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen(void);
char *editor_prompt(char *prompt, void (*callback)(char *, int));

// terminal
void die(const char *s)
{
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disable_rawmode(void)
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ed_cfg.orig_termios) == -1)
    die("tcsetattr");
}

void enable_rawmode(void)
{
  if (tcgetattr(STDIN_FILENO, &ed_cfg.orig_termios) == -1)
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

int editor_row_cx_to_rx(struct erow *row, int cx)
{
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }

  return rx;
}

int editor_row_rx_to_cx(struct erow *row, int rx)
{
  int curr_rx = 0;
  int cx = 0;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      curr_rx += (TAB_STOP - 1) - (curr_rx % TAB_STOP);
    ++curr_rx;

    if (curr_rx > rx)
      return cx;
  }

  return cx;
}

void editor_update_row(struct erow *row)
{
  int tabs = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      ++tabs;
  }

  free(row->render);
  row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TAB_STOP != 0)
        row->render[idx++] = ' ';
    }
    else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len)
{
  if (at < 0 || at > ed_cfg.numrows)
    return;

  ed_cfg.rows = realloc(ed_cfg.rows, sizeof(struct erow) * (ed_cfg.numrows + 1));
  memmove(&ed_cfg.rows[at + 1], &ed_cfg.rows[at], 
    sizeof(struct erow) * (ed_cfg.numrows - at));

  ed_cfg.rows[at].size = len;
  ed_cfg.rows[at].chars = malloc(len + 1);
  memcpy(ed_cfg.rows[at].chars, s, len);
  ed_cfg.rows[at].chars[len] = '\0';

  ed_cfg.rows[at].rsize = 0;
  ed_cfg.rows[at].render = NULL;
  editor_update_row(&ed_cfg.rows[at]);

  ed_cfg.numrows++;
  ed_cfg.dirty = true;
}

void editor_free_row(struct erow *row)
{
  free(row->render);
  free(row->chars);
}

void editor_del_row(int at)
{
  if (at < 0 || at >= ed_cfg.numrows)
    return;

  editor_free_row(&ed_cfg.rows[at]);
  memmove(&ed_cfg.rows[at], &ed_cfg.rows[at + 1], 
    sizeof(struct erow) * (ed_cfg.numrows - at - 1));
  --ed_cfg.numrows;
  ed_cfg.dirty = true;
}

void editor_row_insert_char(struct erow *row, int at, int c) 
{
  if (at < 0 || at > row->size)
    at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;

  editor_update_row(row);
  ed_cfg.dirty = true;
}

void editor_row_append_str(struct erow *row, char *s, size_t len)
{
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editor_update_row(row);
  ed_cfg.dirty = true;
}

void editor_row_del_char(struct erow *row, int at)
{
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  --row->size;
  editor_update_row(row);
  ed_cfg.dirty = true;
}

// editor operations

void editor_insert_char(int c)
{
  if (ed_cfg.cy == ed_cfg.numrows) {
    editor_insert_row(ed_cfg.numrows, "", 0);
  }

  int at = ed_cfg.cx - ed_cfg.left_margin - 1;
  editor_row_insert_char(&ed_cfg.rows[ed_cfg.cy], at, c);
  ++ed_cfg.cx;
}

void editor_insert_newline(void)
{
  if (ed_cfg.numrows == 0) {
    editor_insert_row(0, "", 0);
    ed_cfg.cx = ed_cfg.left_margin + 2;
    return;
  }
  else if (ed_cfg.cx <= ed_cfg.left_margin + 1) {
    editor_insert_row(ed_cfg.cy, "", 0);
  }
  else {
    int pos_in_line = ed_cfg.cx - ed_cfg.left_margin - 1;
    struct erow *row = &ed_cfg.rows[ed_cfg.cy];
    editor_insert_row(ed_cfg.cy + 1, &row->chars[pos_in_line],
      row->size -pos_in_line);
    row = &ed_cfg.rows[ed_cfg.cy];
    row->size = pos_in_line;
    row->chars[row->size] = '\0';
    editor_update_row(row);
  }

  ++ed_cfg.cy;
  ed_cfg.cx = ed_cfg.left_margin + 1;
}

void editor_del_char(void)
{
  if (ed_cfg.cy == ed_cfg.numrows)
    return;
  if (ed_cfg.cx == 0 && ed_cfg.cy == 0)
    return;

  struct erow *row = &ed_cfg.rows[ed_cfg.cy];
  if (ed_cfg.cx > ed_cfg.left_margin + 1) {
    int at = ed_cfg.cx - ed_cfg.left_margin - 2;
    editor_row_del_char(row, at);
    --ed_cfg.cx;
  }
  else {
    ed_cfg.cx = ed_cfg.rows[ed_cfg.cy - 1].size + ed_cfg.left_margin + 1;
    editor_row_append_str(&ed_cfg.rows[ed_cfg.cy - 1], row->chars, row->size);
    editor_del_row(ed_cfg.cy);
    --ed_cfg.cy;
  }
}

// file i/o

char *editor_rows_to_str(int *buflen)
{
  int totlen = 0;
  
  for (int j = 0; j < ed_cfg.numrows; j++)
    totlen += ed_cfg.rows[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int j = 0; j < ed_cfg.numrows; j++) {
    memcpy(p, ed_cfg.rows[j].chars, ed_cfg.rows[j].size);
    p += ed_cfg.rows[j].size;
    *p = '\n';
    ++p;
  }

  return buf;
}

void editor_open(char *filename)
{
  free(ed_cfg.filename);
  ed_cfg.filename = strdup(filename);

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

    editor_insert_row(ed_cfg.numrows, line, line_len);
  }

  free(line);
  fclose(fp);
  ed_cfg.dirty = false;
}

void editor_save(void)
{
  if (!ed_cfg.filename) {
		ed_cfg.filename = editor_prompt("Save as: %s", NULL);
		if (ed_cfg.filename == NULL) {
			editor_set_status_message("Nevermind.");
			return;
		}
	}

  int len;
  char *buf = editor_rows_to_str(&len);

  int fd = open(ed_cfg.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {        
        close(fd);
        free(buf);
        ed_cfg.dirty = false;
        editor_set_status_message("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editor_set_status_message("Buffer not saved! I/O error: %s", strerror(errno));
}

// find

void editor_find_callback(char *query, int key)
{
  static int last_match = -1;
  static int direction = 1;

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  }
  else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  }
  else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  }
  else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;
  int current = last_match;
  for (int i = 0; i < ed_cfg.numrows; i++) {
    current += direction;
    if (current == -1)
      current = ed_cfg.numrows - 1;
    else if (current == ed_cfg.numrows)
      current = 0;

    struct erow *row = &ed_cfg.rows[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      ed_cfg.cy = current;
      ed_cfg.cx = editor_row_rx_to_cx(row, match - row->render) + ed_cfg.left_margin + 1;
      ed_cfg.row_offset = ed_cfg.numrows;
      break;
    }
  }
}

void editor_find(void)
{
  int saved_cx = ed_cfg.cx;
  int saved_cy = ed_cfg.cy;
  int saved_coloff = ed_cfg.col_offset;
  int saved_rowoff = ed_cfg.row_offset;

  char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);

  if (query) {
    free(query);
  }
  else {
    ed_cfg.cx = saved_cx;
    ed_cfg.cy = saved_cy;
    ed_cfg.col_offset = saved_coloff;
    ed_cfg.row_offset = saved_rowoff;
  }
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
  ed_cfg.rx = 0;
  if (ed_cfg.cy < ed_cfg.numrows) {
    ed_cfg.rx = editor_row_cx_to_rx(&ed_cfg.rows[ed_cfg.cy], ed_cfg.cx);
  }

  if (ed_cfg.cy < ed_cfg.row_offset) {
    ed_cfg.row_offset = ed_cfg.cy;
  }
  if (ed_cfg.cy >= ed_cfg.row_offset + ed_cfg.screenrows) {
    ed_cfg.row_offset = ed_cfg.cy - ed_cfg.screenrows + 1;
  }
  if (ed_cfg.cx < ed_cfg.col_offset) {
    ed_cfg.col_offset = ed_cfg.rx;
  }
  if (ed_cfg.rx >= ed_cfg.col_offset + ed_cfg.screencols) {
    ed_cfg.col_offset = ed_cfg.rx - ed_cfg.screencols + 1;
  }
}

void editor_draw_welcome(struct abuf *ab)
{
  char welcome[80];
  int welcome_len = snprintf(welcome, sizeof(welcome), 
        "Femto editor -- version %s", FEMTO_VERSION);
  if (welcome_len > ed_cfg.screencols)
    welcome_len = ed_cfg.screencols;

  int padding = (ed_cfg.screencols - welcome_len) / 2;
  if (padding) {
    abuf_append(ab, "~", 1);
    padding--;
  }
  while (padding--)
    abuf_append(ab, " ", 1);

  abuf_append(ab, welcome, welcome_len);
}

// draw each row that is on screen. Either the row of text in our buffer
// or an empty line with a ~
void editor_draw_rows(struct abuf *ab)
{ 
  if (ed_cfg.numrows > 0)
  {
    // figure out how wide we need the left margin to be
    char buff[80];
    snprintf(buff, 80, "%d", ed_cfg.numrows);
    int left_padding = strlen(buff);
    
    ed_cfg.left_margin = left_padding;
    if (ed_cfg.cx <= ed_cfg.left_margin)
      ed_cfg.cx = ed_cfg.left_margin + 1;
  }

  for (int y = 0; y < ed_cfg.screenrows; y++) {
    int file_row = y + ed_cfg.row_offset;
    if (y >= ed_cfg.numrows) {
      if (ed_cfg.numrows == 0 && y == ed_cfg.screenrows / 3)
       editor_draw_welcome(ab);
      else
        abuf_append(ab, "~", 1);
    }
    else {
      int len = ed_cfg.rows[file_row].rsize - ed_cfg.col_offset;
      if (len < 0)
        len = 0;
      if (len > ed_cfg.screencols) 
        len = ed_cfg.screencols;
      
      int bll = strlen(ed_cfg.rows[file_row].render) + ed_cfg.left_margin + 2;
      char *buf = malloc(ed_cfg.left_margin + 2);
      sprintf(buf, "%*d ", ed_cfg.left_margin, file_row + 1);
      abuf_append(ab, "\x1b[2m", 4);
      abuf_append(ab, buf, ed_cfg.left_margin + 1);
      abuf_append(ab, "\x1b[0m", 4);
      abuf_append(ab, &ed_cfg.rows[file_row].render[ed_cfg.col_offset], len);
      free(buf);
    }

    abuf_append(ab, "\x1b[K", 3);
    abuf_append(ab, "\r\n", 2);
  }
}

void editor_draw_status_bar(struct abuf *ab)
{
  abuf_append(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    ed_cfg.filename ? ed_cfg.filename : "[No Name]", ed_cfg.numrows,
    ed_cfg.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", ed_cfg.cy + 1, 
    ed_cfg.numrows);
  if (len > ed_cfg.screencols)
    len = ed_cfg.screencols;
  abuf_append(ab, status, len);
  while (len < ed_cfg.screencols) {
    if (ed_cfg.screencols - len == rlen) {
      abuf_append(ab, rstatus, rlen);
      break;
    }
    else {
      abuf_append(ab, " ", 1);
      ++len;
    }
  }

  abuf_append(ab, "\x1b[m", 3);
  abuf_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab)
{
  abuf_append(ab, "\x1b[K", 3);
  int msglen = strlen(ed_cfg.status_msg);
  if (msglen > ed_cfg.screencols)
    msglen = ed_cfg.screencols;
  if (msglen && time(NULL) - ed_cfg.status_msg_time < 5)
    abuf_append(ab, ed_cfg.status_msg, msglen);
}

void editor_refresh_screen(void)
{
  editor_scroll();

  struct abuf ab = ABUF_INIT;

  abuf_append(&ab, "\x1b[?25l", 6);
  abuf_append(&ab, "\x1b[H", 3);

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);
  
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
    (ed_cfg.cy - ed_cfg.row_offset) + 1, 
    (ed_cfg.rx - ed_cfg.col_offset) + 1);

  abuf_append(&ab, buf, strlen(buf));

  abuf_append(&ab, "\x1b[?25h", 6);

  write(STDERR_FILENO, ab.b, ab.len);
  abuf_free(&ab);
}

void editor_set_status_message(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ed_cfg.status_msg, sizeof(ed_cfg.status_msg), fmt, ap);
  va_end(ap);
  ed_cfg.status_msg_time = time(NULL);
}

// Input

char *editor_prompt(char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (true) {
		editor_set_status_message(prompt, buf);
		editor_refresh_screen();

		int c = editor_read_key();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0)
				buf[--buflen] = '\0';
		}
		else if (c == '\x1b') {
			editor_set_status_message("");
      if (callback)
        callback(buf, c);
			free(buf);
			return NULL;
		}
		else if (c == '\r') {
			if (buflen != 0) {
				editor_set_status_message("");
        if (callback)
          callback(buf, c);
				return buf;
			}
		}
		else if (!iscntrl(c) && c < 128) {
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}

    if (callback)
      callback(buf, c);
	}
}

void editor_jump_to_line(void)
{
  char *txt = editor_prompt("Goto line: %s", NULL);
  
  bool valid = true;
  char *p = txt;
  while (*p != '\0') {
    if (!isdigit(*p)) {
      valid = false;
      break;
    }
    ++p;
  }

  if (valid) {
    char buf[80];

    // I find I I often type 0 instead of 1 when I want to
    // go to the first line of a file because 0-index brain, so I'll
    // just treat it as 1.
    int ln = atoi(txt);
    if (ln == 0)
      ln = 1;
    else if (ln >= ed_cfg.numrows)
      ln = ed_cfg.numrows;
    
    // I like to have the line we jumped to be around 1/3 the way down 
    // the screen
    if (ln > 10 && ed_cfg.numrows - ln > ed_cfg.screenrows) {
      editor_set_status_message("flag!");
      ed_cfg.cy = ln - 1;
      //ed_cfg.row_offset = ln - 9;
    }
    else {
      editor_set_status_message("flag??");
      ed_cfg.cy = ln - 1;
    }

    //[12;40H
    editor_scroll();
    //sprintf(buf, "row offset to : %d", ed_cfg.row_offset);
    //editor_set_status_message(buf);
  }

  free(txt);
}

void editor_move_cursor(int key)
{
  struct erow *row = ed_cfg.cy >= ed_cfg.numrows ? NULL : &ed_cfg.rows[ed_cfg.cy];
  int right_margin = row ? row->size + ed_cfg.left_margin + 1 : 0;
  
  switch (key) {
    case ARROW_LEFT:
      if (ed_cfg.cx > ed_cfg.left_margin + 1) {
        ed_cfg.cx--;
      }
      else if (ed_cfg.cy > 0) {
        ed_cfg.cy--;
        ed_cfg.cx = ed_cfg.rows[ed_cfg.cy].size + ed_cfg.left_margin + 1;
      }
      break;
    case ARROW_RIGHT:
      if (row && ed_cfg.cx < right_margin) {
        ed_cfg.cx++;        
      }
      else if (row && ed_cfg.cx >= right_margin && ed_cfg.cy < ed_cfg.numrows - 1) {
        ed_cfg.cy++;
        ed_cfg.cx = ed_cfg.left_margin + 1;
      }      
      break;
    case ARROW_DOWN:      
      if (ed_cfg.cy < ed_cfg.numrows - 1)
        ed_cfg.cy++;
      break;
    case ARROW_UP:
      if (ed_cfg.cy > 0)
        ed_cfg.cy--;
      break;
  }
  
  row = ed_cfg.cy >= ed_cfg.numrows ? NULL : &ed_cfg.rows[ed_cfg.cy];
  int row_len = row ? right_margin : ed_cfg.left_margin + 1;
  if (ed_cfg.cx > row_len) {
    ed_cfg.cx = row_len;
  }
}

void editor_process_keypress(void)
{
  static int quit_times = FEMTO_QUIT_TIMES;
  int c = editor_read_key();

  switch (c) {
    case '\r':
      editor_insert_newline();
      break;
    case CTRL_KEY('q'):
      if (ed_cfg.dirty && quit_times > 0) {
        editor_set_status_message("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        --quit_times;
        return;
      }

      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case CTRL_KEY('s'):      
      editor_save();
      break;
    case CTRL_KEY('g'):
      editor_jump_to_line();
      break;
    case HOME_KEY:
      ed_cfg.cx = 0;
      break;
    case END_KEY:
      if (ed_cfg.cy < ed_cfg.numrows)
        ed_cfg.cx = ed_cfg.rows[ed_cfg.cy].size;      
      break;
    case CTRL_KEY('f'):
      editor_find();
      break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY)
        editor_move_cursor(ARROW_RIGHT);
      editor_del_char();
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          ed_cfg.cy = ed_cfg.row_offset;          
        }
        else if (c == PAGE_DOWN) {
          ed_cfg.cy = ed_cfg.row_offset + ed_cfg.screenrows - 1;
          if (ed_cfg.cy > ed_cfg.numrows)
            ed_cfg.cy = ed_cfg.numrows;          
        }

        int times = ed_cfg.screenrows;
        while (times--)
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;
    case CTRL_KEY('l'):
    case '\x1b':
      break;
    default:
      editor_insert_char(c);
      break;
  }

  quit_times = FEMTO_QUIT_TIMES;
}

// Init
void editor_init(void)
{
  ed_cfg.cx = 0;
  ed_cfg.cy = 0;
  ed_cfg.rx = 0;
  ed_cfg.row_offset = 0;
  ed_cfg.col_offset = 0;
  ed_cfg.numrows = 0;
  ed_cfg.left_margin = 0;
  ed_cfg.rows = NULL;
  ed_cfg.dirty = false;
  ed_cfg.filename = NULL;
  ed_cfg.status_msg[0] = '\0';
  ed_cfg.status_msg_time = 0;

  if (get_window_size(&ed_cfg.screenrows, &ed_cfg.screencols) == -1)
    die("get_window_size");
  ed_cfg.screenrows -= 2;
}

int main(int argc, char **argv)
{
  enable_rawmode();
  editor_init();

  if (argc >= 2) {
    editor_open(argv[1]);
  }
  
  editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
}
