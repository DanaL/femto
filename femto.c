
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

// Defines
#define CTRL_KEY(k) ((k) & 0x1f)

// Data
struct termios orig_termios;

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
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enable_rawmode(void)
{
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
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

// Output

void editor_draw_rows(void)
{
  int y;
  for (y = 0; y < 24; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

void editor_refresh_screen(void)
{
  write(STDERR_FILENO, "\x1b[2J", 4);
  write(STDERR_FILENO, "\x1b[H", 3);

  editor_draw_rows();

  write(STDERR_FILENO, "\x1b[H", 3);
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
int main(void)
{
  enable_rawmode();

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
  
  printf("goodbye.\r\n");
}