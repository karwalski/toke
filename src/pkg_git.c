/*
 * pkg_git.c — git-based package fetch (story 76.1.3d, ADR-0004).
 *
 * See pkg_git.h.  git runs as a child process from an argument vector
 * (execvp, no shell), with the repository-locating GIT_* variables removed
 * and GIT_TERMINAL_PROMPT=0 so a fetch never stops to ask for a password.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pkg_git.h"

static void
errf(char *buf, int bufsz, const char *fmt, ...)
{
    if (!buf || bufsz <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)bufsz, fmt, ap);
    va_end(ap);
}

/* ── Filesystem helpers ─────────────────────────────────────────────────── */

static int
rm_one(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb; (void)ftw;
    if (flag == FTW_DP || flag == FTW_D) return rmdir(path) == 0 ? 0 : -1;
    return unlink(path) == 0 ? 0 : -1;
}

int
pkg_rmtree(const char *path)
{
    struct stat st;
    if (!path || !path[0]) return -1;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;
    return nftw(path, rm_one, 32, FTW_DEPTH | FTW_PHYS) == 0 ? 0 : -1;
}

int
pkg_mkdirs(const char *path)
{
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char c = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        buf[i] = c;
    }
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

/* ── Running git ────────────────────────────────────────────────────────── */

/* Variables that would point git at some other repository, index or object
 * store than the one named on its command line. */
static const char *const s_git_env_drop[] = {
    "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE", "GIT_COMMON_DIR",
    "GIT_CEILING_DIRECTORIES", "GIT_DISCOVERY_ACROSS_FILESYSTEM",
    "GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT",
    "GIT_EXTERNAL_DIFF", "GIT_EXEC_PATH", "GIT_TEMPLATE_DIR", NULL
};

/* Append up to cap-1 bytes read from fd to buf (len tracked in *n). */
static int
drain(int fd, char *buf, size_t cap, size_t *n)
{
    char tmp[4096];
    ssize_t r = read(fd, tmp, sizeof tmp);
    if (r <= 0) return (r < 0 && errno == EINTR) ? 1 : 0;
    size_t room = (*n + 1 < cap) ? cap - 1 - *n : 0;
    size_t take = (size_t)r < room ? (size_t)r : room;
    if (take) { memcpy(buf + *n, tmp, take); *n += take; buf[*n] = '\0'; }
    return 1;
}

/*
 * git_run — run git with argv (argv[0] = "git"), capturing stdout and
 * stderr.  Returns git's exit status, or -1 if it could not be started or
 * was killed.
 */
