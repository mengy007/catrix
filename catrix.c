// catrix.c — Matrix render (256-color, buffer reuse, no printf in hot loop)

#define _DARWIN_C_SOURCE 1

#include <stdint.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

/* Characters used */
static const char CHARS[] = ":-=0123456789!@#$%&#$[]|<>?ODUCQAB";
static inline int chars_len(void) { return (int)(sizeof(CHARS) - 1); }

/* 256-color styles (xterm palette) */
#define S_RESET "\x1b[0m"
#define S_HEAD "\x1b[1;38;5;231m" /* bright white */
#define S_NEAR "\x1b[38;5;194m"   /* pale green */
#define S_T1_B "\x1b[38;5;82m"    /* bright green trail */
#define S_T1 "\x1b[38;5;40m"      /* mid-bright green */
#define S_T2 "\x1b[38;5;34m"      /* medium green */
#define S_T3 "\x1b[38;5;22m"      /* dark green */

/* Column state */
struct blue_pill
{
  char *rsi;    /* glyphs per row */
  float speed;  /* rows per tick */
  int lifespan; /* trail length */
  float cycle;  /* head position */
  int bold;     /* hint for brighter trail */
};

/* Globals */
static int COLS = 0, ROWS = 0; /* logical cols (we print "ch ") */
static struct blue_pill *matrix = NULL;
static volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t exit_pending = 0;

static inline int rand_range(int lo, int hi)
{
  if (hi < lo)
    return lo;
  return lo + (rand() % (hi - lo + 1));
}

static inline void pick_lifespan_for_column(struct blue_pill *col, int rows)
{
  int min_len = (int)(rows * 0.30f);
  int max_len = (int)(rows * 0.90f);
  if (min_len < 1)
    min_len = 1;
  if (max_len < min_len)
    max_len = min_len;
  col->lifespan = rand_range(min_len, max_len);
}

/* Terminal size */
static void get_term_size_now(int *out_cols, int *out_rows)
{
  struct winsize w;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0 || w.ws_row == 0)
  {
    *out_cols = 80 / 2;
    *out_rows = 24;
  }
  else
  {
    *out_cols = (int)w.ws_col / 2; /* print "ch " so half width */
    *out_rows = (int)w.ws_row;
  }
}

/* Cleanup */
static void cleanup(void)
{
  if (matrix)
  {
    for (int i = 0; i < COLS; i++)
      free(matrix[i].rsi);
    free(matrix);
    matrix = NULL;
  }
  fputs(S_RESET, stdout);
  fputs("\x1b[?25h", stdout); /* show cursor */
  fflush(stdout);
}

/* Signals */
static void handle_winch(int sig)
{
  (void)sig;
  resize_pending = 1;
}
static void handle_exit_signal(int sig)
{
  (void)sig;
  exit_pending = 1;
}

/* Allocate matrix */
static struct blue_pill *alloc_matrix(int cols, int rows)
{
  struct blue_pill *m = (struct blue_pill *)malloc((size_t)cols * sizeof(*m));
  if (!m)
    return NULL;
  for (int c = 0; c < cols; c++)
  {
    m[c].rsi = (char *)malloc((size_t)rows * sizeof(char));
    if (!m[c].rsi)
    {
      for (int k = 0; k < c; k++)
        free(m[k].rsi);
      free(m);
      return NULL;
    }
    m[c].speed = ((((float)rand() / (float)RAND_MAX) + 0.1f) / 2.0f);
    m[c].cycle = (float)(rand() % rows);
    pick_lifespan_for_column(&m[c], rows);
    m[c].bold = (rand() % 100 > 60);
    for (int r = 0; r < rows; r++)
    {
      m[c].rsi[r] = CHARS[rand() % chars_len()];
    }
  }
  return m;
}

/* Reuse buffer on reseed (no free/malloc in hot path) */
static void reseed_column(int c)
{
  matrix[c].speed = ((((float)rand() / (float)RAND_MAX) + 0.1f) / 2.0f);
  matrix[c].cycle = 0.0f;
  pick_lifespan_for_column(&matrix[c], ROWS);
  matrix[c].bold = (rand() % 100 > 60);
  for (int r = 0; r < ROWS; r++)
  {
    matrix[c].rsi[r] = CHARS[rand() % chars_len()];
  }
}

