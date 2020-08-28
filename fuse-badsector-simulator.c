/*
# Copyright 2020 AT&T Intellectual Property.  All other rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

/* Logical sector size for disk images */
const static size_t sector_size = 512;

/* Global variables populated in init_callback() from command-line arguments */
static char *filepath = NULL;        /* Path to the actual image file */
static char *filename = NULL;        /* Disk image file name from path */
static size_t bad_sector_count = 0;  /* Number of bad sectors */
static off_t *bad_sectors = NULL;    /* List of bad sectors */
static size_t reserve_sectors = 0;   /* Number of reserve sectors for
                                      * reallocation on write */

static int disk_image_fd = -1;       /* File descriptor to the image file */
static size_t disk_size = 0;         /* Size of the image file in bytes */

/* Command-line arguments, populated by filter_disk_opt_proc() */
struct filter_disk_options {
    char *disk_image;       /* Path to the image file */
    char *bad_sector_list;  /* List of bad sectors in the format x-y,z,... */
    char *reserve_sectors;  /* Number of reserve sectors for reallocation */
};

static struct filter_disk_options filter_disk_options = {NULL, NULL, NULL};

/* Get and return the size of the image file in bytes. Populate disk_size on the
 * first call and then simply return that on additional calls */
size_t get_disk_size()
{
    if (0 == disk_size)
    {
        struct stat st;
        stat(filter_disk_options.disk_image, &st);
        disk_size = st.st_size;
    }

    return disk_size;
}

/* Count the number of bad sectors by parsing the sector list argument */
size_t get_sector_count(const char *sector_list)
{
    size_t sector_count = 0;
    char *sector_end;
    /* Get the first sector */
    off_t first_sector = strtoul(sector_list, &sector_end, 10);

    /* If there are more sectors in the list and this isn't a range, recurse */
    if (*sector_end == ',')
        sector_count = 1 + get_sector_count(sector_end + 1);
    /* If this is a range, add the number in the range */
    else if (*sector_end == '-')
    {
        char *real_end;
        off_t last_sector = strtoul(sector_end + 1, &real_end, 10);

        sector_count = last_sector - first_sector + 1;

        /* If there are more sectors in the list, recurse */
        if (*real_end == ',')
            sector_count += get_sector_count(real_end + 1);
    }
    /* This is the end of the list */
    else
        sector_count = 1;

    return sector_count;
}

/* Create a list of bad sectors by parsing the sector list argument */
void add_bad_sectors(const char *sector_list, off_t *sector_array)
{
    char *sector_end;
    /* Get the first sector and add it to the list */
    off_t first_sector = strtoul(sector_list, &sector_end, 10);
    sector_array[0] = first_sector;

    /* If there are more sectors in the list and this isn't a range, recurse */
    if (*sector_end == ',')
        add_bad_sectors(sector_end + 1, sector_array + 1);
    /* If this is a range, add the range to the list of bad sectors */
    else if (*sector_end == '-')
    {
        char *real_end;
        off_t last_sector = strtoul(sector_end + 1, &real_end, 10);

        for (off_t i = first_sector + 1; i <= last_sector; i++)
            sector_array[i - first_sector] = i;

        /* If there are more sectors in the list, recurse */
        if (*real_end == ',')
            add_bad_sectors(real_end + 1, 
                    sector_array + last_sector - first_sector + 1);
    }
}

/* Build the list of bad sectors using the recursive helper functions above */
void build_bad_sector_list(const char *sector_list)
{
    /* If we don't have a bad sector list argument, nothing to do */
    if (NULL != sector_list)
    {
        bad_sector_count = get_sector_count(sector_list);
        bad_sectors = malloc(bad_sector_count * sizeof(off_t));
        add_bad_sectors(sector_list, bad_sectors);
    }
}

/* Repair a bad sector at the specified offset */
/* Returns 0 on success and nonzero on error (if there are no reserve sectors
 * available) */
int repair_bad_sector(off_t sector)
{
    int ret = -1;

    if (reserve_sectors > 0)
    {
        /* Make sure the sector provided is in the bad sector list */
        for (off_t i = 0; i < bad_sector_count; i++)
            if (bad_sectors[i] == sector)
            {
                /* Decrement the number of bad sectors, remove the sector from
                 * the bad sector list, and decrement the number of reserve
                 * sectors */
                bad_sector_count--;
                off_t *old_bad_sectors = bad_sectors;
                bad_sectors = malloc(bad_sector_count * sizeof(off_t));
                memcpy(bad_sectors, old_bad_sectors, i * sizeof(off_t));
                memcpy(bad_sectors + i,
                        old_bad_sectors + i + 1,
                        (bad_sector_count - i) * sizeof(off_t));
                free(old_bad_sectors);
                reserve_sectors--;
                ret = 0;
                break;
            }
    }

    return ret;
}

