#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include <mpi.h>

#include "file_info_hash.h"
#include "persistent_db.h"

#include "common.h"
#define MAX_TARGETS 10
#define TARGET_BUFFER_SIZE (10*1024*1024)
#define TARGET_SEND_THRESHOLD (1*1024*1024)

extern int process_task(int my_st, const char *path, const FileInfo *fi);

static const int global_coordinator = 0;
static int mpi_rank;
static int mpi_world_size;

int st2rank[MAX_STORAGE_TARGETS];
int rank2st[MAX_STORAGE_TARGETS*2+1];

static
void send_sync_message_to(int recieving_rank, int msg_size, const uint8_t msg[static msg_size])
{
    MPI_Ssend((void*)msg, msg_size, MPI_BYTE, recieving_rank, 0, MPI_COMM_WORLD);
}

static int uint64_cmp(const void *a, const void *b)
{
    uint64_t va = *(uint64_t *)a;
    uint64_t vb = *(uint64_t *)b;
    if (va < vb)
        return -1;
    if (va > vb)
        return 1;
    return 0;
}

static
int sts_in_use(uint64_t locations)
{
    /* gcc specific function to count number of ones */
    return __builtin_popcountll(locations & L_MASK);
}

static
unsigned simple_hash(const char *p, int len)
{
    unsigned h = 5381;
    for (int i = 0; i < len; i++)
        h = h + (h << 5) + p[i];
    return h;
}

static
int eater_rank_from_st(int storage_target)
{
    return st2rank[storage_target];
}
static
int st_from_feeder_rank(int feeder)
{
    assert((feeder > 0) && (feeder % 2 == 0));
    return rank2st[feeder];
}

/*
 * Combine the two `locations` fields and max_chunk_size.
 * The P value is only copied over if it isn't present in `dst->locations`
 */
static void fill_in_missing_fields(FileInfo *dst, const FileInfo *src)
{
    dst->max_chunk_size = MAX(dst->max_chunk_size, src->max_chunk_size);
    uint64_t old_P = GET_P(src->locations);
    dst->locations = (dst->locations | src->locations) & L_MASK;
    if (TEST_BIT(dst->locations, old_P) == 0)
        dst->locations = WITH_P(dst->locations, old_P);
}

/*
 * Selecting the P rank is done by hashing the path once and then iteratively
 * hashing the hash result until we can map it to a rank that is not already
 * mentioned in the list of locations.
 * Could be done smarter -- for one there is no guarantee this terminates.
 */
static
void select_P(const char *path, FileInfo *fi, unsigned ntargets)
{
    if (sts_in_use(fi->locations) == (int)ntargets)
        return;
    unsigned H = simple_hash(path, strlen(path));
choose_P_again:
    H = H ^ simple_hash((const char *)&H, sizeof(H));
    uint64_t P = H % ntargets;
    if (TEST_BIT(fi->locations, P))
        goto choose_P_again;
    fi->locations = WITH_P(fi->locations, P);
}

typedef struct {
    uint64_t byte_size;
    uint64_t timestamp;
    uint64_t path_len;
    char path[];
} packed_file_info;

static ssize_t  dst_written[MAX_TARGETS] = {0};
static ssize_t  dst_in_transit[MAX_TARGETS] = {0};
static MPI_Request async_send_req[MAX_TARGETS] = {0};
static uint8_t dst_buffer[MAX_TARGETS][TARGET_BUFFER_SIZE];

static FileInfoHash *file_info_hash;
#define MAX_WORKITEMS (1000*1000)
static char flat_file_names[MAX_WORKITEMS*100];
static size_t name_bytes_written;
static FileInfo worklist_info[MAX_WORKITEMS];
static char worklist_keys[sizeof(flat_file_names)];

static
int is_done_with_prev_async_send(int target)
{
    if (dst_in_transit[target] == 0)
        return 0;
    MPI_Status stat;
    int flag;
    MPI_Test(&async_send_req[target], &flag, &stat);
    return flag;
}

static
void finish_prev_async_send(int target)
{
    MPI_Status stat;
    MPI_Wait(&async_send_req[target], &stat);

    ssize_t sent = dst_in_transit[target];
    ssize_t written = dst_written[target];
    uint8_t *buf = dst_buffer[target];
    if (sent < written)
        memmove(buf, buf + sent, written - sent);
    dst_written[target] -= sent;
    dst_in_transit[target] = 0;
}

