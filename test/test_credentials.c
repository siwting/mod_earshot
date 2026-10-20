#include "../src/es_credentials.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static void save(const char *path, const void *data, size_t size, mode_t mode) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(data, 1, size, f) == size); fclose(f); assert(!chmod(path, mode));
}
int main(void) {
    char dir[] = "/tmp/earshot-credentials-XXXXXX", path[256], link[256], out[ES_SECRET_MAX+1];
    char big[ES_SECRET_MAX+4];
    assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/key",dir);snprintf(link,sizeof(link),"%s/link",dir);
    save(path,"test-only-key\r\n",15,0640);
    assert(!es_secret_read(out,path));assert(!strcmp(out,"test-only-key"));
    es_secret_wipe(out,sizeof(out));for(size_t i=0;i<sizeof(out);i++)assert(out[i]==0);
    assert(!symlink(path,link));assert(es_secret_read(out,link)<0);unlink(link);
    assert(!chmod(path,0644));assert(es_secret_read(out,path)<0);
    assert(!chmod(path,0660));assert(es_secret_read(out,path)<0);
    save(path,"",0,0600);assert(es_secret_read(out,path)<0);
    save(path,"key\nInjected: value",19,0600);assert(es_secret_read(out,path)<0);
    save(path,"key\0hidden",10,0600);assert(es_secret_read(out,path)<0);
    memset(big,'x',sizeof(big));save(path,big,sizeof(big),0600);assert(es_secret_read(out,path)<0);
    save(path,big,ES_SECRET_MAX,0600);assert(!es_secret_read(out,path));assert(strlen(out)==ES_SECRET_MAX);
    assert(es_secret_copy(out,"bad\r\nheader")<0);assert(es_secret_copy(out,"bad key")<0);
    assert(es_secret_read(out,dir)<0);assert(es_secret_read(out,"relative")<0);
    unlink(path);rmdir(dir);
    puts("credential permission, size, injection, symlink and wipe checks passed");return 0;
}
