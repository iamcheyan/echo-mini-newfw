/* Persistent favourites stored in reserved NEWFWLOG.TXT sectors. */
#include "ui_priv.h"

struct fav {
    uint32_t cluster, size, serial;
    uint16_t flags;
    uint8_t sd, rsv;
    char name[40];
};
#define FAV_PER 8
#define FAV_SECTORS 8
#define FAV_MAX (FAV_PER * FAV_SECTORS)
struct fav_sector {
    char magic[8];
    struct fav e[FAV_PER];
    uint32_t sum;
    uint32_t pad[13];
};
_Static_assert(sizeof(struct fav_sector) == 512, "favourite record must be one sector");

static struct fav fav[FAV_MAX];
static int fav_ready;
static struct entry view[FAV_MAX];
static char view_name[FAV_MAX][40];
static int view_n;

static uint32_t sector_sum(const struct fav_sector *s)
{
    const uint32_t *w = (const uint32_t *)s;
    uint32_t sum = 0x46415653;
    for (unsigned i = 0; i < __builtin_offsetof(struct fav_sector, sum) / 4; i++)
        sum = (sum << 5 | sum >> 27) ^ w[i];
    return sum;
}

static int visible(const struct fav *f)
{
    return f->cluster && (!f->sd ||
           (sdvol.mounted && sd_present() && sdvol.serial == f->serial));
}

void fav_load(void)
{
    static struct fav_sector s;
    for (int i = 0; i < FAV_MAX; i++)
        fav[i].cluster = 0;
    int n = log_aux_count();
    for (int k = 0; k < n && k < FAV_SECTORS; k++) {
        if (log_aux_read(k, &s) || __builtin_memcmp(s.magic, "NEWFWFAV", 8) ||
            s.sum != sector_sum(&s))
            continue;
        for (int j = 0; j < FAV_PER; j++)
            fav[k * FAV_PER + j] = s.e[j];
    }
    fav_ready = 1;
}

static int fav_save(int idx)
{
    static struct fav_sector s;
    int k = idx / FAV_PER;
    __builtin_memset(&s, 0, sizeof s);
    __builtin_memcpy(s.magic, "NEWFWFAV", 8);
    for (int j = 0; j < FAV_PER; j++)
        s.e[j] = fav[k * FAV_PER + j];
    s.sum = sector_sum(&s);
    return log_aux_write(k, &s);
}

static int matches(const struct fav *f, const struct entry *e)
{
    int on_sd = (e->flags & F_SD) != 0;
    return f->cluster && f->cluster == e->cluster && f->size == e->size && f->sd == on_sd &&
           (!on_sd || f->serial == sdvol.serial);
}

int fav_find(const struct entry *e)
{
    if (!fav_ready || !e || (e->flags & F_DIR))
        return -1;
    for (int i = 0; i < FAV_MAX; i++)
        if (matches(&fav[i], e))
            return i;
    return -1;
}

/* 1 added, 0 removed, -2 no suitable log file, -3 full, -1 write error/invalid track */
int fav_toggle(const struct entry *e, const char *name)
{
    if (!fav_ready || !e || (e->flags & F_DIR) || !e->cluster || !e->size)
        return -1;
    if (log_aux_count() <= 0)
        return -2;
    int i = fav_find(e), result = 1;
    if (i < 0) {
        for (i = 0; i < FAV_MAX && fav[i].cluster; i++)
            ;
        if (i == FAV_MAX)
            return -3;
    } else {
        result = 0;
    }
    struct fav old = fav[i];
    if (result) {
        struct fav *f = &fav[i];
        f->cluster = e->cluster;
        f->size = e->size;
        f->flags = e->flags;
        f->sd = (e->flags & F_SD) != 0;
        f->serial = f->sd ? sdvol.serial : 0;
        f->rsv = 0;
        copy_name(f->name, name, sizeof f->name);
    } else {
        fav[i].cluster = 0;
    }
    if (fav_save(i)) {
        fav[i] = old;
        return -1;
    }
    return result;
}

int fav_count(void)
{
    int n = 0;
    for (int i = 0; i < FAV_MAX; i++)
        n += visible(&fav[i]);
    return n;
}

int fav_get(int index, struct entry *e, char *name, int max)
{
    if (!fav_ready || index < 0)
        return -1;
    for (int i = 0, n = 0; i < FAV_MAX; i++) {
        if (!visible(&fav[i]))
            continue;
        if (n++ != index)
            continue;
        *e = (struct entry){ .cluster = fav[i].cluster, .size = fav[i].size,
                             .name = 0, .flags = fav[i].flags };
        copy_name(name, fav[i].name, max);
        return 0;
    }
    return -1;
}

int fav_view_load(void)
{
    view_n = 0;
    for (int i = 0; i < fav_count() && i < FAV_MAX; i++) {
        if (!fav_get(i, &view[view_n], view_name[view_n], sizeof view_name[view_n]))
            view_n++;
    }
    return view_n;
}

const struct entry *fav_view_entry(int i)
{
    return i >= 0 && i < view_n ? &view[i] : 0;
}

const char *fav_view_name(int i)
{
    return i >= 0 && i < view_n ? view_name[i] : "";
}