static int
git_run(char *const argv[], char *out, size_t outsz, char *err, size_t errsz)
{
    int po[2], pe[2];
    if (out && outsz) out[0] = '\0';
    if (err && errsz) err[0] = '\0';
    if (pipe(po) != 0) return -1;
    if (pipe(pe) != 0) { close(po[0]); close(po[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(po[1], STDOUT_FILENO);
        dup2(pe[1], STDERR_FILENO);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        for (int i = 0; s_git_env_drop[i]; i++) unsetenv(s_git_env_drop[i]);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("GIT_ASKPASS", "true", 1);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(po[1]); close(pe[1]);

    size_t on = 0, en = 0;
    char odummy[1], edummy[1];
    char *ob = (out && outsz) ? out : odummy;
    char *eb = (err && errsz) ? err : edummy;
    size_t ocap = (out && outsz) ? outsz : 1, ecap = (err && errsz) ? errsz : 1;
    struct pollfd fds[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
    int open_fds = 2;
    while (open_fds > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < 2; k++) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int more = (k == 0) ? drain(fds[k].fd, ob, ocap, &on)
                                : drain(fds[k].fd, eb, ecap, &en);
            if (!more) { close(fds[k].fd); fds[k].fd = -1; open_fds--; }
        }
    }
    for (int k = 0; k < 2; k++) if (fds[k].fd >= 0) close(fds[k].fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/* The first line of git's stderr, for a diagnostic message. */
static void
first_line(const char *in, char *outb, int outsz)
{
    const char *p = in;
    /* skip blank lines and git's "hint:" lines */
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0 && strncmp(p, "hint:", 5) != 0) {
            if ((int)len >= outsz) len = (size_t)outsz - 1;
            memcpy(outb, p, len);
            outb[len] = '\0';
            return;
        }
        if (!e) break;
        p = e + 1;
    }
    snprintf(outb, (size_t)outsz, "(no message)");
}

/* Build "git <protocol options> <rest...>" into argv.  The protocol options
 * restrict the transport at git's layer: nothing is allowed but https and
 * ssh, plus file under the explicit override. */
#define GIT_MAX_ARGV 32
static int
git_argv(char *argv[GIT_MAX_ARGV], const char *dir, ...)
{
    int n = 0;
    argv[n++] = "git";
    if (dir) { argv[n++] = "-C"; argv[n++] = (char *)dir; }
    argv[n++] = "-c"; argv[n++] = "protocol.allow=never";
    argv[n++] = "-c"; argv[n++] = "protocol.https.allow=always";
    argv[n++] = "-c"; argv[n++] = "protocol.ssh.allow=always";
    argv[n++] = "-c";
    argv[n++] = pkg_allow_file_sources() ? "protocol.file.allow=always"
                                         : "protocol.file.allow=never";
    argv[n++] = "-c"; argv[n++] = "core.hooksPath=/dev/null";
    argv[n++] = "-c"; argv[n++] = "advice.detachedHead=false";
    va_list ap;
    va_start(ap, dir);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < GIT_MAX_ARGV - 1)
        argv[n++] = (char *)a;
    va_end(ap);
    argv[n] = NULL;
    return n;
}

int
pkg_git_fetch_tag(const char *url, const char *tag, const char *dest,
                  char commit[PKG_MAX_COMMIT], char *err, int errsz)
{
    char out[8192], gerr[8192], line[512], ref[160], refspec[340], rev[180];
    char *argv[GIT_MAX_ARGV];
    commit[0] = '\0';

    if (!pkg_source_valid(url)) {
        errf(err, errsz, "source '%s' is not an allowed transport", url);
        return PKG_GIT_FAILED;
    }
    snprintf(ref, sizeof ref, "refs/tags/%s", tag);
    snprintf(refspec, sizeof refspec, "+%s:%s", ref, ref);
    snprintf(rev, sizeof rev, "%s^{commit}", ref);

    /* 1. Is there such a tag?  An empty answer is E9014, a failure E9013. */
    git_argv(argv, NULL, "ls-remote", "--tags", "--", url, ref, (char *)NULL);
    int rc = git_run(argv, out, sizeof out, gerr, sizeof gerr);
    if (rc != 0) {
        first_line(gerr, line, (int)sizeof line);
        errf(err, errsz, rc < 0 || rc == 127 ? "git could not be run: %s"
                                             : "git ls-remote failed: %s",
             rc < 0 || rc == 127 ? "is git installed and on PATH?" : line);
        return PKG_GIT_FAILED;
    }
    if (out[0] == '\0') {
        errf(err, errsz, "no tag %s in %s", tag, url);
        return PKG_GIT_NOTAG;
    }

    /* 2. An empty repository, no templates (so no sample hooks), then the
     *    one tag, shallow. */
    git_argv(argv, NULL, "init", "-q", "--template=", "--", dest, (char *)NULL);
    rc = git_run(argv, out, sizeof out, gerr, sizeof gerr);
    if (rc != 0) {
        first_line(gerr, line, (int)sizeof line);
        errf(err, errsz, "git init failed: %s", line);
        pkg_rmtree(dest);
        return PKG_GIT_FAILED;
    }
    git_argv(argv, dest, "fetch", "-q", "--no-tags", "--depth=1", "--",
             url, refspec, (char *)NULL);
    rc = git_run(argv, out, sizeof out, gerr, sizeof gerr);
    if (rc != 0) {
        first_line(gerr, line, (int)sizeof line);
        errf(err, errsz, "git fetch of %s failed: %s", tag, line);
        pkg_rmtree(dest);
        return PKG_GIT_FAILED;
    }

    /* 3. The commit the tag names (an annotated tag is peeled). */
    git_argv(argv, dest, "rev-parse", "--verify", "-q", rev, (char *)NULL);
    rc = git_run(argv, out, sizeof out, gerr, sizeof gerr);
    size_t ol = strlen(out);
    while (ol > 0 && (out[ol - 1] == '\n' || out[ol - 1] == '\r')) out[--ol] = '\0';
    if (rc != 0 || !pkg_commit_valid(out)) {
        errf(err, errsz, "tag %s does not name a commit", tag);
        pkg_rmtree(dest);
        return PKG_GIT_FAILED;
    }
    snprintf(commit, PKG_MAX_COMMIT, "%s", out);

    /* 4. Check it out, then keep only the working tree. */
    git_argv(argv, dest, "checkout", "-q", "--detach", commit, (char *)NULL);
    rc = git_run(argv, out, sizeof out, gerr, sizeof gerr);
    if (rc != 0) {
        first_line(gerr, line, (int)sizeof line);
        errf(err, errsz, "git checkout of %s failed: %s", tag, line);
        pkg_rmtree(dest);
        commit[0] = '\0';
        return PKG_GIT_FAILED;
    }
    char gitdir[4096];
    snprintf(gitdir, sizeof gitdir, "%s/.git", dest);
    if (pkg_rmtree(gitdir) != 0) {
        errf(err, errsz, "could not remove %s", gitdir);
        pkg_rmtree(dest);
        commit[0] = '\0';
        return PKG_GIT_FAILED;
    }
    return PKG_GIT_OK;
}
