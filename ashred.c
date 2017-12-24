#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/disk.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

static int shred_file(char* filename);
static int shred(int fd);
static ssize_t shred_from(int rfd, int wfd, struct aiocb* bl, int nbl,
    size_t len);

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "Please specify a file/device to shred\n");
        return -1;
    }

    return shred_file(argv[1]);
}

int shred_file(char* filename)
{
    int fd = open(filename, O_WRONLY);
    if (fd == -1) {
        return errno;
    }

    int res = shred(fd);
    close(fd);
    if (res != 0) {
        printf("%s failed with error %d: %s. Exiting.\n", filename, res,
            strerror(res));
    }

    return res;
}

int get_size(int fd, off_t* size)
{
    struct stat info = { 0 };
    int err = fstat(fd, &info);
    if (err != 0) {
        return err;
    }

    off_t total_size = info.st_size;
    if (S_ISCHR(info.st_mode)) {
        err = ioctl(fd, DIOCGMEDIASIZE, &total_size);
        if (err != 0) {
            return err;
        }
    }

    *size = total_size;
    return 0;
}

void bl_init(struct aiocb* bl, int rfd, size_t buf_size)
{
    memset(bl, 0, sizeof(*bl));

    bl->aio_fildes = rfd;
    bl->aio_buf = malloc(buf_size);
    bl->aio_nbytes = buf_size;
    bl->aio_sigevent.sigev_notify = SIGEV_NONE;
}

int shred(int fd)
{
    struct timeval tvstart, tvend;
    int randfd = open("/dev/urandom", O_RDONLY | O_DIRECT);
    if (randfd < 0) {
        printf("Can't open /dev/urandom, error %s\n", strerror(errno));
        return errno;
    }

    const size_t blcnt = 8;
    struct aiocb* bl = malloc(blcnt * sizeof(struct aiocb));
    const size_t buf_size = 128 * 1024;

    gettimeofday(&tvstart, NULL);
    for (int i = 0; i < blcnt; ++i) {
        bl_init(bl + i, randfd, buf_size);
        ssize_t res = read(bl[i].aio_fildes, (void*)bl[i].aio_buf, bl[i].aio_nbytes);
        if (res < 0) {
            printf("Failed initial read");
            return errno;
        }
    }

    off_t total_size = 0;
    int err = get_size(fd, &total_size);
    if (err != 0) {
        return err;
    }

    ssize_t bytes_written = shred_from(randfd, fd, bl, blcnt, total_size);
    if (bytes_written < 0) {
        return errno;
    }

    close(randfd);
    for (int i = 0; i < blcnt; ++i) {
        free((void*)bl[i].aio_buf);
    }

    gettimeofday(&tvend, NULL);
    double elapsed = (double)(tvend.tv_usec - tvstart.tv_usec) / 1000000 + (double)(tvend.tv_sec - tvstart.tv_sec);
    printf("%zd bytes in %g seconds = %g bytes/s\n", bytes_written, elapsed,
        (double)bytes_written / elapsed);

    return 0;
}

struct aiocb const* const*
get_bl_list_from_array(struct aiocb* bl, int nbl)
{
    struct aiocb** list = malloc(nbl * sizeof(struct aiocb*));
    for (int i = 0; i < nbl; ++i) {
        list[i] = bl + i;
    }
    return (struct aiocb const* const*)list;
}

void free_bl_list(struct aiocb const* const* bl_list)
{
    free((void*)bl_list);
}

/*
 * Dispatches writes from rfd to wfd with the goal of maximinzing
 * write throughput.
 */
static ssize_t
shred_from(int rfd, int wfd, struct aiocb* bl, int nbl, size_t len)
{
    off_t off = 0;
    size_t total = 0;
    struct aiocb const* const* bl_list = get_bl_list_from_array(bl, nbl);

    while (off < len) {
        int nread = 0;
        for (int i = 0; i < nbl; ++i) {
            if (bl[i].aio_fildes == rfd) {
                bl[i].aio_fildes = wfd;
                bl[i].aio_offset = off;
                if (bl[i].aio_nbytes + off >= len) {
                    bl[i].aio_nbytes = len - off;
                }

                if (aio_write(bl + i) < 0) {
                    printf("Write error: %s\n", strerror(errno));
                    free_bl_list(bl_list);
                    return -1;
                }

                off += bl[i].aio_nbytes;
                if (off >= len) {
                    break;
                }
            } else if (bl[i].aio_fildes == wfd) {
                if (EINPROGRESS == aio_error(bl + i))
                    continue;
                ssize_t bcnt = aio_return(bl + i);
                if (bcnt < 0) {
                    printf("Write completion error: %s\n", strerror(errno));
                    free_bl_list(bl_list);
                    return -1;
                }
                total += bcnt;

                bl[i].aio_fildes = rfd;
                ssize_t res = read(bl[i].aio_fildes, (void*)bl[i].aio_buf, bl[i].aio_nbytes);
                if (res < 0) {
                    printf("Read error: %s\n", strerror(errno));
                    free_bl_list(bl_list);
                    return -1;
                }
                ++nread;
            }
        }

        int err = 0;
        if (nread == 0) {
            // no empty write slots, so wait
            err = aio_suspend(bl_list, nbl, NULL);
        }
        if (err < 0) {
            printf("Suspend failed (%d): %s\n", errno, strerror(errno));
            free_bl_list(bl_list);
            return -1;
        }
    }

    free_bl_list(bl_list);
    bl_list = NULL;

    for (int i = 0; i < nbl; ++i) {
        if (bl[i].aio_fildes != wfd) {
            continue;
        }
        // Busy waiting since it's probably good enough.
        while (EINPROGRESS == aio_error(bl + i))
            ;

        ssize_t bcnt = aio_return(bl + i);
        if (bcnt < 0) {
            printf("Final write completion error: %s\n", strerror(errno));
            return -1;
        }
        total += bcnt;
    }

    int err = fsync(wfd);
    if (err != 0) {
        printf("Failed to flush writes, error %d: %s\n", err, strerror(err));
        errno = err;
        return -1;
    }

    return total;
}