static
void begin_async_send(int target)
{
    assert(dst_in_transit[target] == 0);
    assert(dst_written[target] > 0);
    dst_in_transit[target] = dst_written[target];
    MPI_Isend(
            dst_buffer[target],
            dst_in_transit[target],
            MPI_BYTE,
            eater_rank_from_st(target),
            0,
            MPI_COMM_WORLD,
            &async_send_req[target]);
}

static
void push_to_target(int target, const char *path, int path_len, uint64_t byte_size, uint64_t timestamp)
{
    assert(0 <= target && target < MAX_TARGETS);
    assert(path != NULL);
    assert(path_len > 0);

    ssize_t in_transit = dst_in_transit[target];
    ssize_t written = dst_written[target];
    assert(in_transit <= written);
    assert(written <= TARGET_BUFFER_SIZE);
    ssize_t new_size = written + sizeof(packed_file_info) + path_len;
    if (new_size >= TARGET_BUFFER_SIZE
            || is_done_with_prev_async_send(target)) {
        new_size -= in_transit;
        written -= in_transit;
        in_transit = 0;
        finish_prev_async_send(target);
    }

    packed_file_info finfo = {byte_size, timestamp, path_len};
    uint8_t *dst = dst_buffer[target] + written;
    memcpy(dst, &finfo, sizeof(packed_file_info));
    memcpy(dst + sizeof(packed_file_info), path, path_len);
    dst_written[target] = written = new_size;

    if (in_transit == 0 && written >= TARGET_SEND_THRESHOLD) {
        begin_async_send(target);
    }
}

static
void send_remaining_data_to_targets(void)
{
    for (int i = 0; i < MAX_TARGETS; i++)
    {
        if (dst_in_transit[i] > 0)
            finish_prev_async_send(i);
        if (dst_written[i] > 0)
            begin_async_send(i);
    }
    for (int i = 0; i < MAX_TARGETS; i++)
    {
        if (dst_in_transit[i] > 0)
            finish_prev_async_send(i);
    }
}

