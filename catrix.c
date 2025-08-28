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

const useconds_t DELAY = 12000;
const char CHARS[] = ":-=0123456789!@#$%&#$[]|<>?ODUCQAB";

struct blue_pill
{
  char *rsi;
  float speed;
  int lifespan; /* trail length */
  float cycle;  /* head position */
};

/* Globals */
static int COLS = 0, ROWS = 0;
static struct blue_pill *matrix = NULL;
static volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t exit_pending = 0;
// const double TARGET_FPS = 60.0;
// const uint64_t FRAME_NS = (uint64_t)(1e9 / TARGET_FPS);

/* Utils */
static inline int chars_len(void) { return (int)(sizeof(CHARS) - 1); }

static inline int rand_range(int min_inclusive, int max_inclusive)
{
  if (max_inclusive < min_inclusive)
    return min_inclusive;
  return min_inclusive + (rand() % (max_inclusive - min_inclusive + 1));
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
void get_term_size_now(int *out_cols, int *out_rows)
{
  struct winsize w;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0 || w.ws_row == 0)
  {
    *out_cols = 80;
    *out_rows = 24;
  }
  else
  {
    *out_cols = w.ws_col / 2;
    *out_rows = w.ws_row;
  }
}

/* Cleanup */
void cleanup(void)
{
  if (matrix)
  {
    for (int i = 0; i < COLS; i++)
    {
      free(matrix[i].rsi);
    }
    free(matrix);
    matrix = NULL;
  }
  /* SHOW CURSOR */
  printf("\x1b[?25h");
  fflush(stdout);
}

/* Signal handlers (async-safe: set flags only) */
void handle_winch(int sig)
{
  (void)sig;
  resize_pending = 1;
}
void handle_exit_signal(int sig)
{
  (void)sig;
  exit_pending = 1;
}

/* Allocate a fresh matrix */
struct blue_pill *alloc_matrix(int cols, int rows)
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
    pick_lifespan_for_column(&m[c], rows); /* 40%–80% of height */
    for (int r = 0; r < rows; r++)
    {
      m[c].rsi[r] = CHARS[rand() % chars_len()];
    }
  }
  return m;
}

/* Resize safely at a sync point */
int apply_resize_if_needed(void)
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

  /* Free old using OLD COLS */
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

/* Initial world */
int init_world(void)
{
  get_term_size_now(&COLS, &ROWS);
  matrix = alloc_matrix(COLS, ROWS);
  return matrix ? 0 : -1;
}

void clear_screen(void)
{
  printf("\x1b[H");
}

void print_matrix(void)
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if (r - 3 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        printf("\x1b[38;2;10;255;65m%c \033[0m", matrix[c].rsi[r]);
      }
      else if (r - 1 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        printf("\x1b[38;2;10;143;17m%c \033[0m", matrix[c].rsi[r]);
      }
      else if (r > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        printf("\x1b[38;2;10;59;0m%c \033[0m", matrix[c].rsi[r]);
      }
      else if (matrix[c].cycle > r + 1 && matrix[c].cycle < r + 2)
      {
        printf("\x1b[38;2;220;255;220m%c \033[0m", matrix[c].rsi[r]);
      }
      else if (matrix[c].cycle > r && matrix[c].cycle < r + 1)
      {
        // Head - Bright, almost white
        printf("\x1b[1;38;2;250;255;250m%c \x1b[22m", matrix[c].rsi[r]);
      }
      else
      {
        // Blank spaces
        printf("  ");
      }
    }
    if (r < ROWS - 1)
      printf("\n");
  }
}

void simulate_matrix(void)
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
      /* Re-seed this column */
      free(matrix[c].rsi);
      matrix[c].rsi = (char *)malloc((size_t)ROWS * sizeof(char));
      if (!matrix[c].rsi)
        continue; /* limp along */
      matrix[c].speed = ((((float)rand() / (float)RAND_MAX) + 0.1f) / 2.0f);
      matrix[c].cycle = 0.0f;
      pick_lifespan_for_column(&matrix[c], ROWS); /* 40%–80% */
      for (int r = 0; r < ROWS; r++)
      {
        matrix[c].rsi[r] = CHARS[rand() % chars_len()];
      }
    }
  }
}

/* High-res monotonic time (nanoseconds) */
static inline uint64_t ns_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Sleep until a target time (nanoseconds since epoch of CLOCK_MONOTONIC) */
static inline void sleep_until(uint64_t target_ns)
{
  uint64_t now = ns_now();
  if (target_ns <= now)
    return;
  uint64_t diff = target_ns - now;
  struct timespec ts = {.tv_sec = diff / 1000000000ull,
                        .tv_nsec = diff % 1000000000ull};
  nanosleep(&ts, NULL);
}

int main(void)
{
  atexit(cleanup);
  signal(SIGINT, handle_exit_signal);
  signal(SIGTERM, handle_exit_signal);

#ifdef SIGWINCH
  signal(SIGWINCH, handle_winch);
#endif

  time_t t;
  srand((unsigned)time(&t));
  uint64_t last = ns_now();
  uint64_t next = last + FRAME_NS;

  if (init_world() != 0)
  {
    fprintf(stderr, "Failed to initialize matrix\n");
    return 1;
  }

  /* HIDE CURSOR */
  printf("\x1b[?25l");
  fflush(stdout);

  for (;;)
  {
    uint64_t now = ns_now();
    double dt = (now - last) / 1e9; // seconds
    if (dt > 0.1)
      dt = 0.1;
    last = now;

    if (exit_pending)
      break;
    apply_resize_if_needed();

    if (COLS <= 0 || ROWS <= 0)
    {
      // usleep(DELAY);
      continue;
    }

    clear_screen();
    print_matrix();
    simulate_matrix();

    sleep_until(next);
    now = ns_now();
    // Advance the schedule; if we fell behind, skip ahead one frame
    if (now > next + FRAME_NS)
      next = now + FRAME_NS;
    else
      next += FRAME_NS;

    // usleep(DELAY);
  }

  return 0;
}
