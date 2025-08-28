#define _DARWIN_C_SOURCE 1
#include <stdint.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

/* time constants */
#define NSEC_PER_SEC 1000000000ull
#define TARGET_FPS 60u
#define FRAME_NS (NSEC_PER_SEC / TARGET_FPS)

const char CHARS[] = ":-=0123456789!@#$%&#$[]|<>?ODUCQAB";

struct blue_pill
{
  char *rsi;
  float speed;
  int lifespan; /* trail length */
  float cycle;  /* head position */
  int bold;
};

/* Globals */
static int COLS = 0, ROWS = 0;
static struct blue_pill *matrix = NULL;
static volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t exit_pending = 0;

/* Frame buffer (reused every frame) */
static char *frame = NULL;
static size_t frame_cap = 0;

/* 256-color SGR (keeps your earlier palette, no truecolor) */
static const char *SGR_RESET = "\x1b[0m";
static const char *SGR_HEAD = "\x1b[1;38;5;15m"; /* bold white */
static const char *SGR_NECK = "\x1b[38;5;194m";  /* pale green */
static const char *SGR_TAIL3 = "\x1b[38;5;82m";  /* bright green */
static const char *SGR_TAIL2 = "\x1b[38;5;40m";  /* mid green */
static const char *SGR_TAIL1 = "\x1b[38;5;22m";  /* dark green */

/* --- utils --- */
static inline int chars_len(void) { return (int)(sizeof(CHARS) - 1); }

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

/* term size */
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
    *out_cols = w.ws_col / 2; /* we print "c " per col -> half width */
    *out_rows = w.ws_row;
  }
}

/* cleanup */
static void cleanup(void)
{
  if (matrix)
  {
    for (int i = 0; i < COLS; i++)
      free(matrix[i].rsi);
    free(matrix);
    matrix = NULL;
  }
  free(frame);
  frame = NULL;
  frame_cap = 0;
  /* show cursor & restore cursor to home */
  write(1, "\x1b[?25h\x1b[H", 8);
}

/* signals */
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

/* allocs */
static struct blue_pill *alloc_matrix(int cols, int rows)
{
  struct blue_pill *m = (struct blue_pill *)malloc((size_t)cols * sizeof(*m));
  if (!m)
    return NULL;
  for (int c = 0; c < cols; c++)
  {
    m[c].rsi = (char *)malloc((size_t)rows);
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
    for (int r = 0; r < rows; r++)
    {
      m[c].rsi[r] = CHARS[rand() % chars_len()];
    }
    m[c].bold = (rand() % 100 > 60);
  }
  return m;
}

static int ensure_frame_buffer(int cols, int rows)
{
  /* generous cap: each cell worst-case ~24 bytes (SGR + char + reset + space),
     plus newline per row and a little headroom */
  size_t need = (size_t)rows * (size_t)cols * 24u + (size_t)rows + 64u;
  if (need <= frame_cap)
    return 0;
  char *nf = (char *)realloc(frame, need);
  if (!nf)
    return -1;
  frame = nf;
  frame_cap = need;
  return 0;
}

/* resize */
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

  struct blue_pill *nm = alloc_matrix(new_cols, new_rows);
  if (!nm)
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
  matrix = nm;
  COLS = new_cols;
  ROWS = new_rows;

  if (ensure_frame_buffer(COLS, ROWS) != 0)
  {
    resize_pending = 0;
    return -1;
  }

  resize_pending = 0;
  return 1;
}

/* init */
static int init_world(void)
{
  get_term_size_now(&COLS, &ROWS);
  matrix = alloc_matrix(COLS, ROWS);
  if (!matrix)
    return -1;
  return ensure_frame_buffer(COLS, ROWS);
}

/* time */
static inline uint64_t ns_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline void sleep_until(uint64_t target_ns)
{
  uint64_t now = ns_now();
  if (target_ns <= now)
    return;
  uint64_t diff = target_ns - now;
  struct timespec ts = {.tv_sec = (time_t)(diff / 1000000000ull),
                        .tv_nsec = (long)(diff % 1000000000ull)};
  nanosleep(&ts, NULL);
}

/* frame building helpers */
static inline void buf_puts(char **p, const char *s)
{
  size_t n = strlen(s);
  memcpy(*p, s, n);
  *p += n;
}
static inline void buf_putc(char **p, char c)
{
  **p = c;
  (*p)++;
}

/* draw into frame buffer, then single write() */
static void draw_frame(void)
{
  char *ptr = frame;

  /* cursor to home */
  buf_puts(&ptr, "\x1b[H");

  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      const char *sgr = NULL;

      if (r - 3 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        sgr = matrix[c].bold ? SGR_TAIL3 : SGR_TAIL2;
      }
      else if (r - 1 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        sgr = SGR_TAIL2;
      }
      else if (r > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        sgr = SGR_TAIL1;
      }
      else if (matrix[c].cycle > r + 1 && matrix[c].cycle < r + 2)
      {
        sgr = SGR_NECK;
      }
      else if (matrix[c].cycle > r && matrix[c].cycle < r + 1)
      {
        sgr = SGR_HEAD;
      }
      else
      {
        /* blank: two spaces to account for half width printing ("c ") */
        buf_putc(&ptr, ' ');
        buf_putc(&ptr, ' ');
        continue;
      }

      buf_puts(&ptr, sgr);
      buf_putc(&ptr, matrix[c].rsi[r]);
      buf_putc(&ptr, ' ');
      buf_puts(&ptr, SGR_RESET);
    }
    if (r < ROWS - 1)
      buf_putc(&ptr, '\n');
  }

  /* write the whole frame at once */
  size_t len = (size_t)(ptr - frame);
  (void)write(1, frame, len);
}

static void simulate_matrix(void)
{
  for (int c = 0; c < COLS; c++)
  {
    for (int r = 0; r < ROWS; r++)
    {
      if ((rand() % 100) > 98)
      {
        matrix[c].rsi[r] = CHARS[rand() % chars_len()];
      }
    }
    matrix[c].cycle += matrix[c].speed;
    if (matrix[c].cycle > ROWS + matrix[c].lifespan)
    {
      free(matrix[c].rsi);
      matrix[c].rsi = (char *)malloc((size_t)ROWS);
      if (!matrix[c].rsi)
        continue;
      matrix[c].speed = ((((float)rand() / (float)RAND_MAX) + 0.1f) / 2.0f);
      matrix[c].cycle = 0.0f;
      pick_lifespan_for_column(&matrix[c], ROWS);
      for (int r = 0; r < ROWS; r++)
        matrix[c].rsi[r] = CHARS[rand() % chars_len()];
      matrix[c].bold = (rand() % 100 > 60);
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

  /* hide cursor & use alt screen (optional but nice) */
  write(1, "\x1b[?25l\x1b[H", 8);

  uint64_t last = ns_now();
  uint64_t next = last + FRAME_NS;

  for (;;)
  {
    if (exit_pending)
      break;
    if (resize_pending)
      apply_resize_if_needed();
    if (COLS <= 0 || ROWS <= 0)
      continue;

    draw_frame();
    simulate_matrix();

    sleep_until(next);
    uint64_t now = ns_now();
    next = (now > next + FRAME_NS) ? now + FRAME_NS : next + FRAME_NS;
  }
  return 0;
}