/* getattr() FUSE callback */
static int getattr_callback(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    if (strcmp(path, filepath) == 0) {
        struct stat st;
        stat(filter_disk_options.disk_image, &st);
        stbuf->st_mode = S_IFREG | 0777;
        stbuf->st_nlink = 1;
        stbuf->st_size = st.st_size;
        return 0;
    }

    return -ENOENT;
}

/* readdir() FUSE callback */
static int readdir_callback(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi) {
  (void) offset;
  (void) fi;

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  filler(buf, filename, NULL, 0);

  return 0;
}

/* open() FUSE callback */
static int open_callback(const char *path, struct fuse_file_info *fi) {
  return 0;
}

/* read() FUSE callback */
static int read_callback(const char *path, char *buf, size_t size, off_t offset,
    struct fuse_file_info *fi)
{
    if (strcmp(path, filepath) == 0)
    {
        size_t len = get_disk_size();
        if (offset >= len)
        {
            printf("Tried to read after the end of the disk at offset %li\n", offset);
            return 0;
        }

        if (offset + size > len)
        {
            printf("Tried to read past the end of the disk, truncating size from %lu to %lu\n", size, len - offset);
            size = len - offset;
        }

        off_t first_sector = offset / sector_size;
        off_t last_sector = (offset + size + sector_size - 1) / sector_size;

        for (off_t sector = first_sector; sector <= last_sector; sector++)
            for (size_t i = 0; i < bad_sector_count; i++)
                if (sector == bad_sectors[i])
                {
                    errno = EIO;
                    return -1;
                }

        return pread(disk_image_fd, buf, size, offset);
    }

    printf("Trying to read from %s!\n", path);
    return -ENOENT;
}

/* write() FUSE callback */
static int write_callback(const char *path, const char *buf, size_t size,
    off_t offset, struct fuse_file_info *fi)
{
    if (strcmp(path, filepath) == 0)
    {
        size_t len = get_disk_size();
        if (offset >= len)
        {
            printf("Tried to write after the end of the disk at offset %li\n", offset);
            return 0;
        }

        if (offset + size > len)
        {
            printf("Tried to write past the end of the disk, truncating size from %lu to %lu\n", size, len - offset);
            size = len - offset;
        }

        off_t first_sector = offset / sector_size;
        off_t last_sector = (offset + size + sector_size - 1) / sector_size;

        for (off_t sector = first_sector; sector <= last_sector; sector++)
            for (size_t i = 0; i < bad_sector_count; i++)
                if (sector == bad_sectors[i])
                    if (0 == repair_bad_sector(sector))
                        break;
                    else
                    {
                        errno = EIO;
                        return -1;
                    }

        return pwrite(disk_image_fd, buf, size, offset);
    }

    printf("Trying to write to %s!\n", path);
    return -ENOENT;
}

/* init() FUSE callback */
static void *init_callback(struct fuse_conn_info *conn)
{
    filepath = filter_disk_options.disk_image
            + strlen(filter_disk_options.disk_image)
            - 1;

    while ((filepath > filter_disk_options.disk_image) && (*filepath != '/'))
        filepath--;

    if (*filepath != '/')
    {
        filepath = malloc(strlen(filter_disk_options.disk_image) + 1);
        strcpy(filepath, "/");
        strcpy(filepath + 1, filter_disk_options.disk_image);
        filename = filter_disk_options.disk_image;
    }
    else
    {
        filename = filepath + 1;
    }

    disk_image_fd = open(filter_disk_options.disk_image, O_RDWR);

    if (NULL == filter_disk_options.reserve_sectors)
        reserve_sectors = 0;
    else
        reserve_sectors = strtoul(filter_disk_options.reserve_sectors, NULL, 10);

    build_bad_sector_list(filter_disk_options.bad_sector_list);

    return NULL;
}

/* destroy() FUSE callback */
static void destroy_callback(void *private_data)
{
    printf("Entering destroy_callback()\n");
    if (NULL != bad_sectors)
        free(bad_sectors);

    if (-1 != disk_image_fd)
    {
        fsync(disk_image_fd);
        close(disk_image_fd);
        disk_image_fd = -1;
    }
}

/* acess() FUSE callback */
static int access_callback(const char *path, int permissions)
{
    return access(filter_disk_options.disk_image, permissions);
}

/* flush() FUSE callback */
static int flush_callback(const char *path, struct fuse_file_info *fi)
{
    return fsync(disk_image_fd);
}