static
void feed_targets_with(FILE *input_file, unsigned ntargets)
{
    char buf[64*1024];
    ssize_t buf_size = sizeof(buf);
    ssize_t buf_offset = 0;
    int read;
    int counter = 0;
    while ((read = fread(buf + buf_offset, 1, buf_size - buf_offset, input_file)) > 0)
    {
        size_t buf_alive = buf_offset + read;
        const char *bufp = buf;
        while (buf_alive >= 3*sizeof(uint64_t)) {
            uint64_t timestamp_secs = ((uint64_t *)bufp)[0];
            uint64_t byte_size = ((uint64_t *)bufp)[1];
            uint64_t len_of_path = ((uint64_t *)bufp)[2];
            if (3*sizeof(uint64_t) + len_of_path > buf_alive) {
                buf_offset = buf_alive;
                memmove(buf, bufp, buf_alive);
                break;
            }
            const char *path = bufp + 3*sizeof(uint64_t);
            unsigned st = (simple_hash(path, len_of_path)) % ntargets;
            push_to_target(
                    st,
                    path,
                    len_of_path,
                    byte_size,
                    timestamp_secs);
            counter += 1;
            bufp += len_of_path + 3*sizeof(uint64_t) + 1;
            buf_alive -= len_of_path + 3*sizeof(uint64_t) + 1;
        }
    }
    send_remaining_data_to_targets();
    /* tell global-coordinator that we are done */
    uint8_t failed = 0;
    send_sync_message_to(global_coordinator, sizeof(failed), &failed);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fputs("We need 4 arguments\n", stdout);
        return 1;
    }

    const char *operation = argv[1];
    const char *store_dir = argv[2];
    const char *timestamp_a = argv[3];
    const char *timestamp_b = argv[4];

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_world_size);

    int ntargets = (mpi_world_size - 1)/2;

    if (ntargets > MAX_TARGETS) {
        return 1;
    }

    /* Create mapping from storage targets to ranks, and vice versa */
    uint64_t targetIDs[2*MAX_TARGETS] = {0};
    uint64_t targetID = 0;
    if (mpi_rank != 0)
    {
        int store_fd = open(store_dir, O_DIRECTORY | O_RDONLY);
        int target_ID_fd = openat(store_fd, "targetID", O_RDONLY);
        char targetID_s[8] = {0};
        read(target_ID_fd, targetID_s, sizeof(targetID_s));
        close(target_ID_fd);
        close(store_fd);
        targetID = (atoll(targetID_s) << 32) | mpi_rank;
    }
    MPI_Gather(
            &targetID, 8, MPI_BYTE,
            targetIDs, 8, MPI_BYTE,
            0,
            MPI_COMM_WORLD);
    if (mpi_rank == 0) {
        for (int i = 0; i < 2*ntargets; i+=2)
            assert((targetIDs[i] >> 32) == (targetIDs[i+1] >> 32));
        for (int i = 0; i < ntargets; i++)
            targetIDs[i] = targetIDs[2*i+1];
        qsort(targetIDs, ntargets, sizeof(uint64_t), uint64_cmp);
        rank2st[0] = -1;
        for (int i = 0; i < ntargets; i++)
        {
            st2rank[i] = (int)(targetIDs[i] & UINT64_C(0xFFFFFFFF));
            rank2st[st2rank[i]] = i;
            rank2st[st2rank[i]+1] = i;
        }
    }
    MPI_Bcast(st2rank, sizeof(st2rank), MPI_BYTE, 0, MPI_COMM_WORLD);
    MPI_Bcast(rank2st, sizeof(rank2st), MPI_BYTE, 0, MPI_COMM_WORLD);

    int eater_ranks[MAX_TARGETS];
    int feeder_ranks[MAX_TARGETS];
    for (int i = 0; i < ntargets; i++) {
        eater_ranks[i] = 1 + 2*i;
        feeder_ranks[i] = 2 + 2*i;
    }

    /* 
     * When the feeders are done they have nothing else to do.
     * Broadcasts would still transfer data to them, so we create a group
     * without the feeders.
     *
     * Ideally we could have made the feeders/eaters as two different threads,
     * simply closing the feeder threads when done. But that doesn't work with
     * the MPI version I am testing on - it causes data races and maybe some
     * deadlocks.
     * */
    MPI_Group everyone, not_everyone;
    MPI_Comm_group(MPI_COMM_WORLD, &everyone);
    MPI_Group_excl(everyone, ntargets, feeder_ranks, &not_everyone);
    MPI_Comm comm;
    MPI_Comm_create(MPI_COMM_WORLD, not_everyone, &comm);

    /*
     * In phase 1 we have 3 kinds of processes:
     *  - global coordinator
     *  - feeders
     *  - eaters
     *
     * An eater simply receives data from anyone (storing it for later) - only
     * stopping when the global coordinator sends them a message.
     *
     * The feeders run through their files/chunks, selects a "random" eater and
     * sends filename, size, etc to it. Once they are done with their files
     * they message the global coordinator.
     *
     * Finally the global coordinator waits until every feeder has told it that
     * they are done processing - then it tells the eaters.
     */
    int p1_eater = (mpi_rank % 2 == 1);
    int p1_feeder = (mpi_rank > 0 && mpi_rank % 2 == 0);
    if (mpi_rank == global_coordinator)
    {
        int still_in_stage_1 = ntargets;
        int failed = 0;
        while (still_in_stage_1 > 0) {
            MPI_Status stat;
            int8_t error;
            MPI_Recv(&error, sizeof(error), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &stat);
            still_in_stage_1 -= 1;
            failed += error;
            printf("gco: got message from %d, with error = %d\n", stat.MPI_SOURCE, error);
        }
        /* Inform all eaters that there is no more food */
        uint8_t dummy = 1;
        for (int i = 1; i < 1 + 2*ntargets; i+=2)
            send_sync_message_to(i, 1, &dummy);
    }
    else if (p1_feeder)
    {
        FILE *slave;
        char cmd_buf[512];
        if (strcmp(operation, "complete") == 0)
            snprintf(cmd_buf, sizeof(cmd_buf), "bp-find-all-chunks %s/chunks", store_dir);
        else if (strcmp(operation, "partial") == 0)
            snprintf(cmd_buf, sizeof(cmd_buf), "audit-find-between %s %s %s/chunks", timestamp_a, timestamp_b, store_dir);
        else
            strcpy(cmd_buf, "cat /dev/null");
        slave = popen(cmd_buf, "r");
        feed_targets_with(slave, ntargets);
        pclose(slave);
    }
    else if (p1_eater)
    {
        file_info_hash = fih_init();
        uint8_t recv_buffer[TARGET_BUFFER_SIZE] = {0};
        for (;;) {
            MPI_Status stat;
            MPI_Recv(recv_buffer, TARGET_BUFFER_SIZE, MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &stat);
            if (stat.MPI_SOURCE == global_coordinator)
                break;
            /* parse and add data */
            int actually_received = 0;
            MPI_Get_count(&stat, MPI_BYTE, &actually_received);
            int src = stat.MPI_SOURCE;
            int i = 0;
            while (i < actually_received) {
                packed_file_info *pfi = (packed_file_info *)(recv_buffer+i);
                i += sizeof(packed_file_info) + pfi->path_len;
                if (name_bytes_written + pfi->path_len + 1 >= sizeof(flat_file_names))
                    continue;
                /*printf("%d - received '%.*s' from %d\n", mpi_rank, (uint32_t)(pfi->path_len), pfi->path, src);*/
                char *n = flat_file_names + name_bytes_written;
                memmove(n, pfi->path, pfi->path_len);
                n[pfi->path_len] = '\0';
                name_bytes_written += pfi->path_len + 1;
                if (fih_add_info(file_info_hash,
                        n,
                        st_from_feeder_rank(src),
                        pfi->byte_size,
                        pfi->timestamp))
                    name_bytes_written -= pfi->path_len + 1;
            }
            printf("%d - received %d bytes from %d\n", mpi_rank, actually_received, stat.MPI_SOURCE);
        }
    }

    if (p1_feeder) {
        MPI_Finalize();
        return 0;
    }

    PersistentDB *pdb = pdb_init();

    int mpi_bcast_rank;
    MPI_Comm_rank(comm, &mpi_bcast_rank);
    int mpi_bcast_size;
    MPI_Comm_size(comm, &mpi_bcast_size);
    int my_st = rank2st[mpi_rank];

    /*
     * We have now sent info on all out chunks, and received info on all chunks
     * that we are responsible for.
     * */
    for (int i = 1; i < mpi_bcast_size; i++)
    {
        size_t nitems = 0;
        size_t path_bytes = 0;
        if (mpi_bcast_rank == i)
        {
            /*
             * Collect all file info entries in to a packed array that is ready for
             * broadcasting.
             * */
            const char *s = flat_file_names;
            while (s < flat_file_names + name_bytes_written && *s != '\0')
            {
                size_t s_len = strlen(s);
                FileInfo prev_fi;
                int has_an_old_version = pdb_get(pdb, s, s_len, &prev_fi);
                FileInfo *fi = worklist_info + nitems;
                fih_get(file_info_hash, s, fi);
                if (has_an_old_version)
                    fill_in_missing_fields(fi, &prev_fi);
                if (GET_P(fi->locations) == NO_P)
                    select_P(s, fi, (unsigned)ntargets);
                pdb_set(pdb, s, s_len, fi);
                s += s_len + 1;
                nitems += 1;
            }
            printf("%d - nitems = %zu\n", mpi_rank, nitems);
            path_bytes = name_bytes_written;
            memcpy(worklist_keys, flat_file_names, path_bytes);
            fih_term(file_info_hash);
            file_info_hash = NULL;
        }
        MPI_Bcast(&nitems, sizeof(nitems), MPI_BYTE, i, comm);
        MPI_Bcast(worklist_info, sizeof(FileInfo)*nitems, MPI_BYTE, i, comm);
        MPI_Bcast(&path_bytes, sizeof(path_bytes), MPI_BYTE, i, comm);
        MPI_Bcast(worklist_keys, path_bytes, MPI_BYTE, i, comm);

        size_t j = 0;
        const char *s = worklist_keys;
        while (j < nitems)
        {
            size_t s_len = strlen(s);
            if (process_task(my_st, s, worklist_info + j))
                pdb_set(pdb, s, s_len, worklist_info + j);
            s += s_len + 1;
            j += 1;
        }
    }

    pdb_term(pdb);
    pdb = NULL;

    MPI_Finalize();
    return 0;
}
