#include <assert.h>
#include <stdint.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <mpi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define FILE_TRANSFER_BUFFER_SIZE (10*1024*1024)

/* Replicates a mkdir -p/--parents command for the dir the filename is in */
static void mkdir_for_file(const char *filename) {
    char tmp[256];
    strncpy(tmp, filename, sizeof(tmp));
    for(char *p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
}

static
void send_sync_message_to(int16_t recieving_rank, int msg_size, const uint8_t msg[static msg_size])
{
    MPI_Send((void*)msg, msg_size, MPI_BYTE, recieving_rank, 0, MPI_COMM_WORLD);
}

static
int open_fileid_readonly(const char *id)
{
    int fd = open(id, O_RDONLY);
    if (fd < 0)
        return -errno;
    return fd;
}

static
int open_fileid_new_parity(const char *id)
{
    char tmp[256];
    const char A[] = "/store01/chunks/";
    const char B[] = "/store02/chunks/";
    int Alen = sizeof(A)-1;
    int Blen = sizeof(B)-1;
    if (strncmp(A, id, Alen) != 0 && strncmp(B, id, Blen) != 0) {
        fputs("ERROR: All input files must start with /store0_/chunks/!"
                " Writing to /dev/null.\n", stderr);
        return open("/dev/null", O_WRONLY);
    }
    snprintf(tmp, sizeof(tmp),
            "/store0%c/parity/%s",
            id[strlen("/store0")],
            id+Alen);
    mkdir_for_file(tmp);
    int fd = creat(tmp, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return -errno;
    return fd;
}

#define P_rank(fi) ((fi)->locations[P_INDEX])

static
int active_ranks(const int16_t *ranks, int n)
{
    int i = 0;
    for (; i < n && ranks[i] != 0; i++)
        ;
    return i;
}

static
uint64_t div_round_up(uint64_t a, uint64_t b)
{
    return (a + (b - 1)) / b;
}

static
void xor_parity(uint8_t *restrict dst, size_t nbytes, const uint8_t *data, int nsources)
{
    for (int j = 0; j < nsources; j++)
    {
        const uint8_t *src = data + j*nbytes; 
        size_t i = 0;
        for (; i + 8 < nbytes; i += 8)
            *(uint64_t *)(dst + i) ^= *(uint64_t *)(src + i);
        for (; i < nbytes; i++)
            dst[i] ^= src[i];
    }
}

/*
 * Roles:
 *  chunk_sender - open file and start sending parts to P-rank
 *  parity_generator:
 *      receives data from chunk sources, calculate and store parity
 */
static
void parity_generator(const char *path, const FileInfo *task)
{
#define IRECV_ALL(ii, loc, size) do { \
    for (int ii = 0; ii < active_source_ranks; ii++) \
        MPI_Irecv((loc), (size), MPI_BYTE, task->locations[ii], \
                0, MPI_COMM_WORLD, &source_messages[ii]); \
    } while(0)

    const int active_source_ranks = active_ranks(task->locations, MAX_LOCS);
    size_t buffer_size = MIN(FILE_TRANSFER_BUFFER_SIZE, task->max_chunk_size);
    uint8_t *data = calloc(active_source_ranks, FILE_TRANSFER_BUFFER_SIZE);
    uint8_t *P_block = calloc(1, FILE_TRANSFER_BUFFER_SIZE);
    uint64_t chunk_sizes[active_source_ranks];
    int P_fd = open_fileid_new_parity(path);
    int P_local_write_error = (P_fd < 0);
    int expected_messages = div_round_up(task->max_chunk_size, FILE_TRANSFER_BUFFER_SIZE);
    MPI_Request source_messages[active_source_ranks];
    MPI_Status source_stat[active_source_ranks];
    IRECV_ALL(src, chunk_sizes + src, sizeof(uint64_t));
    MPI_Waitall(active_source_ranks, source_messages, source_stat);
    uint64_t full_size = 0;
    for (int i = 0; i < active_source_ranks; i++)
        full_size += chunk_sizes[i];
    P_local_write_error = (write(P_fd, &full_size, sizeof(full_size)) <= 0);
    for (int msg_i = 0; msg_i < expected_messages; msg_i++)
    {
        /* begin async receive from all sources and wait for all receives to
         * finish. Ideally we would do calculations and local I/O on previously
         * received data before waiting -- but this is only a first draft. */
        IRECV_ALL(src, data + src*buffer_size, buffer_size);
        MPI_Waitall(active_source_ranks, source_messages, source_stat);
        /* calculate P and write to disk */
        xor_parity(P_block, buffer_size, data, active_source_ranks);
        if (!P_local_write_error) {
            ssize_t w = write(P_fd, P_block, buffer_size);
            P_local_write_error |= (w <= 0);
        }
    }
    free(P_block);
    free(data);
    close(P_fd);
#undef IRECV_ALL
}

static
void chunk_sender(const char *path, const FileInfo *task)
{
    int16_t coordinator = P_rank(task);
    uint64_t data_in_fd = task->max_chunk_size;
    size_t buffer_size = MIN(FILE_TRANSFER_BUFFER_SIZE, task->max_chunk_size);
    uint8_t *data = calloc(1, FILE_TRANSFER_BUFFER_SIZE);
    int have_had_error = 0;
    int fd = open_fileid_readonly(path);
    uint64_t fd_size = 0;
    if (fd < 0)
        have_had_error = 1;
    else {
        struct stat st;
        fstat(fd, &st);
        fd_size = st.st_size;
    }
    send_sync_message_to(coordinator, sizeof(fd_size), (uint8_t *)&fd_size);
    uint64_t read_from_fd = 0;
    while (read_from_fd < data_in_fd)
    {
        if (!have_had_error) {
            ssize_t r = read(fd, data, buffer_size);
            have_had_error |= (r <= 0);
        }
        read_from_fd += buffer_size;
        send_sync_message_to(coordinator, buffer_size, data);
    }
    free(data);
    close(fd);
}

/* Returns non-zero if we are involved in the task */
int process_task(int16_t my_rank, const char *path, const FileInfo *fi)
{
    int found_in_srcs = 0;
    for (int j = 0; j < MAX_LOCS; j++)
        found_in_srcs |= (fi->locations[j] == my_rank);
    if (P_rank(fi) == my_rank)
        parity_generator(path, fi);
    else if (found_in_srcs && my_rank > 0)
        chunk_sender(path, fi);
    else
        return 0;
    return 1;
}
