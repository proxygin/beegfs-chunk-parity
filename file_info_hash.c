#include <stdint.h>

#include "file_info_hash.h"

FileInfoHash* fih_init()
{
    FileInfoHash *res = calloc(1, sizeof(FileInfoHash));
    res->h = kh_init(fih);
    return res;
}

void fih_add_info(FileInfoHash *fih, char *key, uint16_t src, uint64_t size, uint64_t time)
{
    khash_t(fih) *h = fih->h;
    int r;
    khint_t it = kh_put_fih(h, key, &r);
    FileInfo *fi = &kh_val(h, it);
    if (r == 1)
        memset(fi, 0, sizeof(FileInfo));
    else
        free(key);
    fi->byte_size = fi->byte_size > size? fi->byte_size : size;
    fi->timestamp = time;
    int i = 0;
    for (; fi->locations[i] != 0 && i < 15; i++)
    {
    }
    fi->locations[i] = src;
}

size_t fih_collect(const FileInfoHash *fih, size_t max, const char **keys, FileInfo *vals)
{
    size_t j = 0;
    khash_t(fih) *h = fih->h;
    for (khint_t i = kh_begin(h); i != kh_end(h) && j < max; i++)
        if (kh_exist(h, i)) {
            keys[j] = kh_key(h, i);
            vals[j] = kh_val(h, i);
            j += 1;
        }
    return j;
}