/* Resize */
static int apply_resize_if_needed(void)
{
  if (!resize_pending)
    return 0;
  int new_cols, new_rows;
  get_term_size_now(&new_cols, &new_rows);
  if (new_cols <= 0 || new_rows <= 0)
  {
    resize_pending = 0;
    return -1;
  }

  struct blue_pill *new_m = alloc_matrix(new_cols, new_rows);
  if (!new_m)
  {
    resize_pending = 0;
    return -1;
  }

  if (matrix)
  {
    for (int c = 0; c < COLS; c++)
      free(matrix[c].rsi);
    free(matrix);
  }
  matrix = new_m;
  COLS = new_cols;
  ROWS = new_rows;

  resize_pending = 0;
  return 1;
}

/* Init */
static int init_world(void)
{
  get_term_size_now(&COLS, &ROWS);
  matrix = alloc_matrix(COLS, ROWS);
  return matrix ? 0 : -1;
}

/* Faster char output */
#if defined(__GLIBC__) || defined(__APPLE__)
#define PUTCH(ch) putchar_unlocked((ch))
#else
#define PUTCH(ch) putchar((ch))
#endif

static inline void emit_style(const char *s, const char **cur)
{
  if (s != *cur)
  {
    fputs(s, stdout);
    *cur = s;
  }
}

static inline void home_cursor(void) { fputs("\x1b[H", stdout); }

/* Render frame (no printf in hot loop) */
static void print_matrix(void)
{
  for (int r = 0; r < ROWS; r++)
  {
    const char *cur = S_RESET; /* cache last style */
    for (int c = 0; c < COLS; c++)
    {
      const float cy = matrix[c].cycle;
      const float tl = cy - (float)matrix[c].lifespan;
      const char ch = matrix[c].rsi[r];

      const char *style = NULL;
      if (cy > r && cy < r + 1)
      {
        style = S_HEAD;
      }
      else if (cy > r + 1 && cy < r + 2)
      {
        style = S_NEAR;
      }
      else if ((r - 3 > tl && r < cy - 2))
      {
        style = matrix[c].bold ? S_T1_B : S_T1;
      }
      else if ((r - 1 > tl && r < cy - 2))
      {
        style = S_T2;
      }
      else if ((r > tl && r < cy - 2))
      {
        style = S_T3;
      }
      else
      {
        /* blank */
        if (cur != S_RESET)
          emit_style(S_RESET, &cur);
        PUTCH(' ');
        PUTCH(' ');
        continue;
      }

      emit_style(style, &cur);
      PUTCH(ch);
      PUTCH(' ');
    }
    if (r < ROWS - 1)
      PUTCH('\n');
  }
  fputs(S_RESET, stdout);
}

/* Simulation step */
static void simulate_matrix(void)
{
  for (int c = 0; c < COLS; c++)
  {
    for (int r = 0; r < ROWS; r++)
    {
      if (rand() % 100 > 98)
      {
        matrix[c].rsi[r] = CHARS[rand() % chars_len()];
      }
    }
    matrix[c].cycle += matrix[c].speed;
    if (matrix[c].cycle > ROWS + matrix[c].lifespan)
    {
      reseed_column(c);
    }
  }
}

int main(void)
{
  atexit(cleanup);
  signal(SIGINT, handle_exit_signal);
  signal(SIGTERM, handle_exit_signal);
#ifdef SIGWINCH
  signal(SIGWINCH, handle_winch);
#endif

  srand((unsigned)time(NULL));

  if (init_world() != 0)
  {
    fprintf(stderr, "Failed to initialize matrix\n");
    return 1;
  }

  /* hide cursor */
  fputs("\x1b[?25l", stdout);
  fflush(stdout);

  const useconds_t DELAY_US = 12000; /* simple pacing */

  for (;;)
  {
    if (exit_pending)
      break;
    apply_resize_if_needed();

    home_cursor();
    print_matrix();
    simulate_matrix();

    fflush(stdout);
    usleep(DELAY_US);
  }

  return 0;
}
