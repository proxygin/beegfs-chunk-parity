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

#define FILE_TRANSFER_BUFFER_SIZE (1*1024*1024)


static
void send_sync_message_to(int16_t recieving_rank, int msg_size, const uint8_t msg[static msg_size])
{
    MPI_Send((void*)msg, msg_size, MPI_BYTE, recieving_rank, 0, MPI_COMM_WORLD);
}

#define MAX_CHUNKS_PER_FILE 8
typedef const char* FileID;

static
int open_fileid_readonly(FileID id)
{
    (void) id;
    int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0)
        return -errno;
    return fd;
}

static
int open_fileid_new_parity(FileID id)
{
    (void) id;
    int fd = open("/dev/null", O_WRONLY);
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

/*
 * Roles:
 *  parity_receiver - simply receives parts of Q from the P-rank
 *  chunk_sender - open file_id and starts sending parts to P-rank
 *  parity_generator:
 *      receives chunks sources, does parity and sends parts of Q to Q-rank
 */
void parity_generator(const char *path, const FileInfo *task)
{
    const int active_source_ranks = active_ranks(task->locations, MAX_LOCS);
    uint8_t *data = calloc(active_source_ranks, FILE_TRANSFER_BUFFER_SIZE);
    int P_fd = open_fileid_new_parity(path);
    int P_local_write_error = (P_fd < 0);
    int expected_messages = (task->max_chunk_size == 0)? 0 : (1 + task->max_chunk_size / FILE_TRANSFER_BUFFER_SIZE);
    MPI_Request source_messages[active_source_ranks];
    MPI_Status source_stat[active_source_ranks];
    for (int msg_i = 0; msg_i < expected_messages; msg_i++)
    {
        /* begin async receive from all sources and wait for all receives to
         * finish. Ideally we would do calculations and local I/O on previously
         * received data before waiting -- but this is only a first draft. */
        for (int i = 0; i < active_source_ranks; i++) {
            MPI_Irecv(
                    data + i*FILE_TRANSFER_BUFFER_SIZE,
                    FILE_TRANSFER_BUFFER_SIZE,
                    MPI_BYTE,
                    task->locations[i],
                    0,
                    MPI_COMM_WORLD,
                    &source_messages[i]);
        }
        MPI_Waitall(active_source_ranks, source_messages, source_stat);
        /* calculate P/Q and write P to disk */
        if (!P_local_write_error) {
            ssize_t w = write(P_fd, data, FILE_TRANSFER_BUFFER_SIZE);
            P_local_write_error |= (w <= 0);
        }
    }
    free(data);
    close(P_fd);
}

void chunk_sender(const char *path, const FileInfo *task)
{
    int16_t coordinator = P_rank(task);
    uint64_t data_in_fd = task->max_chunk_size;
    uint8_t *data = calloc(1, FILE_TRANSFER_BUFFER_SIZE);
    int have_had_error = 0;
    int fd = open_fileid_readonly(path);
    if (fd < 0) {
        have_had_error = 1;
    }
    uint64_t read_from_fd = 0;
    while (read_from_fd < data_in_fd)
    {
        if (!have_had_error) {
            ssize_t r = read(fd, data, FILE_TRANSFER_BUFFER_SIZE);
            have_had_error |= (r <= 0);
        }
        read_from_fd += FILE_TRANSFER_BUFFER_SIZE;
        send_sync_message_to(coordinator, FILE_TRANSFER_BUFFER_SIZE, data);
    }
    free(data);
    close(fd);
}

void process_task(int16_t my_rank, const char *path, const FileInfo *fi)
{
    int found_in_srcs = 0;
    for (int j = 0; j < MAX_LOCS; j++)
        found_in_srcs |= (fi->locations[j] == my_rank);
    if (P_rank(fi) == my_rank)
        parity_generator(path, fi);
    else if (found_in_srcs && my_rank > 0)
        chunk_sender(path, fi);
}
