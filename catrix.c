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

/* characters for rain */
static const char CHARS[] = ":-=0123456789!@#$%&#$[]|<>?ODUCQAB";

/* matrix column */
struct blue_pill
{
  char *rsi;
  float speed;
  int lifespan; /* trail length */
  float cycle;  /* head position */
  int bold;
};

/* render cell (grid for diffing) */
typedef struct
{
  char ch;       /* printable char or space */
  uint8_t style; /* 0=blank, 1=tail1(dark), 2=tail2(mid), 3=tail3(bright), 4=neck, 5=head */
} Cell;

/* Globals */
static int COLS = 0, ROWS = 0; /* logical columns (half the terminal) and rows */
static struct blue_pill *matrix = NULL;
static volatile sig_atomic_t resize_pending = 0;
static volatile sig_atomic_t exit_pending = 0;

/* double buffer for diff rendering */
static Cell *prev_grid = NULL, *cur_grid = NULL;
static size_t grid_cap_cells = 0;

/* big output buffer reused each frame */
static char *outbuf = NULL;
static size_t out_cap = 0;

/* 256-color SGR (no truecolor) */
static const char *SGR_RESET = "\x1b[0m";
static const char *SGR_MAP[] = {
    NULL,             /* 0 blank - no SGR needed */
    "\x1b[38;5;22m",  /* 1 tail1 dark */
    "\x1b[38;5;40m",  /* 2 tail2 mid */
    "\x1b[38;5;82m",  /* 3 tail3 bright */
    "\x1b[38;5;194m", /* 4 neck pale */
    "\x1b[1;38;5;15m" /* 5 head bold white */
};

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
  }
  free(prev_grid);
  prev_grid = NULL;
  free(cur_grid);
  cur_grid = NULL;
  free(outbuf);
  outbuf = NULL;
  /* show cursor & home */
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

/* alloc matrix */
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

/* ensure grids & output buffer sizes */
static int ensure_buffers(int cols, int rows)
{
  size_t cells = (size_t)cols * (size_t)rows;
  if (cells > grid_cap_cells)
  {
    Cell *pg = (Cell *)realloc(prev_grid, cells * sizeof(Cell));
    Cell *cg = (Cell *)realloc(cur_grid, cells * sizeof(Cell));
    if (!pg || !cg)
    {
      free(pg);
      free(cg);
      return -1;
    }
    prev_grid = pg;
    cur_grid = cg;
    grid_cap_cells = cells;
    /* poison prev so first diff draws full */
    memset(prev_grid, 0xFF, cells * sizeof(Cell));
  }
  /* worst-case diff (move+SGR per cell): ~64 bytes per cell + slack */
  size_t need = cells * 64u + 4096u;
  if (need > out_cap)
  {
    char *nb = (char *)realloc(outbuf, need);
    if (!nb)
      return -1;
    outbuf = nb;
    out_cap = need;
  }
  return 0;
}

/* resize */
static int apply_resize_if_needed(int *force_full)
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

  if (ensure_buffers(COLS, ROWS) != 0)
  {
    resize_pending = 0;
    return -1;
  }

  *force_full = 1; /* after resize, repaint everything once */
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
  return ensure_buffers(COLS, ROWS);
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

/* small buffered emit helpers */
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
static inline void buf_move_cursor(char **p, int row1, int col1)
{
  /* ESC[row;colH  (1-based) */
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "\x1b[%d;%dH", row1, col1);
  memcpy(*p, tmp, (size_t)n);
  *p += n;
}

/* build current grid from simulation state */
static void build_cur_grid(void)
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t style = 0;
      if (r - 3 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        style = matrix[c].bold ? 3 : 2;
      }
      else if (r - 1 > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        style = 2;
      }
      else if (r > matrix[c].cycle - matrix[c].lifespan && r < matrix[c].cycle - 2)
      {
        style = 1;
      }
      else if (matrix[c].cycle > r + 1 && matrix[c].cycle < r + 2)
      {
        style = 4;
      }
      else if (matrix[c].cycle > r && matrix[c].cycle < r + 1)
      {
        style = 5;
      }
      else
      {
        style = 0;
      }

      Cell *cell = &cur_grid[(size_t)r * (size_t)COLS + (size_t)c];
      if (style == 0)
      {
        cell->style = 0;
        cell->ch = ' ';
      }
      else
      {
        cell->style = style;
        cell->ch = matrix[c].rsi[r];
      }
    }
  }
}

/* diff renderer: emits only changed runs (grouped by style) */
static void render_diff(int force_full)
{
  char *ptr = outbuf;

  if (force_full)
  {
    /* clear and home once */
    buf_puts(&ptr, "\x1b[2J\x1b[H");
  }

  for (int r = 0; r < ROWS; r++)
  {
    int c = 0;
    while (c < COLS)
    {
      Cell *cur = &cur_grid[(size_t)r * (size_t)COLS + (size_t)c];
      Cell *prv = &prev_grid[(size_t)r * (size_t)COLS + (size_t)c];

      int unchanged = (!force_full) &&
                      (cur->style == prv->style) &&
                      (cur->style == 0 || cur->ch == prv->ch);
      if (unchanged)
      {
        c++;
        continue;
      }

      /* start a run at c with this style; extend while cells need update and share style */
      uint8_t style = cur->style;
      int start = c, end = c + 1;
      while (end < COLS)
      {
        Cell *cc = &cur_grid[(size_t)r * (size_t)COLS + (size_t)end];
        Cell *pp = &prev_grid[(size_t)r * (size_t)COLS + (size_t)end];

        int need = force_full ||
                   (cc->style != pp->style) ||
                   (cc->style != 0 && cc->ch != pp->ch);
        if (!need || cc->style != style)
          break;
        end++;
      }

      /* move cursor to the physical column (2*start + 1), row is 1-based */
      buf_move_cursor(&ptr, r + 1, 2 * start + 1);

      /* set SGR for non-blank */
      if (style != 0 && SGR_MAP[style])
        buf_puts(&ptr, SGR_MAP[style]);

      /* emit the run: for each cell, print char + a trailing space; blanks -> two spaces */
      for (int x = start; x < end; x++)
      {
        Cell *cc = &cur_grid[(size_t)r * (size_t)COLS + (size_t)x];
        if (style == 0)
        {
          buf_putc(&ptr, ' ');
          buf_putc(&ptr, ' ');
        }
        else
        {
          buf_putc(&ptr, cc->ch);
          buf_putc(&ptr, ' ');
        }
      }

      /* optional: we can leave SGR active; next run sets it explicitly */
      c = end;
    }
  }

  /* flush all updates at once */
  size_t len = (size_t)(ptr - outbuf);
  if (len)
    (void)write(1, outbuf, len);

  /* swap/copy current -> previous */
  memcpy(prev_grid, cur_grid, (size_t)COLS * (size_t)ROWS * sizeof(Cell));
}

/* simulate rain */
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

  /* hide cursor & home */
  write(1, "\x1b[?25l\x1b[H", 8);

  uint64_t last = ns_now();
  uint64_t next = last + FRAME_NS;
  int force_full = 1;

  for (;;)
  {
    if (exit_pending)
      break;
    if (resize_pending)
      apply_resize_if_needed(&force_full);
    if (COLS <= 0 || ROWS <= 0)
      continue;

    build_cur_grid();
    render_diff(force_full);
    force_full = 0;

    simulate_matrix();

    sleep_until(next);
    uint64_t now = ns_now();
    next = (now > next + FRAME_NS) ? now + FRAME_NS : next + FRAME_NS;
  }
  return 0;
}
