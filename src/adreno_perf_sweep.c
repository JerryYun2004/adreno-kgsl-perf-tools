// SPDX-License-Identifier: MIT
// adreno_perf_sweep.c
// Phone-side Adreno KGSL performance-counter sweeper.
//
// This is intentionally separate from adreno_perf_stream.  It sweeps selected
// A8xx perfcounter groups in chunks that fit the empirically measured physical
// counter-slot capacity of each group.  For each chunk it launches a benchmark,
// streams counter deltas for a fixed duration, writes one CSV file, releases the
// counters, and moves to the next chunk.

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ADRENO_IOC_TYPE 0x09
#define DEFAULT_DEVICE "/dev/kgsl-3d0"
#define DEFAULT_ROOT "/data/local/tmp/adreno-perf-tools/sweeps"
#define DEFAULT_SAMPLE_INTERVAL 0.001
#define DEFAULT_BURSTS 1
#define DEFAULT_BURST_SLEEP 0.0
#define DEFAULT_WIDTH_SLEEP 0.0
#define MAX_WIDTH_SEQUENCE 128
#define MAX_GROUP_COUNTERS 512
#define MAX_ACTIVE 64
#define MAX_PATH_LEN 512
#define MAX_CMD_LEN 2048

struct adreno_perfcounter_get {
  unsigned int group_id;
  unsigned int countable_selector;
  unsigned int regster_offset_low;
  unsigned int regster_offset_high;
  unsigned int __pad;
};
#define ADRENO_IOCTL_PERFCOUNTER_GET \
  _IOWR(ADRENO_IOC_TYPE, 0x38, struct adreno_perfcounter_get)

struct adreno_perfcounter_put {
  unsigned int group_id;
  unsigned int countable_selector;
  unsigned int __pad[2];
};
#define ADRENO_IOCTL_PERFCOUNTER_PUT \
  _IOW(ADRENO_IOC_TYPE, 0x39, struct adreno_perfcounter_put)

struct adreno_perfcounter_read_group {
  unsigned int group_id;
  unsigned int countable_selector;
  unsigned long long value;
};

struct adreno_perfcounter_read {
  struct adreno_perfcounter_read_group *groups;
  unsigned int num_groups;
  unsigned int __pad[2];
};
#define ADRENO_IOCTL_PERFCOUNTER_READ \
  _IOWR(ADRENO_IOC_TYPE, 0x3B, struct adreno_perfcounter_read)

struct counter_desc {
  const char *group_name;
  unsigned int group_id;
  unsigned int selector;
  const char *xml_name;
  const char *short_name;
};

#include "a8xx_perf_table.inc"

struct sweep_group {
  const char *name;
  unsigned int group_id;
  size_t capacity;
  int selector_limit;  // -1 means all selectors in this group.  LRZ uses 3.
};

static const struct sweep_group k_sweep_groups[] = {
  {"CP",       0x00, 14, -1},
  {"RBBM",     0x01,  4, -1},
  {"PC",       0x02,  8, -1},
  {"VFD",      0x03,  8, -1},
  {"HLSQ",     0x04,  6, -1},
  {"VPC",      0x05,  6, -1},
  {"TSE",      0x06,  4, -1},
  {"RAS",      0x07,  4, -1},
  {"UCHE",     0x08, 24, -1},
  {"TP",       0x09, 12, -1},
  {"SP",       0x0a, 24, -1},
  {"RB",       0x0b,  8, -1},
  {"LRZ",      0x0e,  3,  3},  // first 3 LRZ counters only
  {"ALWAYSON", 0x1b,  1, -1},
};
static const size_t k_num_sweep_groups = sizeof(k_sweep_groups) / sizeof(k_sweep_groups[0]);

static volatile sig_atomic_t g_stop = 0;

struct benchmark_run_config {
  int bursts;
  double burst_sleep;
  int widths[MAX_WIDTH_SEQUENCE];
  int width_count;
  double width_sleep;
};

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds) {
  if (seconds < 0.000001) seconds = 0.000001;
  struct timespec req;
  req.tv_sec = (time_t)seconds;
  req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1000000000.0);
  while (!g_stop && nanosleep(&req, &req) == -1 && errno == EINTR) {}
}

static int mkdir_p(const char *path) {
  char tmp[MAX_PATH_LEN];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp)) return -1;
  memcpy(tmp, path, len + 1);
  if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0775) == -1 && errno != EEXIST) {
        fprintf(stderr, "[mkdir] failed '%s': %s\n", tmp, strerror(errno));
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0775) == -1 && errno != EEXIST) {
    fprintf(stderr, "[mkdir] failed '%s': %s\n", tmp, strerror(errno));
    return -1;
  }
  return 0;
}