/* release() FUSE callback */
static int release_callback(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

/* fsync() FUSE callback */
static int fsync_callback(const char *path, int datasync,
    struct fuse_file_info *fi)
{
    return fsync(disk_image_fd);
}

/* fgetattr() FUSE callback */
static int fgetattr_callback(const char *path, struct stat *fstat, struct fuse_file_info *fi)
{
    return getattr_callback(path, fstat);
}

/* FUSE callback function pointers */
static struct fuse_operations filter_disk_operations = {
    .access = access_callback,
    .destroy = destroy_callback,
    .fgetattr = fgetattr_callback,
    .flush = flush_callback,
    .fsync = fsync_callback,
    .getattr = getattr_callback,
    .init = init_callback,
    .open = open_callback,
    .read = read_callback,
    .readdir = readdir_callback,
    .release = release_callback,
    .write = write_callback,
};

/* Keys for command-line arguments */
enum {
    KEY_HELP,
    KEY_VERSION,
    KEY_DISK_IMAGE,
    KEY_DISK_IMAGE_LONG,
    KEY_BAD_SECTOR_LIST,
    KEY_BAD_SECTOR_LIST_LONG,
    KEY_RESERVE_SECTORS,
    KEY_RESERVE_SECTORS_LONG
};

/* FUSE command-line arguments */
static struct fuse_opt filter_disk_opts[] = {
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    {"-i %s", offsetof(struct filter_disk_options, disk_image), KEY_DISK_IMAGE},
    {"--diskimage=%s", offsetof(struct filter_disk_options, disk_image),
     KEY_DISK_IMAGE_LONG},
    {"-s %s", offsetof(struct filter_disk_options, bad_sector_list),
     KEY_BAD_SECTOR_LIST},
    {"--badsectors=%s", offsetof(struct filter_disk_options, bad_sector_list),
     KEY_BAD_SECTOR_LIST_LONG},
    {"-r %s", offsetof(struct filter_disk_options, reserve_sectors),
     KEY_RESERVE_SECTORS},
    {"--reservesectors=%s", offsetof(struct filter_disk_options, reserve_sectors),
     KEY_RESERVE_SECTORS_LONG},
    FUSE_OPT_END
};

/* Documents supported comnmand-line arguments */
static void usage(const char *progname)
{
	fprintf(stderr,
"Usage: %s mountpoint [options]\n"
"\n"
"General options:\n"
"    -h   --help            print help\n"
"    -V   --version         print version\n"
"    -i   --diskimage       path to disk image to filter\n"
"    -s   --badsectors      list of bad sectors, use , to delimit and - for ranges []\n"
"    -r   --reservesectors  number of reserve sectors for reallocation [0]\n"
"\n", progname);
}

/* Process command-line arguments and populate the filter_disk_options struct */
static int filter_disk_opt_proc(void *data, const char *arg, int key,
			    struct fuse_args *outargs)
{
    if (key == KEY_HELP)
    {
        usage(outargs->argv[0]);
        fuse_opt_add_arg(outargs, "-ho");
        fuse_main(outargs->argc, outargs->argv, &filter_disk_operations, NULL);
        exit(1);
    }

    if (key == KEY_VERSION)
    {
        fuse_opt_add_arg(outargs, "--version");
        fuse_main(outargs->argc, outargs->argv, &filter_disk_operations, NULL);
        exit(0);
    }

    if (key == KEY_DISK_IMAGE)
    {
        if (filter_disk_options.disk_image != NULL) {
            free(filter_disk_options.disk_image);
            filter_disk_options.disk_image = NULL;
        }
        filter_disk_options.disk_image = strdup(arg + 2);
        return 0;
    }

    if (key == KEY_BAD_SECTOR_LIST)
    {
        if (filter_disk_options.bad_sector_list != NULL) {
            free(filter_disk_options.bad_sector_list);
            filter_disk_options.bad_sector_list = NULL;
        }
        filter_disk_options.bad_sector_list = strdup(arg + 2);
        return 0;
    }

    if (key == KEY_RESERVE_SECTORS)
    {
        if (filter_disk_options.reserve_sectors != NULL) {
            free(filter_disk_options.reserve_sectors);
            filter_disk_options.reserve_sectors = NULL;
        }
        filter_disk_options.reserve_sectors = strdup(arg + 2);
        return 0;
    }

    return 1;
}

/* Main */
int main(int argc, char *argv[])
{
    /* Use fuse_opt_parse() to parse the command line and call fuse_main() */
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &filter_disk_options, filter_disk_opts,
        filter_disk_opt_proc) == -1)
        exit(1);

    return fuse_main(args.argc, args.argv, &filter_disk_operations, NULL);
}
