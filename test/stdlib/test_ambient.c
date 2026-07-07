/* Verifies the six 124.4h ambient hardening fixes at the stdlib C level. */
#include "../../src/stdlib/path.h"
#include "../../src/stdlib/file.h"
#include "../../src/stdlib/env.h"
#include "../../src/stdlib/os.h"
#include "../../src/stdlib/capabilities.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int fails = 0;
#define CHECK(c,m) do{ if(c) printf("pass: %s\n",(m)); else {printf("FAIL: %s\n",(m)); fails++;} }while(0)

int main(void){
    /* Grant everything so os.* (now deny-by-default) is callable in this harness. */
    char *av[] = {"t","--allow-all",NULL};
    tk_cap_init(2, av);

    /* AMB-06: path_normalize */
    CHECK(strcmp(path_normalize("a/b/../c"),"a/c")==0, "AMB-06 a/b/../c -> a/c");
    CHECK(strcmp(path_normalize("a/../../x"),"x")==0,  "AMB-06 a/../../x -> x (clamped)");
    CHECK(strcmp(path_normalize("./a//b"),"a/b")==0,   "AMB-06 ./a//b -> a/b");
    CHECK(strcmp(path_normalize("/a/../.."),"/")==0,   "AMB-06 /a/../.. -> / (abs clamp)");

    /* Build a sandbox with an internal symlink to an external dir. */
    char tmpl[] = "/tmp/amb_XXXXXX";
    char *root = mkdtemp(tmpl);
    char ext[256], extfile[256], tree[256], link[256];
    snprintf(ext,sizeof ext,"%s_ext",root);
    mkdir(ext,0755);
    snprintf(extfile,sizeof extfile,"%s/keep.txt",ext);
    FILE *ef=fopen(extfile,"w"); fputs("survive",ef); fclose(ef);
    snprintf(tree,sizeof tree,"%s/tree",root); mkdir(tree,0755);
    snprintf(link,sizeof link,"%s/link",tree);
    symlink(ext, link);

    /* AMB-05: rmdir_r must remove the symlink, not recurse into the external dir. */
    file_rmdir_r(tree);
    CHECK(access(extfile,F_OK)==0, "AMB-05 external file survives rmdir_r of symlinked tree");

    /* AMB-07: opening a symlink fails (ELOOP). */
    char real[256], sym[256];
    snprintf(real,sizeof real,"%s/real.txt",root);
    FILE *rf=fopen(real,"w"); fputs("data",rf); fclose(rf);
    snprintf(sym,sizeof sym,"%s/sym.txt",root);
    symlink(real, sym);
    StrFileResult viaSym = file_read(sym);
    CHECK(viaSym.is_err==1, "AMB-07 file_read through a symlink fails (O_NOFOLLOW)");
    StrFileResult viaReal = file_read(real);
    CHECK(viaReal.is_err==0 && strcmp(viaReal.ok,"data")==0, "AMB-07 real file still reads");

    /* AMB-04: dotenv denylist. */
    char envf[256]; snprintf(envf,sizeof envf,"%s/.env",root);
    FILE *evf=fopen(envf,"w"); fputs("LD_PRELOAD=/evil.so\nFOO=bar\nPATH=/evil\n",evf); fclose(evf);
    env_file_load(envf);
    CHECK(getenv("FOO")!=NULL && strcmp(getenv("FOO"),"bar")==0, "AMB-04 dotenv sets FOO");
    CHECK(getenv("LD_PRELOAD")==NULL, "AMB-04 dotenv refuses LD_PRELOAD");

    /* AMB-08: os.read/os.write are str-based (no raw pointer). Round-trip via a temp file. */
    char of[256]; snprintf(of,sizeof of,"%s/os.txt",root);
    int fd = open(of, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    int64_t nw = tk_os_write((int64_t)fd, (int64_t)(intptr_t)"hello os");
    close(fd);
    CHECK(nw==8, "AMB-08 os.write(str) wrote 8 bytes");
    int fd2 = open(of, O_RDONLY);
    int64_t s = tk_os_read((int64_t)fd2, 8);
    close(fd2);
    CHECK(strcmp((const char*)(intptr_t)s,"hello os")==0, "AMB-08 os.read -> str round-trips");

    if(fails==0){ printf("All AMB hardening checks passed.\n"); return 0; }
    fprintf(stderr,"%d failed\n",fails); return 1;
}