static void make_timestamp(char *out, size_t out_sz) {
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, out_sz, "%Y%m%d_%H%M%S", &tmv);
}

static void csv_write_header(FILE *f, size_t active_n, const size_t *active_idx) {
  fprintf(f, "elapsed_s");
  for (size_t i = 0; i < active_n; ++i) fprintf(f, ",%s", k_counters[active_idx[i]].short_name);
  fprintf(f, "\n");
  fflush(f);
}

static void csv_write_row(FILE *f, double elapsed_s, size_t active_n, const unsigned long long *diffv) {
  fprintf(f, "%.6f", elapsed_s);
  for (size_t i = 0; i < active_n; ++i) fprintf(f, ",%llu", diffv[i]);
  fprintf(f, "\n");
}

static int activate_counter(int fd, const struct counter_desc *c, FILE *meta) {
  struct adreno_perfcounter_get p;
  memset(&p, 0, sizeof(p));
  p.group_id = c->group_id;
  p.countable_selector = c->selector;
  int ret = ioctl(fd, ADRENO_IOCTL_PERFCOUNTER_GET, &p);
  if (ret == -1) {
    fprintf(stderr, "[GET] skip: %-44s group=0x%x selector=%u: %s\n",
            c->short_name, c->group_id, c->selector, strerror(errno));
    if (meta) fprintf(meta, "GET_FAIL,%s,0x%x,%u,%s\n", c->short_name, c->group_id, c->selector, strerror(errno));
    return -1;
  }
  printf("[GET] %-44s group=0x%x selector=%u reg_low=0x%x reg_high=0x%x\n",
         c->short_name, c->group_id, c->selector, p.regster_offset_low, p.regster_offset_high);
  if (meta) fprintf(meta, "GET_OK,%s,0x%x,%u,0x%x,0x%x\n", c->short_name, c->group_id, c->selector, p.regster_offset_low, p.regster_offset_high);
  return 0;
}

static void deactivate_counter(int fd, const struct counter_desc *c) {
  struct adreno_perfcounter_put p;
  memset(&p, 0, sizeof(p));
  p.group_id = c->group_id;
  p.countable_selector = c->selector;
  if (ioctl(fd, ADRENO_IOCTL_PERFCOUNTER_PUT, &p) == -1) {
    fprintf(stderr, "[PUT] failed: %-44s group=0x%x selector=%u: %s\n",
            c->short_name, c->group_id, c->selector, strerror(errno));
  }
}

