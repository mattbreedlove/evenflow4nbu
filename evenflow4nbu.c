#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ftw.h>  // Ensure this is included to avoid implicit declarations
#include <strings.h>  // For strcasecmp

#define VERSION "1.0.2"

/* ================================
   Vector
   ================================ */

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} vec_t;

void vec_init(vec_t *v, size_t cap) {
    v->items = malloc(cap * sizeof(char *));
    v->count = 0;
    v->capacity = cap;
}

void vec_push(vec_t *v, const char *s) {
    if (v->count == v->capacity) {
        v->capacity *= 2;
        v->items = realloc(v->items, v->capacity * sizeof(char *));
    }
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    v->items[v->count++] = copy;
}

/* ================================
   ftw callback
   ================================ */

static const char *tls_exclude_ext = NULL; // Define global exclusion extension variable
static vec_t *tls_vec = NULL;         // Define global vector for file paths

int collect_cb(const char *fpath,
               const struct stat *sb,
               int typeflag)
{
    if (typeflag != FTW_F)
        return 0;

    if (tls_exclude_ext) {
        const char *dot = strrchr(fpath, '.');
        if (dot && strcasecmp(dot, tls_exclude_ext) == 0)
            return 0;
    }

    vec_push(tls_vec, fpath);
    return 0;
}

/* ================================
   Shuffle
   ================================ */

void shuffle(vec_t *v)
{
    for (size_t i = v->count - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);
        char *tmp = v->items[i];
        v->items[i] = v->items[j];
        v->items[j] = tmp;
    }
}

/* ================================
   Worker thread
   ================================ */

typedef struct {
    const char *path;
    const char *exclude_ext;
    vec_t *out;
} walk_job_t;

void *walk_thread(void *arg)
{
    walk_job_t *job = arg;

    tls_vec = job->out;
    tls_exclude_ext = job->exclude_ext;

    /* Use ftw to traverse the directories (fallback option) */
    if (ftw(job->path, collect_cb, 32) == -1) {
        fprintf(stderr, "Error: ftw failed to traverse %s\n", job->path);
        return NULL;
    }
    
    shuffle(job->out);

    return NULL;
}

/* ================================
   Show Help or Version
   ================================ */

void show_help(const char *prog_name) {
    printf("evenflow4nbu version %s\n", VERSION);
    printf("Usage: %s SET ORA_DIR RPT_DIR [SQL_DIR]\n", prog_name);
    printf("  SET      - Number of items per stream\n");
    printf("  ORA_DIR  - Path to the Oracle file directory\n");
    printf("  RPT_DIR  - Path to the Report file directory\n");
    printf("  SQL_DIR  - Path to the SQL file directory (optional)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -v, --version     Show version\n");
    printf("  -h, --help        Show this help message\n");
    printf("  -?, /?            Show this help message\n");
}

/* ================================
   Path Validation
   ================================ */

int is_valid_directory(const char *path) {
    struct stat sb;
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        return 1; // Valid directory
    }
    return 0; // Invalid directory
}

/* ================================
   Main
   ================================ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        show_help(argv[0]);
        return 1;
    }

    // Handle version and help flags
    if (argc == 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("evenflow4nbu version %s\n", VERSION);
        return 0;
    }

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0 || strcmp(argv[1], "/?") == 0)) {
        show_help(argv[0]);
        return 0;
    }

    // Check for correct number of arguments
    if (argc < 4) {
        fprintf(stderr, "Error: Missing required arguments.\n");
        show_help(argv[0]);
        return 1;
    }

    int SET = atoi(argv[1]);
    srand((unsigned)time(NULL));

    // Validate paths
    if (!is_valid_directory(argv[2])) {
        fprintf(stderr, "Error: Invalid directory path: %s\n", argv[2]);
        return 1;
    }
    if (!is_valid_directory(argv[3])) {
        fprintf(stderr, "Error: Invalid directory path: %s\n", argv[3]);
        return 1;
    }
    if (argc > 4 && !is_valid_directory(argv[4])) {
        fprintf(stderr, "Error: Invalid directory path: %s\n", argv[4]);
        return 1;
    }

    vec_t ora, rpt, sql, all;
    vec_init(&ora, 1024);
    vec_init(&rpt, 1024);
    vec_init(&sql, 1024);
    vec_init(&all, 4096);

    pthread_t t_ora, t_rpt, t_sql;

    walk_job_t ora_job = {
        .path = argv[2],
        .exclude_ext = ".log",
        .out = &ora
    };

    walk_job_t rpt_job = {
        .path = argv[3],
        .exclude_ext = NULL,
        .out = &rpt
    };

    pthread_create(&t_ora, NULL, walk_thread, &ora_job);
    pthread_create(&t_rpt, NULL, walk_thread, &rpt_job);

    int has_sql = (argc > 4);
    walk_job_t sql_job;

    if (has_sql) {
        sql_job.path = argv[4];
        sql_job.exclude_ext = ".trn";
        sql_job.out = &sql;
        pthread_create(&t_sql, NULL, walk_thread, &sql_job);
    }

    pthread_join(t_ora, NULL);
    pthread_join(t_rpt, NULL);
    if (has_sql) pthread_join(t_sql, NULL);

    /* Merge */
    for (size_t i = 0; i < ora.count; i++) all.items[all.count++] = ora.items[i];
    for (size_t i = 0; i < rpt.count; i++) all.items[all.count++] = rpt.items[i];
    for (size_t i = 0; i < sql.count; i++) all.items[all.count++] = sql.items[i];

    shuffle(&all);

    /* Emit streams */
    int count = 0;
    int stream = 1;
    size_t total = all.count;

    printf("NEW_STREAM\n");

    for (size_t i = 0; i < all.count; i++) {
        printf("%s\n", all.items[i]);
        count++;

        if (count == SET) {
            printf("NEW_STREAM\n");
            count = 0;
            stream++;
        }
    }

    printf("Total Streams=%d\n", stream);
    printf("Total Dirs=%zu\n", total);

    return 0;
}