static int read_counters(int fd, unsigned int n, struct adreno_perfcounter_read_group *groups) {
  struct adreno_perfcounter_read p;
  memset(&p, 0, sizeof(p));
  p.groups = groups;
  p.num_groups = n;
  if (ioctl(fd, ADRENO_IOCTL_PERFCOUNTER_READ, &p) == -1) {
    fprintf(stderr, "[READ] failed: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

static void cleanup_active(int fd, size_t active_n, const size_t *active_idx) {
  for (size_t i = 0; i < active_n; ++i) deactivate_counter(fd, &k_counters[active_idx[i]]);
}

static int decode_wait_status(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return status;
}

static int parse_width_sequence(const char *s, int *widths, int cap) {
  if (!s || !s[0]) return 0;
  char tmp[1024];
  if (strlen(s) >= sizeof(tmp)) return -1;
  snprintf(tmp, sizeof(tmp), "%s", s);

  int n = 0;
  char *save = NULL;
  for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    while (isspace((unsigned char)*tok)) tok++;
    char *end = tok + strlen(tok);
    while (end > tok && isspace((unsigned char)end[-1])) *--end = '\0';
    if (*tok == '\0') continue;
    char *parse_end = NULL;
    long v = strtol(tok, &parse_end, 10);
    if (parse_end == tok || *parse_end != '\0' || v <= 0 || v > 1000000000L) return -1;
    if (n >= cap) return -1;
    widths[n++] = (int)v;
  }
  return n;
}

static int contains_width_arg(const char *s) {
  if (!s) return 0;
  const char *p = s;
  while ((p = strstr(p, "--width")) != NULL) {
    char before = (p == s) ? ' ' : p[-1];
    char after = p[7];
    int before_ok = isspace((unsigned char)before) || before == '\'' || before == '"';
    int after_ok = after == '\0' || isspace((unsigned char)after) || after == '=';
    if (before_ok && after_ok) return 1;
    p += 7;
  }
  return 0;
}

static int build_width_command(const char *base_cmd, int width, char *out, size_t out_sz) {
  const char *placeholder = strstr(base_cmd, "{width}");
  if (!placeholder) {
    int n = snprintf(out, out_sz, "%s --width %d", base_cmd, width);
    return (n < 0 || (size_t)n >= out_sz) ? -1 : 0;
  }

  size_t pos = 0;
  const char *p = base_cmd;
  while ((placeholder = strstr(p, "{width}")) != NULL) {
    size_t prefix_len = (size_t)(placeholder - p);
    if (pos + prefix_len >= out_sz) return -1;
    memcpy(out + pos, p, prefix_len);
    pos += prefix_len;
    int n = snprintf(out + pos, out_sz - pos, "%d", width);
    if (n < 0 || (size_t)n >= out_sz - pos) return -1;
    pos += (size_t)n;
    p = placeholder + strlen("{width}");
  }
  size_t tail_len = strlen(p);
  if (pos + tail_len >= out_sz) return -1;
  memcpy(out + pos, p, tail_len + 1);
  return 0;
}

static pid_t launch_benchmark_runner(const char *benchmark_cmd,
                                     const char *log_path,
                                     const struct benchmark_run_config *run_cfg) {
  int bursts = run_cfg ? run_cfg->bursts : DEFAULT_BURSTS;
  double burst_sleep = run_cfg ? run_cfg->burst_sleep : DEFAULT_BURST_SLEEP;
  int width_count = run_cfg ? run_cfg->width_count : 0;
  double width_sleep = run_cfg ? run_cfg->width_sleep : DEFAULT_WIDTH_SLEEP;
  if (bursts < 1) bursts = 1;
  if (burst_sleep < 0.0) burst_sleep = 0.0;
  if (width_count < 0) width_count = 0;
  if (width_sleep < 0.0) width_sleep = 0.0;

  pid_t pid = fork();
  if (pid == -1) {
    fprintf(stderr, "[bench] fork failed: %s\n", strerror(errno));
    return -1;
  }

  if (pid == 0) {
    // Put the runner and its shell children in their own process group so the
    // parent can stop the whole benchmark cleanly at the end of the stream.
    setpgid(0, 0);

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (log_fd == -1) {
      fprintf(stderr, "[bench] failed to open log %s: %s\n", log_path, strerror(errno));
      _exit(125);
    }
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);

    printf("[bench] command_base: %s\n", benchmark_cmd);
    printf("[bench] bursts: %d\n", bursts);
    printf("[bench] burst_sleep_s: %.6f\n", burst_sleep);
    printf("[bench] width_count: %d\n", width_count);
    printf("[bench] width_sleep_s: %.6f\n", width_sleep);
    if (width_count > 0 && run_cfg) {
      printf("[bench] widths:");
      for (int i = 0; i < width_count; ++i) printf(" %d", run_cfg->widths[i]);
      printf("\n");
    }
    fflush(stdout);

    int final_status = 0;
    if (width_count > 0 && run_cfg) {
      for (int w = 0; w < width_count && !g_stop; ++w) {
        char width_cmd[MAX_CMD_LEN];
        if (build_width_command(benchmark_cmd, run_cfg->widths[w], width_cmd, sizeof(width_cmd)) != 0) {
          fprintf(stderr, "[bench] width command too long for width=%d\n", run_cfg->widths[w]);
          _exit(124);
        }
        printf("[bench] width_step %d/%d width=%d start\n", w + 1, width_count, run_cfg->widths[w]);
        printf("[bench] width_step_cmd: %s\n", width_cmd);
        fflush(stdout);
        int status = system(width_cmd);
        int exit_code = (status == -1) ? 127 : decode_wait_status(status);
        printf("[bench] width_step %d/%d width=%d exit_status=%d\n", w + 1, width_count, run_cfg->widths[w], exit_code);
        fflush(stdout);
        if (exit_code != 0) {
          final_status = exit_code;
          break;
        }
        if (w + 1 < width_count && width_sleep > 0.0) {
          printf("[bench] sleeping %.6f s before next width\n", width_sleep);
          fflush(stdout);
          sleep_seconds(width_sleep);
        }
      }
    } else {
      for (int b = 1; b <= bursts && !g_stop; ++b) {
        printf("[bench] burst %d/%d start\n", b, bursts);
        fflush(stdout);
        int status = system(benchmark_cmd);
        int exit_code = (status == -1) ? 127 : decode_wait_status(status);
        printf("[bench] burst %d/%d exit_status=%d\n", b, bursts, exit_code);
        fflush(stdout);
        if (exit_code != 0) {
          final_status = exit_code;
          break;
        }
        if (b < bursts && burst_sleep > 0.0) {
          printf("[bench] sleeping %.6f s before next burst\n", burst_sleep);
          fflush(stdout);
          sleep_seconds(burst_sleep);
        }
      }
    }
    _exit(final_status);
  }

  // Best effort in case the parent runs before the child calls setpgid().
  setpgid(pid, pid);
  if (width_count > 0 && run_cfg) {
    printf("[bench] pid=%d width_count=%d width_sleep=%.6f cmd_base=%s\n", (int)pid, width_count, width_sleep, benchmark_cmd);
    printf("[bench] widths:");
    for (int i = 0; i < width_count; ++i) printf(" %d", run_cfg->widths[i]);
    printf("\n");
  } else {
    printf("[bench] pid=%d bursts=%d burst_sleep=%.6f cmd=%s\n", (int)pid, bursts, burst_sleep, benchmark_cmd);
  }
  printf("[bench] log=%s\n", log_path);
  return pid;
}

static int finish_benchmark(pid_t pid, int kill_at_end) {
  if (pid <= 0) return -1;
  int status = 0;
  pid_t r;
  do {
    r = waitpid(pid, &status, WNOHANG);
  } while (r == -1 && errno == EINTR);
  if (r == -1) {
    fprintf(stderr, "[bench] waitpid failed: %s\n", strerror(errno));
    return -1;
  }
  if (r == 0 && kill_at_end) {
    printf("[bench] benchmark still running after stream window; sending SIGTERM to benchmark process group\n");
    if (kill(-pid, SIGTERM) == -1) kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
      sleep_seconds(0.1);
      do {
        r = waitpid(pid, &status, WNOHANG);
      } while (r == -1 && errno == EINTR);
      if (r == pid) break;
      if (r == -1) {
        fprintf(stderr, "[bench] waitpid failed: %s\n", strerror(errno));
        return -1;
      }
    }
    if (r == 0) {
      printf("[bench] benchmark still running; sending SIGKILL to benchmark process group\n");
      if (kill(-pid, SIGKILL) == -1) kill(pid, SIGKILL);
      do {
        r = waitpid(pid, &status, 0);
      } while (r == -1 && errno == EINTR);
    }
  } else if (r == 0) {
    printf("[bench] benchmark still running; waiting because --no-kill-benchmark was used\n");
    do {
      r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
  }
  if (r == -1) {
    fprintf(stderr, "[bench] waitpid failed: %s\n", strerror(errno));
    return -1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return status;
}

static int collect_group_indices(const struct sweep_group *g, size_t *out, size_t cap) {
  size_t n = 0;
  int lrz_seen = 0;
  for (size_t i = 0; i < k_num_counters; ++i) {
    const struct counter_desc *c = &k_counters[i];
    if (strcasecmp(c->group_name, g->name) != 0) continue;
    if (g->selector_limit >= 0) {
      if (lrz_seen >= g->selector_limit) continue;
      ++lrz_seen;
    }
    if (n >= cap) return -1;
    out[n++] = i;
  }
  return (int)n;
}

static int run_one_chunk(int fd,
                         const struct sweep_group *g,
                         size_t chunk_id,
                         const size_t *indices,
                         size_t total_n,
                         size_t *cursor,
                         double duration,
                         double interval,
                         const char *benchmark_cmd,
                         const struct benchmark_run_config *run_cfg,
                         const char *group_dir,
                         FILE *summary,
                         int kill_benchmark_at_end) {
  char csv_path[MAX_PATH_LEN], meta_path[MAX_PATH_LEN], bench_log_path[MAX_PATH_LEN];
  snprintf(csv_path, sizeof(csv_path), "%s/%s_chunk%03zu.csv", group_dir, g->name, chunk_id);
  snprintf(meta_path, sizeof(meta_path), "%s/%s_chunk%03zu_meta.txt", group_dir, g->name, chunk_id);
  snprintf(bench_log_path, sizeof(bench_log_path), "%s/%s_chunk%03zu_benchmark.log", group_dir, g->name, chunk_id);

  FILE *meta = fopen(meta_path, "w");
  if (!meta) {
    fprintf(stderr, "[meta] cannot open %s: %s\n", meta_path, strerror(errno));
    return -1;
  }
  fprintf(meta, "group,%s\ncapacity,%zu\nduration_s,%.6f\ninterval_s,%.6f\nbursts,%d\nburst_sleep_s,%.6f\nwidth_count,%d\nwidth_sleep_s,%.6f\nbenchmark_cmd_base,%s\n",
          g->name, g->capacity, duration, interval,
          run_cfg ? run_cfg->bursts : DEFAULT_BURSTS,
          run_cfg ? run_cfg->burst_sleep : DEFAULT_BURST_SLEEP,
          run_cfg ? run_cfg->width_count : 0,
          run_cfg ? run_cfg->width_sleep : DEFAULT_WIDTH_SLEEP,
          benchmark_cmd);
  if (run_cfg && run_cfg->width_count > 0) {
    fprintf(meta, "widths");
    for (int wi = 0; wi < run_cfg->width_count; ++wi) fprintf(meta, ",%d", run_cfg->widths[wi]);
    fprintf(meta, "\n");
  }
  fprintf(meta, "activation_status,counter,group,selector,reg_low,reg_high_or_error\n");

  size_t active_idx[MAX_ACTIVE];
  size_t attempted = 0;
  size_t first_cursor = *cursor;
  size_t active_n = 0;

  while (*cursor < total_n && active_n < g->capacity && active_n < MAX_ACTIVE) {
    size_t idx = indices[*cursor];
    (*cursor)++;
    attempted++;
    if (activate_counter(fd, &k_counters[idx], meta) == 0) {
      active_idx[active_n++] = idx;
    }
  }

  if (active_n == 0) {
    fprintf(stderr, "[chunk] %s chunk %zu: no counters activated; skipping CSV/benchmark\n", g->name, chunk_id);
    fprintf(meta, "result,no_active_counters\n");
    fclose(meta);
    if (summary) fprintf(summary, "%s,%zu,%zu,%zu,%zu,%s,%s,%d\n", g->name, chunk_id, first_cursor, *cursor, active_n, csv_path, bench_log_path, -1);
    return 0;
  }

  FILE *csv = fopen(csv_path, "w");
  if (!csv) {
    fprintf(stderr, "[csv] cannot open %s: %s\n", csv_path, strerror(errno));
    cleanup_active(fd, active_n, active_idx);
    fclose(meta);
    return -1;
  }
  csv_write_header(csv, active_n, active_idx);

  struct adreno_perfcounter_read_group *read_groups = calloc(active_n, sizeof(*read_groups));
  unsigned long long *oldv = calloc(active_n, sizeof(*oldv));
  unsigned long long *diffv = calloc(active_n, sizeof(*diffv));
  if (!read_groups || !oldv || !diffv) {
    fprintf(stderr, "[alloc] failed\n");
    free(read_groups); free(oldv); free(diffv);
    fclose(csv);
    cleanup_active(fd, active_n, active_idx);
    fclose(meta);
    return -1;
  }
  for (size_t i = 0; i < active_n; ++i) {
    const struct counter_desc *c = &k_counters[active_idx[i]];
    read_groups[i].group_id = c->group_id;
    read_groups[i].countable_selector = c->selector;
  }
  if (read_counters(fd, (unsigned int)active_n, read_groups) != 0) {
    fprintf(meta, "result,initial_read_failed\n");
    free(read_groups); free(oldv); free(diffv);
    fclose(csv);
    unlink(csv_path);
    cleanup_active(fd, active_n, active_idx);
    fclose(meta);
    if (summary) fprintf(summary, "%s,%zu,%zu,%zu,%zu,%s,%s,%d\n",
                         g->name, chunk_id, first_cursor, *cursor, active_n,
                         csv_path, bench_log_path, -1);
    return -1;
  }
  for (size_t i = 0; i < active_n; ++i) oldv[i] = read_groups[i].value;

  pid_t bench_pid = launch_benchmark_runner(benchmark_cmd, bench_log_path, run_cfg);
  if (bench_pid < 0) {
    fprintf(meta, "result,benchmark_launch_failed\n");
    free(read_groups); free(oldv); free(diffv);
    fclose(csv);
    unlink(csv_path);
    cleanup_active(fd, active_n, active_idx);
    fclose(meta);
    if (summary) fprintf(summary, "%s,%zu,%zu,%zu,%zu,%s,%s,%d\n",
                         g->name, chunk_id, first_cursor, *cursor, active_n,
                         csv_path, bench_log_path, -1);
    return -1;
  }
  double t0 = now_seconds();
  size_t rows = 0;
  int sample_status = 0;
  while (!g_stop) {
    double elapsed = now_seconds() - t0;
    if (elapsed >= duration) break;
    double remaining = duration - elapsed;
    sleep_seconds(interval < remaining ? interval : remaining);
    if (g_stop) break;
    elapsed = now_seconds() - t0;
    if (read_counters(fd, (unsigned int)active_n, read_groups) != 0) {
      sample_status = -1;
      break;
    }
    for (size_t i = 0; i < active_n; ++i) {
      unsigned long long newv = read_groups[i].value;
      diffv[i] = newv - oldv[i];
      oldv[i] = newv;
    }
    csv_write_row(csv, elapsed, active_n, diffv);
    rows++;
  }
  fflush(csv);
  int bench_status = finish_benchmark(bench_pid, kill_benchmark_at_end);

  fprintf(meta, "result,%s\nactive_counters,%zu\nattempted_counters,%zu\ncsv,%s\nbenchmark_log,%s\nrows,%zu\nbenchmark_exit_status,%d\n",
          sample_status == 0 ? "ok" : "counter_read_failed",
          active_n, attempted, csv_path, bench_log_path, rows, bench_status);
  fclose(meta);
  fclose(csv);
  cleanup_active(fd, active_n, active_idx);
  free(read_groups); free(oldv); free(diffv);

  if (summary) fprintf(summary, "%s,%zu,%zu,%zu,%zu,%s,%s,%d\n", g->name, chunk_id, first_cursor, *cursor, active_n, csv_path, bench_log_path, bench_status);
  printf("[chunk] saved %s (%zu active counters, %zu rows)\n", csv_path, active_n, rows);
  return sample_status;
}

static void print_usage(const char *argv0) {
  printf("Usage:\n");
  printf("  %s --time <seconds> --benchmark-cmd '<full benchmark command>'\n\n", argv0);
  printf("Examples:\n");
  printf("  %s --time 2 --benchmark-cmd '/data/local/tmp/my_benchmark --size 1024'\n", argv0);
  printf("  %s --time 3 --bursts 10 --burst-sleep 0.1 --benchmark-cmd '/data/local/tmp/my_benchmark'\n", argv0);
  printf("  %s --time 4 --widths 128,256,512 --width-sleep 0.1 --benchmark-cmd '/data/local/tmp/my_benchmark --size {width}'\n\n", argv0);
  printf("Options:\n");
  printf("  --benchmark-cmd <command>     Required. Full command executed by /system/bin/sh. Use trusted input only.\n");
  printf("  --interval <seconds>          Sample interval. Default: %.6f\n", DEFAULT_SAMPLE_INTERVAL);
  printf("  --bursts <count>              Run the benchmark this many times inside each sampled chunk. Default: %d\n", DEFAULT_BURSTS);
  printf("  --burst-sleep <seconds>       CPU-side delay between repeated identical benchmark bursts. Default: %.6f\n", DEFAULT_BURST_SLEEP);
  printf("  --widths <csv-list>           Run widths sequentially. Replace {width} in the command, or append --width <value>.\n");
  printf("  --width-sleep <seconds>       CPU-side delay between width-sequence steps. Default: %.6f\n", DEFAULT_WIDTH_SLEEP);
  printf("  --device <path>               KGSL device. Default: %s\n", DEFAULT_DEVICE);
  printf("  --out-root <path>             Output root. Default: %s\n", DEFAULT_ROOT);
  printf("  --no-kill-benchmark           Wait for benchmark if it outlives the stream window. Default: terminate it at the end of each chunk.\n");
  printf("  --list-plan                   Print sweep groups/capacities and exit.\n");
}

static void print_plan(void) {
  printf("Sweep plan: only these groups are included. All other generated groups are ignored.\n");
  printf("%-10s %-10s %-10s\n", "Group", "Capacity", "Selector filter");
  for (size_t i = 0; i < k_num_sweep_groups; ++i) {
    const struct sweep_group *g = &k_sweep_groups[i];
    if (g->selector_limit >= 0) printf("%-10s %-10zu first %d counters\n", g->name, g->capacity, g->selector_limit);
    else printf("%-10s %-10zu all counters\n", g->name, g->capacity);
  }
}

int main(int argc, char **argv) {
  const char *dev = DEFAULT_DEVICE;
  const char *out_root = DEFAULT_ROOT;
  double duration = -1.0;
  double interval = DEFAULT_SAMPLE_INTERVAL;
  const char *benchmark_cmd_user = NULL;
  int kill_benchmark_at_end = 1;
  struct benchmark_run_config run_cfg;
  memset(&run_cfg, 0, sizeof(run_cfg));
  run_cfg.bursts = DEFAULT_BURSTS;
  run_cfg.burst_sleep = DEFAULT_BURST_SLEEP;
  run_cfg.width_sleep = DEFAULT_WIDTH_SLEEP;
  int list_plan = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) duration = atof(argv[++i]);
    else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval = atof(argv[++i]);
    else if (strcmp(argv[i], "--bursts") == 0 && i + 1 < argc) run_cfg.bursts = atoi(argv[++i]);
    else if (strcmp(argv[i], "--burst-sleep") == 0 && i + 1 < argc) run_cfg.burst_sleep = atof(argv[++i]);
    else if (strcmp(argv[i], "--widths") == 0 && i + 1 < argc) {
      int n = parse_width_sequence(argv[++i], run_cfg.widths, MAX_WIDTH_SEQUENCE);
      if (n <= 0) {
        fprintf(stderr, "Invalid --widths list. Example: --widths 128,256,512,1024,2048\n");
        return 2;
      }
      run_cfg.width_count = n;
    }
    else if (strcmp(argv[i], "--width-sleep") == 0 && i + 1 < argc) run_cfg.width_sleep = atof(argv[++i]);
    else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) dev = argv[++i];
    else if (strcmp(argv[i], "--out-root") == 0 && i + 1 < argc) out_root = argv[++i];
    else if (strcmp(argv[i], "--benchmark-cmd") == 0 && i + 1 < argc) benchmark_cmd_user = argv[++i];
    else if (strcmp(argv[i], "--no-kill-benchmark") == 0) kill_benchmark_at_end = 0;
    else if (strcmp(argv[i], "--list-plan") == 0) list_plan = 1;
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
    else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return 2;
    }
  }

  if (list_plan) {
    print_plan();
    return 0;
  }

  if (duration <= 0.0) {
    printf("Stream duration per counter chunk in seconds: ");
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return 2;
    duration = atof(line);
  }
  if (duration <= 0.0) {
    fprintf(stderr, "Duration must be positive.\n");
    return 2;
  }
  if (interval <= 0.0) interval = DEFAULT_SAMPLE_INTERVAL;
  if (run_cfg.bursts < 1) {
    fprintf(stderr, "--bursts must be >= 1.\n");
    return 2;
  }
  if (run_cfg.burst_sleep < 0.0) {
    fprintf(stderr, "--burst-sleep must be >= 0.\n");
    return 2;
  }
  if (run_cfg.width_sleep < 0.0) {
    fprintf(stderr, "--width-sleep must be >= 0.\n");
    return 2;
  }
  if (run_cfg.width_count > 0 && run_cfg.bursts != DEFAULT_BURSTS) {
    fprintf(stderr, "[warn] --widths mode ignores --bursts because widths define the internal sequence.\n");
  }
  if (run_cfg.bursts > 1 && run_cfg.burst_sleep * (double)(run_cfg.bursts - 1) >= duration) {
    fprintf(stderr, "[warn] burst sleeps alone take %.6f s, which is >= --time %.6f s. Increase --time or reduce --bursts/--burst-sleep if you want all bursts visible.\n",
            run_cfg.burst_sleep * (double)(run_cfg.bursts - 1), duration);
  }
  if (run_cfg.width_count > 1 && run_cfg.width_sleep * (double)(run_cfg.width_count - 1) >= duration) {
    fprintf(stderr, "[warn] width sleeps alone take %.6f s, which is >= --time %.6f s. Increase --time or reduce --width-sleep if you want all widths visible.\n",
            run_cfg.width_sleep * (double)(run_cfg.width_count - 1), duration);
  }

  if (!benchmark_cmd_user || !benchmark_cmd_user[0]) {
    fprintf(stderr, "--benchmark-cmd is required. The public tool has no built-in workload.\n");
    print_usage(argv[0]);
    return 2;
  }
  if (run_cfg.width_count > 0 &&
      strstr(benchmark_cmd_user, "{width}") == NULL &&
      contains_width_arg(benchmark_cmd_user)) {
    fprintf(stderr, "[warn] --widths appends --width <value>, but the command already contains --width. Prefer a {width} placeholder.\n");
  }

  char benchmark_cmd[MAX_CMD_LEN];
  int benchmark_len = snprintf(benchmark_cmd, sizeof(benchmark_cmd), "%s", benchmark_cmd_user);
  if (benchmark_len < 0 || (size_t)benchmark_len >= sizeof(benchmark_cmd)) {
    fprintf(stderr, "--benchmark-cmd is too long; maximum is %zu bytes.\n", sizeof(benchmark_cmd) - 1);
    return 2;
  }

  char stamp[64];
  make_timestamp(stamp, sizeof(stamp));
  char run_dir[MAX_PATH_LEN];
  int run_dir_len = snprintf(run_dir, sizeof(run_dir), "%s/sweep_%s_%ld",
                             out_root, stamp, (long)getpid());
  if (run_dir_len < 0 || (size_t)run_dir_len >= sizeof(run_dir)) {
    fprintf(stderr, "[run] output path is too long.\n");
    return 1;
  }
  if (mkdir_p(run_dir) != 0) return 1;

  char summary_path[MAX_PATH_LEN];
  snprintf(summary_path, sizeof(summary_path), "%s/summary.csv", run_dir);
  FILE *summary = fopen(summary_path, "w");
  if (!summary) {
    fprintf(stderr, "[summary] cannot open %s: %s\n", summary_path, strerror(errno));
    return 1;
  }
  fprintf(summary, "group,chunk,counter_start_index,counter_end_index,active_counters,csv_path,benchmark_log,benchmark_exit_status\n");

  char readme_path[MAX_PATH_LEN];
  snprintf(readme_path, sizeof(readme_path), "%s/run_config.txt", run_dir);
  FILE *cfg = fopen(readme_path, "w");
  if (cfg) {
    fprintf(cfg, "run_dir=%s\nduration_s=%.6f\ninterval_s=%.6f\nbursts=%d\nburst_sleep_s=%.6f\nwidth_count=%d\nwidth_sleep_s=%.6f\ndevice=%s\nbenchmark_cmd_base=%s\n",
            run_dir, duration, interval, run_cfg.bursts, run_cfg.burst_sleep, run_cfg.width_count, run_cfg.width_sleep, dev, benchmark_cmd);
    if (run_cfg.width_count > 0) {
      fprintf(cfg, "widths=");
      for (int wi = 0; wi < run_cfg.width_count; ++wi) fprintf(cfg, "%s%d", wi ? "," : "", run_cfg.widths[wi]);
      fprintf(cfg, "\n");
    }
    fclose(cfg);
  }

  printf("[run] output directory: %s\n", run_dir);
  printf("[run] duration per chunk: %.6f s\n", duration);
  printf("[run] interval: %.6f s\n", interval);
  printf("[run] bursts per chunk: %d\n", run_cfg.bursts);
  printf("[run] burst sleep: %.6f s\n", run_cfg.burst_sleep);
  printf("[run] width sequence count: %d\n", run_cfg.width_count);
  printf("[run] width sleep: %.6f s\n", run_cfg.width_sleep);
  if (run_cfg.width_count > 0) {
    printf("[run] width sequence:");
    for (int wi = 0; wi < run_cfg.width_count; ++wi) printf(" %d", run_cfg.widths[wi]);
    printf("\n");
  }
  printf("[run] benchmark command base: %s\n", benchmark_cmd);

  int fd = open(dev, O_RDWR);
  if (fd == -1) {
    fprintf(stderr, "open(%s) failed: %s\n", dev, strerror(errno));
    fclose(summary);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  for (size_t gi = 0; gi < k_num_sweep_groups && !g_stop; ++gi) {
    const struct sweep_group *g = &k_sweep_groups[gi];
    size_t indices[MAX_GROUP_COUNTERS];
    int n_int = collect_group_indices(g, indices, MAX_GROUP_COUNTERS);
    if (n_int < 0) {
      fprintf(stderr, "[group] too many counters in %s\n", g->name);
      continue;
    }
    size_t n = (size_t)n_int;
    if (n == 0) {
      fprintf(stderr, "[group] no generated counters found for %s\n", g->name);
      continue;
    }
    char group_dir[MAX_PATH_LEN];
    snprintf(group_dir, sizeof(group_dir), "%s/%02zu_%s", run_dir, gi + 1, g->name);
    if (mkdir_p(group_dir) != 0) break;

    printf("\n[group] %s: total selected counters=%zu, physical slots=%zu\n", g->name, n, g->capacity);
    size_t cursor = 0;
    size_t chunk_id = 0;
    while (cursor < n && !g_stop) {
      chunk_id++;
      printf("[chunk] %s chunk %zu starting at enum-filtered index %zu\n", g->name, chunk_id, cursor);
      if (run_one_chunk(fd, g, chunk_id, indices, n, &cursor, duration, interval,
                        benchmark_cmd, &run_cfg, group_dir, summary, kill_benchmark_at_end) != 0) {
        fprintf(stderr, "[chunk] failed for group %s chunk %zu; continuing to next chunk/group if possible\n", g->name, chunk_id);
      }
      fflush(summary);
      sleep_seconds(0.05);  // small gap to let PUT/GET settle before next reservation batch
    }
  }

  close(fd);
  fclose(summary);
  if (g_stop) {
    printf("[done] stopped early by signal. Partial results are in %s\n", run_dir);
    return 130;
  }
  printf("done\n");
  printf("[done] results: %s\n", run_dir);
  return 0;
}
