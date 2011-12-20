/*
 * migration-postcopy.c: postcopy livemigration
 *
 * Copyright (c) 2011
 * National Institute of Advanced Industrial Science and Technology
 *
 * https://sites.google.com/site/grivonhome/quick-kvm-migration
 * Author: Isaku Yamahata <yamahata at valinux co jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config-host.h"

#if defined(CONFIG_MADVISE) || defined(CONFIG_POSIX_MADVISE)
#include <sys/mman.h>
#endif

#include "bitmap.h"
#include "sysemu.h"
#include "kvm.h"
#include "hw/hw.h"
#include "arch_init.h"
#include "migration.h"
#include "buffered_file.h"
#include "qemu_socket.h"
#include "qemu-thread.h"
#include "umem.h"

#include "memory.h"
#include "cpu-common.h"

//#define DEBUG_POSTCOPY
#ifdef DEBUG_POSTCOPY
#include <sys/syscall.h>
#define DPRINTF(fmt, ...)                                               \
    do {                                                                \
        printf("%d:%ld %s:%d: " fmt, getpid(), syscall(SYS_gettid),     \
               __func__, __LINE__, ## __VA_ARGS__);                     \
    } while (0)
#else
#define DPRINTF(fmt, ...)       do { } while (0)
#endif

static void fd_close(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void set_fd(int fd, fd_set *fds, int *nfds)
{
    FD_SET(fd, fds);
    if (fd > *nfds) {
        *nfds = fd;
    }
}

/***************************************************************************
 * umem daemon on destination <-> qemu on source protocol
 */

#define QEMU_UMEM_REQ_INIT      0x00
#define QEMU_UMEM_REQ_EOC       0x01
#define QEMU_UMEM_REQ_PAGE      0x02
#define QEMU_UMEM_REQ_PAGE_CONT 0x03

struct qemu_umem_req {
    int8_t cmd;
    uint8_t len;
    char *idstr;        /* REQ_PAGE */
    uint32_t nr;        /* REQ_PAGE, REQ_PAGE_CONT */

    /* in target page size as qemu migration protocol */
    uint64_t *pgoffs;   /* REQ_PAGE, REQ_PAGE_CONT */
};

static void postcopy_incoming_send_req_idstr(QEMUFile *f, const char* idstr)
{
    qemu_put_byte(f, strlen(idstr));
    qemu_put_buffer(f, (uint8_t *)idstr, strlen(idstr));
}

static void postcopy_incoming_send_req_pgoffs(QEMUFile *f, uint32_t nr,
                                              const uint64_t *pgoffs)
{
    uint32_t i;

    qemu_put_be32(f, nr);
    for (i = 0; i < nr; i++) {
        qemu_put_be64(f, pgoffs[i]);
    }
}

static void postcopy_incoming_send_req_one(QEMUFile *f,
                                           const struct qemu_umem_req *req)
{
    DPRINTF("cmd %d\n", req->cmd);
    qemu_put_byte(f, req->cmd);
    switch (req->cmd) {
    case QEMU_UMEM_REQ_INIT:
    case QEMU_UMEM_REQ_EOC:
        /* nothing */
        break;
    case QEMU_UMEM_REQ_PAGE:
        postcopy_incoming_send_req_idstr(f, req->idstr);
        postcopy_incoming_send_req_pgoffs(f, req->nr, req->pgoffs);
        break;
    case QEMU_UMEM_REQ_PAGE_CONT:
        postcopy_incoming_send_req_pgoffs(f, req->nr, req->pgoffs);
        break;
    default:
        abort();
        break;
    }
}

/* QEMUFile can buffer up to IO_BUF_SIZE = 32 * 1024.
 * So one message size must be <= IO_BUF_SIZE
 * cmd: 1
 * id len: 1
 * id: 256
 * nr: 2
 */
#define MAX_PAGE_NR     ((32 * 1024 - 1 - 1 - 256 - 2) / sizeof(uint64_t))
static void postcopy_incoming_send_req(QEMUFile *f,
                                       const struct qemu_umem_req *req)
{
    uint32_t nr = req->nr;
    struct qemu_umem_req tmp = *req;

    switch (req->cmd) {
    case QEMU_UMEM_REQ_INIT:
    case QEMU_UMEM_REQ_EOC:
        postcopy_incoming_send_req_one(f, &tmp);
        break;
    case QEMU_UMEM_REQ_PAGE:
        tmp.nr = MIN(nr, MAX_PAGE_NR);
        postcopy_incoming_send_req_one(f, &tmp);

        nr -= tmp.nr;
        tmp.pgoffs += tmp.nr;
        tmp.cmd = QEMU_UMEM_REQ_PAGE_CONT;
        /* fall through */
    case QEMU_UMEM_REQ_PAGE_CONT:
        while (nr > 0) {
            tmp.nr = MIN(nr, MAX_PAGE_NR);
            postcopy_incoming_send_req_one(f, &tmp);

            nr -= tmp.nr;
            tmp.pgoffs += tmp.nr;
        }
        break;
    default:
        abort();
        break;
    }
}

static int postcopy_outgoing_recv_req_idstr(QEMUFile *f,
                                            struct qemu_umem_req *req,
                                            size_t *offset)
{
    int ret;

    req->len = qemu_peek_byte(f, *offset);
    *offset += 1;
    if (req->len == 0) {
        return -EAGAIN;
    }
    req->idstr = g_malloc((int)req->len + 1);
    ret = qemu_peek_buffer(f, (uint8_t*)req->idstr, req->len, *offset);
    *offset += ret;
    if (ret != req->len) {
        g_free(req->idstr);
        req->idstr = NULL;
        return -EAGAIN;
    }
    req->idstr[req->len] = 0;
    return 0;
}

static int postcopy_outgoing_recv_req_pgoffs(QEMUFile *f,
                                             struct qemu_umem_req *req,
                                             size_t *offset)
{
    int ret;
    uint32_t be32;
    uint32_t i;

    ret = qemu_peek_buffer(f, (uint8_t*)&be32, sizeof(be32), *offset);
    *offset += sizeof(be32);
    if (ret != sizeof(be32)) {
        return -EAGAIN;
    }

    req->nr = be32_to_cpu(be32);
    req->pgoffs = g_new(uint64_t, req->nr);
    for (i = 0; i < req->nr; i++) {
        uint64_t be64;
        ret = qemu_peek_buffer(f, (uint8_t*)&be64, sizeof(be64), *offset);
        *offset += sizeof(be64);
        if (ret != sizeof(be64)) {
            g_free(req->pgoffs);
            req->pgoffs = NULL;
            return -EAGAIN;
        }
        req->pgoffs[i] = be64_to_cpu(be64);
    }
    return 0;
}

static int postcopy_outgoing_recv_req(QEMUFile *f, struct qemu_umem_req *req)
{
    int size;
    int ret;
    size_t offset = 0;

    size = qemu_peek_buffer(f, (uint8_t*)&req->cmd, 1, offset);
    if (size <= 0) {
        return -EAGAIN;
    }
    offset += 1;

    switch (req->cmd) {
    case QEMU_UMEM_REQ_INIT:
    case QEMU_UMEM_REQ_EOC:
        /* nothing */
        break;
    case QEMU_UMEM_REQ_PAGE:
        ret = postcopy_outgoing_recv_req_idstr(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        ret = postcopy_outgoing_recv_req_pgoffs(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        break;
    case QEMU_UMEM_REQ_PAGE_CONT:
        ret = postcopy_outgoing_recv_req_pgoffs(f, req, &offset);
        if (ret < 0) {
            return ret;
        }
        break;
    default:
        abort();
        break;
    }
    qemu_file_skip(f, offset);
    DPRINTF("cmd %d\n", req->cmd);
    return 0;
}

static void postcopy_outgoing_free_req(struct qemu_umem_req *req)
{
    g_free(req->idstr);
    g_free(req->pgoffs);
}

/***************************************************************************
 * QEMU_VM_POSTCOPY section subtype
 */
#define QEMU_VM_POSTCOPY_INIT           0
#define QEMU_VM_POSTCOPY_SECTION_FULL   1

/* options in QEMU_VM_POSTCOPY_INIT section */
#define POSTCOPY_OPTION_PRECOPY         1ULL

/***************************************************************************
 * outgoing part
 */

enum POState {
    PO_STATE_ERROR_RECEIVE,
    PO_STATE_ACTIVE,
    PO_STATE_EOC_RECEIVED,
    PO_STATE_ALL_PAGES_SENT,
    PO_STATE_COMPLETED,
};
typedef enum POState POState;

struct PostcopyOutgoingState {
    POState state;
    QEMUFile *mig_read;
    int fd_read;
    RAMBlock *last_block_read;

    QEMUFile *mig_buffered_write;
    MigrationState *ms;
};
typedef struct PostcopyOutgoingState PostcopyOutgoingState;

int postcopy_outgoing_create_read_socket(MigrationState *s)
{
    if (!s->params.postcopy) {
        return 0;
    }

    s->fd_read = dup(s->fd);
    if (s->fd_read == -1) {
        int ret = -errno;
        perror("dup");
        return ret;
    }
    s->file_read = qemu_fopen_socket(s->fd_read);
    if (s->file_read == NULL) {
        return -EINVAL;
    }
    return 0;
}

void postcopy_outgoing_state_begin(QEMUFile *f, const MigrationParams *params)
{
    uint64_t options = 0;
    if (params->precopy_count > 0) {
        options |= POSTCOPY_OPTION_PRECOPY;
    }

    qemu_put_ubyte(f, QEMU_VM_POSTCOPY_INIT);
    qemu_put_be32(f, sizeof(options));
    qemu_put_be64(f, options);
}

void postcopy_outgoing_state_complete(
    QEMUFile *f, const uint8_t *buffer, size_t buffer_size)
{
    qemu_put_ubyte(f, QEMU_VM_POSTCOPY_SECTION_FULL);
    qemu_put_be32(f, buffer_size);
    qemu_put_buffer(f, buffer, buffer_size);
}

int postcopy_outgoing_ram_save_iterate(QEMUFile *f, void *opaque)
{
    int ret;
    MigrationState *s = migrate_get_current();
    if (s->params.precopy_count == 0) {
        qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
        return 1;
    }

    ret = ram_save_iterate(f);
    if (ret < 0) {
        return ret;
    }
    if (ret == 1) {
        DPRINTF("precopy worked\n");
        return ret;
    }
    if (ram_bytes_remaining() == 0) {
        DPRINTF("no more precopy\n");
        return 1;
    }
    return s->precopy_count >= s->params.precopy_count? 1: 0;
}

int postcopy_outgoing_ram_save_complete(QEMUFile *f, void *opaque)
{
    MigrationState *s = migrate_get_current();
    if (s->params.precopy_count > 0) {
        /* Make sure all dirty bits are set */
        migration_bitmap_sync();
        memory_global_dirty_log_stop();
    }
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    return 0;
}

static void postcopy_outgoing_ram_save_page(PostcopyOutgoingState *s,
                                            uint64_t pgoffset, bool *written,
                                            bool forward,
                                            int prefault_pgoffset)
{
    ram_addr_t offset;
    int ret;

    if (forward) {
        pgoffset += prefault_pgoffset;
    } else {
        if (pgoffset < prefault_pgoffset) {
            return;
        }
        pgoffset -= prefault_pgoffset;
    }

    offset = pgoffset << TARGET_PAGE_BITS;
    if (offset >= s->last_block_read->length) {
        assert(forward);
        assert(prefault_pgoffset > 0);
        return;
    }

    ret = ram_save_page(s->mig_buffered_write, s->last_block_read, offset,
                        false);
    if (ret > 0) {
        *written = true;
    }
}

/*
 * return value
 *   0: continue postcopy mode
 * > 0: completed postcopy mode.
 * < 0: error
 */
static int postcopy_outgoing_handle_req(PostcopyOutgoingState *s,
                                        const struct qemu_umem_req *req,
                                        bool *written)
{
    int i;
    uint64_t j;
    RAMBlock *block;

    DPRINTF("cmd %d state %d\n", req->cmd, s->state);
    switch(req->cmd) {
    case QEMU_UMEM_REQ_INIT:
        /* nothing */
        break;
    case QEMU_UMEM_REQ_EOC:
        /* tell to finish migration. */
        if (s->state == PO_STATE_ALL_PAGES_SENT) {
            s->state = PO_STATE_COMPLETED;
            DPRINTF("-> PO_STATE_COMPLETED\n");
        } else {
            s->state = PO_STATE_EOC_RECEIVED;
            DPRINTF("-> PO_STATE_EOC_RECEIVED\n");
        }
        return 1;
    case QEMU_UMEM_REQ_PAGE:
        DPRINTF("idstr: %s\n", req->idstr);
        block = ram_find_block(req->idstr, strlen(req->idstr));
        if (block == NULL) {
            return -EINVAL;
        }
        s->last_block_read = block;
        /* fall through */
    case QEMU_UMEM_REQ_PAGE_CONT:
        DPRINTF("nr %d\n", req->nr);
        if (s->mig_buffered_write == NULL) {
            assert(s->state == PO_STATE_ALL_PAGES_SENT);
            break;
        }
        for (i = 0; i < req->nr; i++) {
            DPRINTF("pgoffs[%d] 0x%"PRIx64"\n", i, req->pgoffs[i]);
            postcopy_outgoing_ram_save_page(s, req->pgoffs[i], written,
                                            true, 0);
        }
        /* forward prefault */
        for (j = 1; j <= s->ms->params.prefault_forward; j++) {
            for (i = 0; i < req->nr; i++) {
                DPRINTF("pgoffs[%d] + 0x%"PRIx64" 0x%"PRIx64"\n",
                        i, j, req->pgoffs[i] + j);
                postcopy_outgoing_ram_save_page(s, req->pgoffs[i], written,
                                                true, j);
            }
        }
        if (s->ms->params.movebg) {
            ram_addr_t last_offset =
                (req->pgoffs[req->nr - 1] + s->ms->params.prefault_forward) <<
                TARGET_PAGE_BITS;
            last_offset = MIN(last_offset,
                              s->last_block_read->length - TARGET_PAGE_SIZE);
            ram_save_set_last_block(s->last_block_read, last_offset);
        }
        /* backward prefault */
        for (j = 1; j <= s->ms->params.prefault_backward; j++) {
            for (i = 0; i < req->nr; i++) {
                DPRINTF("pgoffs[%d] - 0x%"PRIx64" 0x%"PRIx64"\n",
                        i, j, req->pgoffs[i] - j);
                postcopy_outgoing_ram_save_page(s, req->pgoffs[i], written,
                                                false, j);
            }
        }
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void postcopy_outgoing_close_mig_read(PostcopyOutgoingState *s)
{
    if (s->mig_read != NULL) {
        qemu_set_fd_handler(s->fd_read, NULL, NULL, NULL);
        qemu_fclose(s->mig_read);
        s->mig_read = NULL;
        fd_close(&s->fd_read);

        s->ms->file_read = NULL;
        s->ms->fd_read = -1;
    }
}

static void postcopy_outgoing_completed(PostcopyOutgoingState *s)
{
    postcopy_outgoing_close_mig_read(s);
    s->ms->postcopy = NULL;
    g_free(s);
}

static void postcopy_outgoing_recv_handler(void *opaque)
{
    PostcopyOutgoingState *s = opaque;
    bool written = false;
    int ret = 0;

    assert(s->state == PO_STATE_ACTIVE ||
           s->state == PO_STATE_ALL_PAGES_SENT);

    do {
        struct qemu_umem_req req = {.idstr = NULL,
                                    .pgoffs = NULL};

        ret = postcopy_outgoing_recv_req(s->mig_read, &req);
        if (ret < 0) {
            if (ret == -EAGAIN) {
                ret = 0;
            }
            break;
        }

        /* Even when s->state == PO_STATE_ALL_PAGES_SENT,
           some request can be received like QEMU_UMEM_REQ_EOC */
        ret = postcopy_outgoing_handle_req(s, &req, &written);
        postcopy_outgoing_free_req(&req);
    } while (ret == 0);

    /*
     * flush buffered_file.
     * Although mig_write is rate-limited buffered file, those written pages
     * are requested on demand by the destination. So forcibly push
     * those pages ignoring rate limiting
     */
    if (written) {
        qemu_buffered_file_drain(s->mig_buffered_write);
    }

    if (ret < 0) {
        switch (s->state) {
        case PO_STATE_ACTIVE:
            s->state = PO_STATE_ERROR_RECEIVE;
            DPRINTF("-> PO_STATE_ERROR_RECEIVE\n");
            break;
        case PO_STATE_ALL_PAGES_SENT:
            s->state = PO_STATE_COMPLETED;
            DPRINTF("-> PO_STATE_ALL_PAGES_SENT\n");
            break;
        default:
            abort();
        }
    }
    if (s->state == PO_STATE_ERROR_RECEIVE || s->state == PO_STATE_COMPLETED) {
        postcopy_outgoing_close_mig_read(s);
    }
    if (s->state == PO_STATE_COMPLETED) {
        DPRINTF("PO_STATE_COMPLETED\n");
        MigrationState *ms = s->ms;
        postcopy_outgoing_completed(s);
        migrate_fd_completed(ms);
    }
}

void *postcopy_outgoing_begin(MigrationState *ms)
{
    PostcopyOutgoingState *s = g_new(PostcopyOutgoingState, 1);
    const RAMBlock *block;
    DPRINTF("outgoing begin\n");
    qemu_buffered_file_drain(ms->file);

    s->ms = ms;
    s->state = PO_STATE_ACTIVE;
    s->fd_read = ms->fd_read;
    s->mig_read = ms->file_read;
    s->mig_buffered_write = ms->file;

    if (ms->params.precopy_count > 0) {
        QEMUFile *f = ms->file;
        uint64_t last_long =
            BITS_TO_LONGS(last_ram_offset() >> TARGET_PAGE_BITS);

        /* send dirty bitmap */
        qemu_mutex_lock_ramlist();
        QLIST_FOREACH(block, &ram_list.blocks, next) {
            const unsigned long *bitmap = migration_bitmap_get();
            uint64_t length;
            uint64_t start;
            uint64_t end;
            uint64_t i;

            qemu_put_byte(f, strlen(block->idstr));
            qemu_put_buffer(f, (uint8_t *)block->idstr, strlen(block->idstr));
            qemu_put_be64(f, block->offset);
            qemu_put_be64(f, block->length);

            start = (block->offset >> TARGET_PAGE_BITS);
            end = (block->offset + block->length) >> TARGET_PAGE_BITS;

            length = BITS_TO_LONGS(end - (start & ~63)) * sizeof(unsigned long);
            length = DIV_ROUND_UP(length, sizeof(uint64_t)) * sizeof(uint64_t);
            qemu_put_be64(f, length);
            DPRINTF("dirty bitmap %s 0x%"PRIx64" 0x%"PRIx64" 0x%"PRIx64"\n",
                    block->idstr, block->offset, block->length, length);

            start /= BITS_PER_LONG;
            end = DIV_ROUND_UP(end, BITS_PER_LONG);
            assert(end <= last_long);

            for (i = start; i < end;
                 i += sizeof(uint64_t) / sizeof(unsigned long)) {
                uint64_t val;
#if HOST_LONG_BITS == 64
                val = bitmap[i];
#elif HOST_LONG_BITS == 32
                if (i + 1 < last_long) {
                    val = bitmap[i] | ((uint64_t)bitmap[i + 1] << 32);
                } else {
                    val = bitmap[i];
                }
#else
# error "unsupported"
#endif
                qemu_put_be64(f, val);
            }
        }
        qemu_mutex_unlock_ramlist();

        /* terminator */
        qemu_put_byte(f, 0);    /* idstr len */
        qemu_put_be64(f, 0);    /* block offset */
        qemu_put_be64(f, 0);    /* block length */
        qemu_put_be64(f, 0);    /* bitmap len */
        DPRINTF("sent dirty bitmap\n");
    }

    qemu_set_fd_handler(s->fd_read,
                        &postcopy_outgoing_recv_handler, NULL, s);
    postcopy_outgoing_recv_handler(s);
    return s;
}

static void postcopy_outgoing_ram_all_sent(QEMUFile *f,
                                           PostcopyOutgoingState *s)
{
    assert(s->state == PO_STATE_ACTIVE);

    s->state = PO_STATE_ALL_PAGES_SENT;
    /* tell incoming side that all pages are sent */
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    qemu_buffered_file_drain(f);
    DPRINTF("sent RAM_SAVE_FLAG_EOS\n");
    migrate_fd_cleanup(s->ms);

    /* Later migrate_fd_complete() will be called which calls
     * migrate_fd_cleanup() again. So dummy file is created
     * for qemu monitor to keep working.
     */
    s->ms->file = qemu_fopen_ops(NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL);
    s->mig_buffered_write = NULL;

    migration_bitmap_free();
}

int postcopy_outgoing_ram_save_background(QEMUFile *f, void *postcopy)
{
    PostcopyOutgoingState *s = postcopy;
#define MAX_WAIT        50      /* stolen from ram_save_iterate() */
    double t0;
    int i;

    assert(s->state == PO_STATE_ACTIVE ||
           s->state == PO_STATE_EOC_RECEIVED ||
           s->state == PO_STATE_ERROR_RECEIVE);

    switch (s->state) {
    case PO_STATE_ACTIVE:
        /* nothing. processed below */
        break;
    case PO_STATE_EOC_RECEIVED:
        qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
        s->state = PO_STATE_COMPLETED;
        postcopy_outgoing_completed(s);
        DPRINTF("PO_STATE_COMPLETED\n");
        return 1;
    case PO_STATE_ERROR_RECEIVE:
        postcopy_outgoing_completed(s);
        DPRINTF("PO_STATE_ERROR_RECEIVE\n");
        return -1;
    default:
        abort();
    }

    if (s->ms->params.nobg) {
        if (ram_bytes_remaining() == 0) {
            postcopy_outgoing_ram_all_sent(f, s);
        }
        return 0;
    }

    DPRINTF("outgoing background state: %d\n", s->state);
    t0 = qemu_get_clock_ns(rt_clock);
    while (qemu_file_rate_limit(f) == 0) {
        int nfds = -1;
        fd_set readfds;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 0};
        int ret;

        if (ram_save_block(f, false) == 0) { /* no more blocks */
            DPRINTF("outgoing background all sent\n");
            assert(s->state == PO_STATE_ACTIVE);
            postcopy_outgoing_ram_all_sent(f, s);
            return 0;
        }

        FD_ZERO(&readfds);
        set_fd(s->fd_read, &readfds, &nfds);
        ret = select(nfds + 1, &readfds, NULL, NULL, &timeout);
        if (ret >= 0 && FD_ISSET(s->fd_read, &readfds)) {
            /* page request is pending */
            DPRINTF("pending request\n");
            break;
        }

        /* stolen from ram_save_iterate() */
        if ((i & 63) == 0) {
            int64_t t1 = (qemu_get_clock_ns(rt_clock) - t0) / 1000000;
            if (t1 > MAX_WAIT) {
                DPRINTF("too long %"PRIu64"\n", t1);
                break;
            }
        }
        i++;
    }

    return 0;
}

/***************************************************************************
 * incoming part
 */

bool incoming_postcopy = false;


#define PIS_STATE_QUIT_RECEIVED         0x01
#define PIS_STATE_QUIT_QUEUED           0x02
#define PIS_STATE_QUIT_SENT             0x04

#define PIS_STATE_QUIT_MASK             (PIS_STATE_QUIT_RECEIVED | \
                                         PIS_STATE_QUIT_QUEUED | \
                                         PIS_STATE_QUIT_SENT)

struct PostcopyIncomingState {
    /* dest qemu state */
    uint32_t    state;

    int host_page_size;
    int host_page_shift;

    /* qemu side */
    int to_umemd_fd;
    QEMUFile *to_umemd;

    int from_umemd_fd;
    QEMUFile *from_umemd;
    int version_id;     /* save/load format version id */
};
typedef struct PostcopyIncomingState PostcopyIncomingState;


#define UMEM_STATE_EOS_RECEIVED         0x01    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SEND_REQ         0x02    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SENDING          0x04    /* umem daemon <-> src qemu */
#define UMEM_STATE_EOC_SENT             0x08    /* umem daemon <-> src qemu */

#define UMEM_STATE_QUIT_RECEIVED        0x10    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_HANDLED         0x20    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_QUEUED          0x40    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_SENDING         0x80    /* umem daemon <-> dst qemu */
#define UMEM_STATE_QUIT_SENT            0x100   /* umem daemon <-> dst qemu */

#define UMEM_STATE_QUIT_MASK            (UMEM_STATE_QUIT_QUEUED |   \
                                         UMEM_STATE_QUIT_SENDING |  \
                                         UMEM_STATE_QUIT_SENT |     \
                                         UMEM_STATE_QUIT_RECEIVED | \
                                         UMEM_STATE_QUIT_HANDLED)
#define UMEM_STATE_END_MASK             (UMEM_STATE_EOS_RECEIVED | \
                                         UMEM_STATE_EOC_SEND_REQ | \
                                         UMEM_STATE_EOC_SENDING |  \
                                         UMEM_STATE_EOC_SENT |     \
                                         UMEM_STATE_QUIT_MASK)

struct UMemBlock {
    UMem* umem;
    char idstr[256];
    ram_addr_t offset;
    ram_addr_t length;
    QLIST_ENTRY(UMemBlock) next;
};
typedef struct UMemBlock UMemBlock;

struct PostcopyIncomingUMemDaemon {
    /* umem daemon side */
    QemuMutex mutex;
    uint32_t state;     /* shared state. protected by mutex */

    /* read only */
    int host_page_size;
    int host_page_shift;
    int nr_host_pages_per_target_page;
    int host_to_target_page_shift;
    int nr_target_pages_per_host_page;
    int target_to_host_page_shift;
    int version_id;     /* save/load format version id */
    bool precopy_enabled;

    QemuThread thread;
    QLIST_HEAD(, UMemBlock) blocks;

    /* thread to communicate with qemu main loop via pipe */
    QemuThread pipe_thread;
    int to_qemu_fd;
    QEMUFile *to_qemu;
    int from_qemu_fd;
    QEMUFile *from_qemu;

    /* = KVM_MAX_VCPUS * (ASYNC_PF_PER_VCPUS + 1) */
#define MAX_REQUESTS    (512 * (64 + 1))

    /* thread to read from outgoing qemu */
    QemuThread mig_read_thread;
    int mig_read_fd;
    QEMUFile *mig_read;                 /* qemu on source -> umem daemon */
    UMemBlock *last_block_read;         /* qemu on source -> umem daemon */
    /* bitmap indexed by target page offset */
    unsigned long *phys_received;
    struct umem_pages *page_cached;

    /* thread to write to outgoing qemu */
    QemuThread mig_write_thread;
    int mig_write_fd;
    QEMUFile *mig_write;                /* umem daemon -> qemu on source */
    UMemBlock *last_block_write;        /* umem daemon -> qemu on source */
    /* bitmap indexed by target page offset */
    unsigned long *phys_requested;
    struct umem_pages *page_request;
    uint64_t *target_pgoffs;
};
typedef struct PostcopyIncomingUMemDaemon PostcopyIncomingUMemDaemon;

static PostcopyIncomingState state = {
    .state = 0,
    .to_umemd_fd = -1,
    .to_umemd = NULL,
    .from_umemd_fd = -1,
    .from_umemd = NULL,
};

static PostcopyIncomingUMemDaemon umemd = {
    .state = 0,
    .precopy_enabled = false,
    .to_qemu_fd = -1,
    .to_qemu = NULL,
    .from_qemu_fd = -1,
    .from_qemu = NULL,
    .blocks = QLIST_HEAD_INITIALIZER(&umemd.blocks),
    .mig_read_fd = -1,
    .mig_read = NULL,
    .mig_write_fd = -1,
    .mig_write = NULL,
};

static void *postcopy_incoming_umemd(void*);
static UMemBlock *postcopy_incoming_umem_block_from_stream(
    QEMUFile *f, int flags);

/* protected by qemu_mutex_lock_ramlist() */
void postcopy_incoming_ram_free(RAMBlock *ram_block)
{
    UMemBlock *block;
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(ram_block->idstr, block->idstr, strlen(block->idstr))) {
            break;
        }
    }
    if (block != NULL) {
        umem_unmap(block->umem);
    } else {
        munmap(ram_block->host, ram_block->length);
    }
}

static int postcopy_incoming_ram_load_get64(QEMUFile *f,
                                            ram_addr_t *addr, int *flags)
{
    *addr = qemu_get_be64(f);
    *flags = *addr & ~TARGET_PAGE_MASK;
    *addr &= TARGET_PAGE_MASK;
    return qemu_file_get_error(f);
}

int postcopy_incoming_ram_load(QEMUFile *f, void *opaque, int version_id)
{
    ram_addr_t addr;
    int flags;
    int error;

    DPRINTF("incoming ram load\n");
    /*
     * RAM_SAVE_FLAGS_EOS or
     * RAM_SAVE_FLAGS_MEM_SIZE + mem size + RAM_SAVE_FLAGS_EOS
     * see postcopy_outgoing_ram_save_live()
     */

    if (version_id != RAM_SAVE_VERSION_ID) {
        DPRINTF("RAM_SAVE_VERSION_ID %d != %d\n",
                version_id, RAM_SAVE_VERSION_ID);
        return -EINVAL;
    }
    error = postcopy_incoming_ram_load_get64(f, &addr, &flags);
    DPRINTF("addr 0x%lx flags 0x%x\n", addr, flags);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }
    if (flags == RAM_SAVE_FLAG_EOS && addr == 0) {
        DPRINTF("EOS\n");
        return 0;
    }

    if (flags != RAM_SAVE_FLAG_MEM_SIZE) {
        DPRINTF("-EINVAL flags 0x%x\n", flags);
        return -EINVAL;
    }
    error = ram_load_mem_size(f, addr);
    if (error) {
        DPRINTF("addr 0x%lx error %d\n", addr, error);
        return error;
    }

    error = postcopy_incoming_ram_load_get64(f, &addr, &flags);
    if (error) {
        DPRINTF("addr 0x%lx flags 0x%x error %d\n", addr, flags, error);
        return error;
    }
    if (flags == RAM_SAVE_FLAG_EOS && addr == 0) {
        DPRINTF("done\n");
        return 0;
    }
    DPRINTF("-EINVAL\n");
    return -EINVAL;
}

static void*
postcopy_incoming_shmem_from_stream_offset(QEMUFile *f, ram_addr_t offset,
                                           int flags)
{
    UMemBlock *block = postcopy_incoming_umem_block_from_stream(f, flags);
    if (block == NULL) {
        DPRINTF("error block = NULL\n");
        return NULL;
    }
    return block->umem->shmem + offset;
}

static int postcopy_incoming_ram_load_precopy(QEMUFile *f, void *opaque,
                                              int version_id)
{
    return ram_load(f, opaque, version_id,
                    &postcopy_incoming_shmem_from_stream_offset);
}

static void postcopy_incoming_prepare(void)
{
    RAMBlock *block;
    int nbits;

    state.state = 0;
    state.host_page_size = getpagesize();
    state.host_page_shift = ffs(state.host_page_size) - 1;
    state.version_id = RAM_SAVE_VERSION_ID; /* = save version of
                                               ram_save_live() */

    qemu_mutex_init(&umemd.mutex);
    umemd.host_page_size = state.host_page_size;
    umemd.host_page_shift = state.host_page_shift;

    umemd.nr_host_pages_per_target_page =
        TARGET_PAGE_SIZE / umemd.host_page_size;
    umemd.nr_target_pages_per_host_page =
        umemd.host_page_size / TARGET_PAGE_SIZE;
    umemd.target_to_host_page_shift =
        ffs(umemd.nr_host_pages_per_target_page) - 1;
    umemd.host_to_target_page_shift =
        ffs(umemd.nr_target_pages_per_host_page) - 1;

    QLIST_INIT(&umemd.blocks);
    qemu_mutex_lock_ramlist();
    QLIST_FOREACH(block, &ram_list.blocks, next) {
        UMemBlock *umem_block;

        if (block->flags & RAM_PREALLOC_MASK) {
            continue;
        }
        umem_block = g_malloc0(sizeof(*umem_block));
        umem_block->umem = umem_new(block->host, block->length);;
        umem_block->offset = block->offset;
        umem_block->length = block->length;
        pstrcpy(umem_block->idstr, sizeof(umem_block->idstr), block->idstr);

        umem_map_shmem(umem_block->umem);
        umem_close_shmem(umem_block->umem);

        block->flags |= RAM_POSTCOPY_UMEM_MASK;
        QLIST_INSERT_HEAD(&umemd.blocks, umem_block, next);
    }
    qemu_mutex_unlock_ramlist();

    umemd.page_request = g_malloc(umem_pages_size(MAX_REQUESTS));
    umemd.page_cached = g_malloc(
        umem_pages_size(MAX_REQUESTS *
                        (TARGET_PAGE_SIZE >= umemd.host_page_size ?
                         1: umemd.nr_host_pages_per_target_page)));
    umemd.target_pgoffs =
        g_new(uint64_t, MAX_REQUESTS *
              MAX(umemd.nr_host_pages_per_target_page,
                  umemd.nr_target_pages_per_host_page));

    nbits = last_ram_offset() >> TARGET_PAGE_BITS;
    umemd.phys_requested = bitmap_new(nbits);
    umemd.phys_received = bitmap_new(nbits);
    umemd.last_block_read = NULL;
    umemd.last_block_write = NULL;
}

static int postcopy_incoming_loadvm_init(QEMUFile *f, uint32_t size)
{
    uint64_t options;
    int flags;

    if (size != sizeof(options)) {
        fprintf(stderr, "unknown size %d\n", size);
        return -EINVAL;
    }
    options = qemu_get_be64(f);
    if (options & POSTCOPY_OPTION_PRECOPY) {
        options &= ~POSTCOPY_OPTION_PRECOPY;
        umemd.precopy_enabled = true;
    } else {
        umemd.precopy_enabled = false;
    }
    if (options) {
        fprintf(stderr, "unknown options 0x%"PRIx64, options);
        return -ENOSYS;
    }
    flags = fcntl(qemu_file_fd(f), F_GETFL);
    if ((flags & O_ACCMODE) != O_RDWR) {
        /* postcopy requires read/write file descriptor */
        fprintf(stderr, "non-writable connection. "
                "postcopy requires read/write connection \n");
        return -EINVAL;
    }
    if (mem_path) {
        fprintf(stderr, "mem_path is specified to %s. "
                "postcopy doesn't work with it\n", mem_path);
        return -ENOSYS;
    }
    DPRINTF("detected POSTCOPY precpoy %d\n", umemd.precopy_enabled);
    incoming_postcopy = true;
    postcopy_incoming_prepare();
    if (umemd.precopy_enabled) {
        savevm_ram_handlers.load_state = postcopy_incoming_ram_load_precopy;
    } else {
        savevm_ram_handlers.load_state = postcopy_incoming_ram_load;
    }

    return 0;
}

static void postcopy_incoming_create_umemd_thread(QEMUFile *mig_read)
{
    int fds[2];
    int mig_read_fd;
    int mig_write_fd;
    assert((fcntl(qemu_file_fd(mig_read), F_GETFL) & O_ACCMODE) == O_RDWR);

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        abort();
    }
    state.from_umemd_fd = fds[0];
    umemd.to_qemu_fd = fds[1];

    if (qemu_pipe(fds) == -1) {
        perror("qemu_pipe");
        abort();
    }
    umemd.from_qemu_fd = fds[0];
    state.to_umemd_fd = fds[1];

    mig_read_fd = qemu_file_fd(mig_read);
    umemd.state = 0;
    umemd.version_id = state.version_id;
    umemd.mig_read_fd = mig_read_fd;
    umemd.mig_read = mig_read;

    mig_write_fd = dup(mig_read_fd);
    if (mig_write_fd < 0) {
        perror("could not dup for writable socket \n");
        abort();
    }
    umemd.mig_write_fd = mig_write_fd;
    umemd.mig_write = qemu_fopen_fd(mig_write_fd, "w");

    qemu_thread_create(&umemd.thread, &postcopy_incoming_umemd,
                       NULL, QEMU_THREAD_DETACHED);

    umem_qemu_wait_for_daemon(state.from_umemd_fd);
    /* now socket is disowned. So tell umem thread that it's safe to use it */
    postcopy_incoming_qemu_ready();
}

static int postcopy_incoming_loadvm_section_full(QEMUFile *f, uint32_t size,
                                                 QEMUFile **buf_file)
{
    uint8_t *buf;
    int read_size;

    buf = g_malloc(size);
    read_size = qemu_get_buffer(f, buf, size);
    if (size != read_size) {
        fprintf(stderr, "qemu: warning: error while postcopy size %d %d\n",
                size, read_size);
        g_free(buf);
        return -EINVAL;
    }
    postcopy_incoming_create_umemd_thread(f);

    /* VMStateDescription:pre/post_load and
     * cpu_sychronize_all_post_init() may fault on guest RAM.
     * (MSR_KVM_WALL_CLOCK, MSR_KVM_SYSTEM_TIME)
     * postcopy daemon needs to be forked before the fault.
     */
    *buf_file = qemu_fopen_buf_read(buf, size);
    return 0;
}

int postcopy_incoming_loadvm_state(QEMUFile *f, QEMUFile **buf_file)
{
    int ret = 0;
    uint8_t subtype;
    uint32_t size;

    subtype = qemu_get_ubyte(f);
    size = qemu_get_be32(f);
    switch (subtype) {
    case QEMU_VM_POSTCOPY_INIT:
        ret = postcopy_incoming_loadvm_init(f, size);
        break;
    case QEMU_VM_POSTCOPY_SECTION_FULL:
        ret = postcopy_incoming_loadvm_section_full(f, size, buf_file);
        break;
    default:
        ret = -EINVAL;
        break;
    }
    return ret;
}

static void postcopy_incoming_qemu_recv_quit(void)
{
    if (state.state & PIS_STATE_QUIT_RECEIVED) {
        return;
    }

    DPRINTF("|= PIS_STATE_QUIT_RECEIVED\n");
    state.state |= PIS_STATE_QUIT_RECEIVED;
    qemu_set_fd_handler(state.from_umemd_fd, NULL, NULL, NULL);
    qemu_fclose(state.from_umemd);
    state.from_umemd = NULL;
    fd_close(&state.from_umemd_fd);
}

static void postcopy_incoming_qemu_check_quite_queued(void)
{
    if (state.state & PIS_STATE_QUIT_QUEUED &&
        !(state.state & PIS_STATE_QUIT_SENT)) {
        DPRINTF("|= PIS_STATE_QUIT_SENT\n");
        state.state |= PIS_STATE_QUIT_SENT;

        qemu_fclose(state.to_umemd);
        state.to_umemd = NULL;
        fd_close(&state.to_umemd_fd);
    }
}

static void postcopy_incoming_qemu_queue_quit(void)
{
    if (state.state & PIS_STATE_QUIT_QUEUED) {
        return;
    }

    DPRINTF("|= PIS_STATE_QUIT_QUEUED\n");
    umem_qemu_quit(state.to_umemd);
    state.state |= PIS_STATE_QUIT_QUEUED;
}

static int postcopy_incoming_qemu_handle_req_one(void)
{
    uint8_t cmd;

    cmd = qemu_get_ubyte(state.from_umemd);
    DPRINTF("cmd %c\n", cmd);

    switch (cmd) {
    case UMEM_DAEMON_QUIT:
        postcopy_incoming_qemu_recv_quit();
        postcopy_incoming_qemu_queue_quit();
        postcopy_incoming_qemu_cleanup();
        break;
    case UMEM_DAEMON_ERROR:
        /* umem daemon hit troubles, so it warned us to stop vm execution */
        vm_stop(RUN_STATE_IO_ERROR); /* or RUN_STATE_INTERNAL_ERROR */
        break;
    default:
        DPRINTF("unknown command %d\n", cmd);
        abort();
        break;
    }

    return 0;
}

static void postcopy_incoming_qemu_handle_req(void *opaque)
{
    postcopy_incoming_qemu_handle_req_one();
    postcopy_incoming_qemu_check_quite_queued();
}

void postcopy_incoming_qemu_ready(void)
{
    umem_qemu_ready(state.to_umemd_fd);

    state.from_umemd = qemu_fopen_fd(state.from_umemd_fd, "r");
    state.to_umemd = qemu_fopen_fd(state.to_umemd_fd, "w");
    qemu_set_fd_handler(state.from_umemd_fd,
                        postcopy_incoming_qemu_handle_req, NULL, NULL);
}

void postcopy_incoming_qemu_cleanup(void)
{
    /* when qemu will quit before completing postcopy, tell umem daemon
       to tear down umem device and exit. */
    if (state.to_umemd_fd >= 0) {
        postcopy_incoming_qemu_queue_quit();
        postcopy_incoming_qemu_check_quite_queued();
    }
}

/**************************************************************************
 * incoming umem daemon
 */

static void postcopy_incoming_umem_recv_quit(void)
{
    qemu_mutex_lock(&umemd.mutex);
    if (umemd.state & UMEM_STATE_QUIT_RECEIVED) {
        qemu_mutex_unlock(&umemd.mutex);
        return;
    }
    DPRINTF("|= UMEM_STATE_QUIT_RECEIVED\n");
    umemd.state |= UMEM_STATE_QUIT_RECEIVED;
    qemu_mutex_unlock(&umemd.mutex);

    qemu_fclose(umemd.from_qemu);
    umemd.from_qemu = NULL;
    fd_close(&umemd.from_qemu_fd);

    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_QUIT_HANDLED\n");
    umemd.state |= UMEM_STATE_QUIT_HANDLED;
    qemu_mutex_unlock(&umemd.mutex);
}

/* call with umemd.mutex held */
static void postcopy_incoming_umem_queue_quit_locked(void)
{
    if (umemd.state & UMEM_STATE_QUIT_QUEUED) {
        return;
    }
    DPRINTF("|= UMEM_STATE_QUIT_QUEUED\n");
    umemd.state |= UMEM_STATE_QUIT_QUEUED;
}

static void postcopy_incoming_umem_check_eoc_req(void)
{
    struct qemu_umem_req req;

    qemu_mutex_lock(&umemd.mutex);
    if (!(umemd.state & UMEM_STATE_EOC_SEND_REQ) ||
        umemd.state & (UMEM_STATE_EOC_SENDING | UMEM_STATE_EOC_SENT)) {
        qemu_mutex_unlock(&umemd.mutex);
        return;
    }

    DPRINTF("|= UMEM_STATE_EOC_SENDING\n");
    umemd.state |= UMEM_STATE_EOC_SENDING;
    qemu_mutex_unlock(&umemd.mutex);

    req.cmd = QEMU_UMEM_REQ_EOC;
    postcopy_incoming_send_req(umemd.mig_write, &req);
    qemu_fclose(umemd.mig_write);
    umemd.mig_write = NULL;
    fd_close(&umemd.mig_write_fd);

    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_EOC_SENT\n");
    umemd.state |= UMEM_STATE_EOC_SENT;
    qemu_mutex_unlock(&umemd.mutex);
}

static void postcopy_incoming_umem_req_eoc(void)
{
    qemu_mutex_lock(&umemd.mutex);
    DPRINTF("|= UMEM_STATE_EOC_SEND_REQ\n");
    umemd.state |= UMEM_STATE_EOC_SEND_REQ;
    qemu_mutex_unlock(&umemd.mutex);
}

static void postcopy_incoming_umem_send_page_req(UMemBlock *block)
{
    struct qemu_umem_req req;
    unsigned long bit;
    uint64_t target_pgoff;
    int i;

    umemd.page_request->nr = MAX_REQUESTS;
    umem_get_page_request(block->umem, umemd.page_request);
    DPRINTF("id %s nr %"PRId64" offs 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, (uint64_t)umemd.page_request->nr,
            (uint64_t)umemd.page_request->pgoffs[0],
            (uint64_t)umemd.page_request->pgoffs[1]);

    if (umemd.last_block_write != block) {
        req.cmd = QEMU_UMEM_REQ_PAGE;
        req.idstr = block->idstr;
    } else {
        req.cmd = QEMU_UMEM_REQ_PAGE_CONT;
    }

    req.nr = 0;
    req.pgoffs = umemd.target_pgoffs;
    if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
        for (i = 0; i < umemd.page_request->nr; i++) {
            target_pgoff = umemd.page_request->pgoffs[i] >>
                umemd.host_to_target_page_shift;
            bit = (block->offset >> TARGET_PAGE_BITS) + target_pgoff;

            if (!test_and_set_bit(bit, umemd.phys_requested)) {
                req.pgoffs[req.nr] = target_pgoff;
                req.nr++;
            }
        }
    } else {
        for (i = 0; i < umemd.page_request->nr; i++) {
            int j;
            target_pgoff = umemd.page_request->pgoffs[i] <<
                umemd.host_to_target_page_shift;
            bit = (block->offset >> TARGET_PAGE_BITS) + target_pgoff;

            for (j = 0; j < umemd.nr_target_pages_per_host_page; j++) {
                if (!test_and_set_bit(bit + j, umemd.phys_requested)) {
                    req.pgoffs[req.nr] = target_pgoff + j;
                    req.nr++;
                }
            }
        }
    }

    DPRINTF("id %s nr %d offs 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, req.nr, req.pgoffs[0], req.pgoffs[1]);
    if (req.nr > 0 && umemd.mig_write != NULL) {
        postcopy_incoming_send_req(umemd.mig_write, &req);
        umemd.last_block_write = block;
    }
}

static void postcopy_incoming_umem_page_fault(UMemBlock *block,
                                              const struct umem_pages *pages)
{
    uint64_t i;

    for (i = 0; i < pages->nr; i++) {
        size_t offset = pages->pgoffs[i] << umemd.host_page_shift;
        RAMBlock *ram_block;

        /* make pages present by forcibly triggering page fault. */
        qemu_mutex_lock_ramlist();
        ram_block = ram_find_block(block->idstr, strlen(block->idstr));
        if (ram_block && offset < ram_block->length) {
            volatile uint8_t *ram =
                memory_region_get_ram_ptr(ram_block->mr) + offset;
            uint8_t dummy_read = ram[0];
            (void)dummy_read;   /* suppress unused variable warning */
        }
        qemu_mutex_unlock_ramlist();

        umem_remove_shmem(block->umem, offset, umemd.host_page_size);
    }
}

static bool postcopy_incoming_umem_check_umem_done(void)
{
    bool all_done = true;
    UMemBlock *block;

    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (umem_shmem_finished(block->umem)) {
            umem_unmap_shmem(block->umem);
        } else {
            all_done = false;
            break;
        }
    }

    return all_done;
}

static void postcopy_incoming_umem_done(void)
{
    postcopy_incoming_umem_req_eoc();
    qemu_mutex_lock(&umemd.mutex);
    postcopy_incoming_umem_queue_quit_locked();
    qemu_mutex_unlock(&umemd.mutex);
}

static UMemBlock *postcopy_incoming_umem_block_from_stream(
    QEMUFile *f, int flags)
{
    uint8_t len;
    char id[256];
    UMemBlock *block;

    if (flags & RAM_SAVE_FLAG_CONTINUE) {
        return umemd.last_block_read;
    }

    len = qemu_get_byte(f);
    qemu_get_buffer(f, (uint8_t*)id, len);
    id[len] = 0;

    DPRINTF("idstr: %s len %d\n", id, len);
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(id, block->idstr, len)) {
            umemd.last_block_read = block;
            return block;
        }
    }
    DPRINTF("error\n");
    return NULL;
}

static int postcopy_incoming_umem_ram_load(void)
{
    ram_addr_t offset;
    int flags;
    UMemBlock *block;

    void *shmem;
    int error;
    int i;
    int bit;

    if (umemd.version_id != RAM_SAVE_VERSION_ID) {
        return -EINVAL;
    }

    error = postcopy_incoming_ram_load_get64(umemd.mig_read, &offset, &flags);
    /* DPRINTF("offset 0x%lx flags 0x%x\n", offset, flags); */
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }
    assert(!(flags & RAM_SAVE_FLAG_MEM_SIZE));

    if (flags & RAM_SAVE_FLAG_EOS) {
        DPRINTF("RAM_SAVE_FLAG_EOS\n");
        postcopy_incoming_umem_req_eoc();

        qemu_fclose(umemd.mig_read);
        umemd.mig_read = NULL;
        fd_close(&umemd.mig_read_fd);

        qemu_mutex_lock(&umemd.mutex);
        umemd.state |= UMEM_STATE_EOS_RECEIVED;
        postcopy_incoming_umem_queue_quit_locked();
        qemu_mutex_unlock(&umemd.mutex);
        DPRINTF("|= UMEM_STATE_EOS_RECEIVED\n");
        return 0;
    }

    block = postcopy_incoming_umem_block_from_stream(umemd.mig_read, flags);
    if (block == NULL) {
        return -EINVAL;
    }
    assert(!umem_shmem_finished(block->umem));
    shmem = block->umem->shmem + offset;
    error = ram_load_page(umemd.mig_read, shmem, flags);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }
    error = qemu_file_get_error(umemd.mig_read);
    if (error) {
        DPRINTF("error %d\n", error);
        return error;
    }

    umemd.page_cached->nr = 0;
    bit = (block->offset + offset) >> TARGET_PAGE_BITS;
    if (!test_and_set_bit(bit, umemd.phys_received)) {
        if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
            uint64_t pgoff = offset >> umemd.host_page_shift;
            for (i = 0; i < umemd.nr_host_pages_per_target_page; i++) {
                umemd.page_cached->pgoffs[umemd.page_cached->nr] = pgoff + i;
                umemd.page_cached->nr++;
            }
        } else {
            bool mark_cache = true;
            for (i = 0; i < umemd.nr_target_pages_per_host_page; i++) {
                if (!test_bit(bit + i, umemd.phys_received)) {
                    mark_cache = false;
                    break;
                }
            }
            if (mark_cache) {
                umemd.page_cached->pgoffs[0] =
                    offset >> umemd.host_page_shift;
                umemd.page_cached->nr = 1;
            }
        }
    }

    if (umemd.page_cached->nr > 0) {
        umem_mark_page_cached(block->umem, umemd.page_cached);
        postcopy_incoming_umem_page_fault(block, umemd.page_cached);
        if (postcopy_incoming_umem_check_umem_done()) {
            postcopy_incoming_umem_done();
        }
    }

    return 0;
}

static int
postcopy_incoming_umemd_read_dirty_bitmap(
    QEMUFile *f, const char *idstr, uint8_t idlen,
    uint64_t block_offset, uint64_t block_length, uint64_t bitmap_length)
{
    UMemBlock *block;
    uint64_t bit_start = block_offset >> TARGET_PAGE_BITS;
    uint64_t bit_end = (block_offset + block_length) >> TARGET_PAGE_BITS;
    uint64_t bit_offset;
    uint8_t *buffer;
    uint64_t index;

    if ((bitmap_length % sizeof(uint64_t)) != 0) {
        return -EINVAL;
    }
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (!strncmp(block->idstr, idstr, idlen)) {
            break;
        }
    }
    if (block == NULL) {
        return -EINVAL;
    }

    DPRINTF("bitmap %s 0x%"PRIx64" 0x%"PRIx64" 0x%"PRIx64"\n",
            block->idstr, block_offset, block_length, bitmap_length);
    buffer = g_malloc(bitmap_length);
    qemu_get_buffer(f, buffer, bitmap_length);

    bit_offset = bit_start & ~63;
    index = 0;
    while (index < bitmap_length) {
        uint64_t bitmap;
        int i;
        int j;
        int bit;

        bitmap = be64_to_cpup((uint64_t*)(buffer + index));
        for (i = 0; i < 64; i++) {
            bit = bit_offset + i;
            if (bit < bit_start) {
                continue;
            }
            if (bit >= bit_end) {
                break;
            }
            if (!(bitmap & (1ULL << i))) {
                set_bit(bit, umemd.phys_received);

                /* this is racy, but write side just sends redundant request */
                set_bit(bit, umemd.phys_requested);
            }
        }

        umemd.page_cached->nr = 0;
        if (TARGET_PAGE_SIZE >= umemd.host_page_size) {
            for (i = 0; i < 64; i++) {
                uint64_t pgoff;
                bit = bit_offset + i;
                if (bit < bit_start) {
                    continue;
                }
                if (bit >= bit_end) {
                    break;
                }
                if (!test_bit(bit, umemd.phys_received)) {
                    continue;
                }
                pgoff = (bit - bit_start) << umemd.target_to_host_page_shift;
                for (j = 0; j < umemd.nr_host_pages_per_target_page; j++) {
                    umemd.page_cached->pgoffs[umemd.page_cached->nr] =
                        pgoff + j;
                    umemd.page_cached->nr++;
                }
            }
        } else {
            for (i = 0; i < 64; i += umemd.nr_target_pages_per_host_page) {
                bool mark_cache = true;
                bit = bit_offset + i;
                if (bit < bit_start) {
                    continue;
                }
                if (bit >= bit_end) {
                    break;
                }
                if (!test_bit(bit, umemd.phys_received)) {
                    continue;
                }
                for (j = 0; j < umemd.nr_target_pages_per_host_page; j++) {
                    if (!test_bit(bit + j, umemd.phys_received)) {
                        mark_cache = false;
                        break;
                    }
                }
                if (mark_cache) {
                    umemd.page_cached->pgoffs[umemd.page_cached->nr] =
                        (bit - bit_start) >>
                        (umemd.host_page_shift - TARGET_PAGE_BITS);
                    umemd.page_cached->nr++;
                }
            }
        }

        if (umemd.page_cached->nr > 0) {
            umem_mark_page_cached(block->umem, umemd.page_cached);
            postcopy_incoming_umem_page_fault(block, umemd.page_cached);
        }

        bit_offset += 64;
        index += sizeof(bitmap);
    }

    g_free(buffer);
    return 0;
}

static int postcopy_incoming_umemd_mig_read_init(void)
{
    QEMUFile *f = umemd.mig_read;
    uint64_t start = qemu_get_clock_ns(rt_clock);
    uint64_t end;

    if (!umemd.precopy_enabled) {
        return 0;
    }

    for (;;) {
        uint8_t idlen;
        char idstr[256];
        uint64_t block_offset;
        uint64_t block_length;
        uint64_t bitmap_length;
        int ret;

        idlen = qemu_get_byte(f);
        qemu_get_buffer(f, (uint8_t*)idstr, idlen);
        idstr[idlen] = 0;
        block_offset = qemu_get_be64(f);
        block_length = qemu_get_be64(f);
        bitmap_length = qemu_get_be64(f);

        if (idlen == 0 && block_offset == 0 && block_length == 0 &&
            bitmap_length == 0) {
            DPRINTF("bitmap done\n");
            break;
        }
        ret = postcopy_incoming_umemd_read_dirty_bitmap(
            f, idstr, idlen, block_offset, block_length, bitmap_length);
        if (ret < 0) {
            return ret;
        }
    }
    if (postcopy_incoming_umem_check_umem_done()) {
        postcopy_incoming_umem_done();
    }
    end = qemu_get_clock_ns(rt_clock);
    DPRINTF("bitmap %"PRIu64" nsec\n", end - start);
    return 0;
}

static int
postcopy_incoming_umemd_mig_read_loop(void)
{
    /* read thread doesn't need to check periodically UMEM_STATE_EOC_SEND_REQ
     * because RAM_SAVE_FLAG_EOS is always sent by the outgoing part. */
    if (umemd.mig_read_fd < 0) {
        return -EINVAL;
    }
    return postcopy_incoming_umem_ram_load();
}

static int
postcopy_incoming_umemd_mig_write_loop(void)
{
    int ret;
    UMemBlock *block;
    /* to check UMEM_STATE_EOC_SEND_REQ periodically */
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int nfds = -1;
    fd_set readfds;
    FD_ZERO(&readfds);

    QLIST_FOREACH(block, &umemd.blocks, next) {
        set_fd(block->umem->fd, &readfds, &nfds);
    }
    ret = select(nfds + 1, &readfds, NULL, NULL, &timeout);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        return ret;
    }
    QLIST_FOREACH(block, &umemd.blocks, next) {
        if (FD_ISSET(block->umem->fd, &readfds)) {
            postcopy_incoming_umem_send_page_req(block);
        }
    }
    if (umemd.mig_write != NULL) {
        qemu_fflush(umemd.mig_write);
    }
    postcopy_incoming_umem_check_eoc_req();

    return 0;
}

static int
postcopy_incoming_umemd_pipe_init(void)
{
    umem_daemon_ready(umemd.to_qemu_fd);
    umemd.to_qemu = qemu_fopen_fd(umemd.to_qemu_fd, "w");

    /* wait for qemu to disown migration_fd */
    umem_daemon_wait_for_qemu(umemd.from_qemu_fd);
    umemd.from_qemu = qemu_fopen_fd(umemd.from_qemu_fd, "r");
    return 0;
}

static int
postcopy_incoming_umemd_pipe_loop(void)
{
    int ret;
    /* to check UMEM_STATE_QUIT_QUEUED periodically */
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    fd_set readfds;
    int nfds = -1;

    FD_ZERO(&readfds);
    if (umemd.from_qemu_fd < 0) {
        set_fd(umemd.from_qemu_fd, &readfds, &nfds);
    }
    ret = select(nfds + 1, &readfds, NULL, NULL, &timeout);
    if (ret == -1) {
        if (errno == EINTR) {
            return 0;
        }
        return ret;
    }
    if (umemd.from_qemu_fd >= 0 && FD_ISSET(umemd.from_qemu_fd, &readfds)) {
        uint8_t cmd;
        cmd = qemu_get_ubyte(umemd.from_qemu);
        DPRINTF("cmd %c\n", cmd);
        switch (cmd) {
        case UMEM_QEMU_QUIT:
            postcopy_incoming_umem_recv_quit();
            postcopy_incoming_umem_done();
            break;
        default:
            abort();
            break;
        }
        qemu_fflush(umemd.to_qemu);
    }

    if (umemd.to_qemu != NULL) {
        qemu_mutex_lock(&umemd.mutex);
        if (umemd.state & UMEM_STATE_QUIT_QUEUED &&
            !(umemd.state & (UMEM_STATE_QUIT_SENDING |
                             UMEM_STATE_QUIT_SENT))) {
            DPRINTF("|= UMEM_STATE_QUIT_SENDING\n");
            umemd.state |= UMEM_STATE_QUIT_SENDING;
            qemu_mutex_unlock(&umemd.mutex);

            umem_daemon_quit(umemd.to_qemu);
            qemu_fclose(umemd.to_qemu);
            umemd.to_qemu = NULL;
            fd_close(&umemd.to_qemu_fd);

            qemu_mutex_lock(&umemd.mutex);
            DPRINTF("|= UMEM_STATE_QUIT_SENT\n");
            umemd.state |= UMEM_STATE_QUIT_SENT;
        }
        qemu_mutex_unlock(&umemd.mutex);
    }

    return 0;
}

struct IncomingThread {
    int (*init_func)(void);
    int (*loop_func)(void);
};
typedef struct IncomingThread IncomingThread;

static void*
postcopy_incoming_umemd_thread(void* arg)
{
    IncomingThread *im  = arg;
    int error;

    DPRINTF("loop %d %p %p\n", getpid(), im->init_func, im->loop_func);
    if (im->init_func) {
        error = im->init_func();
        if (error) {
            return NULL;
        }
    }
    for (;;) {
        qemu_mutex_lock(&umemd.mutex);
        if ((umemd.state & UMEM_STATE_END_MASK) == UMEM_STATE_END_MASK) {
            qemu_mutex_unlock(&umemd.mutex);
            DPRINTF("loop out %p\n", im->loop_func);
            break;
        }
        qemu_mutex_unlock(&umemd.mutex);

        error = im->loop_func();
        if (error) {
            DPRINTF("func %p error = %d\n", im->loop_func, error);
            break;
        }
    }
    return NULL;
}

static void *postcopy_incoming_umemd(void* unused)
{
    UMemBlock *block;
    UMemBlock *block_next;

    DPRINTF("umemd\n");
    qemu_thread_create(&umemd.mig_read_thread,
                       &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           &postcopy_incoming_umemd_mig_read_init,
                           &postcopy_incoming_umemd_mig_read_loop,},
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&umemd.mig_write_thread,
                       &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           NULL, &postcopy_incoming_umemd_mig_write_loop,},
                       QEMU_THREAD_JOINABLE);
    qemu_thread_create(&umemd.pipe_thread, &postcopy_incoming_umemd_thread,
                       &(IncomingThread) {
                           &postcopy_incoming_umemd_pipe_init,
                           &postcopy_incoming_umemd_pipe_loop,},
                       QEMU_THREAD_JOINABLE);

    qemu_thread_join(&umemd.mig_read_thread);
    qemu_thread_join(&umemd.mig_write_thread);
    qemu_thread_join(&umemd.pipe_thread);

    g_free(umemd.page_request);
    g_free(umemd.page_cached);
    g_free(umemd.target_pgoffs);
    g_free(umemd.phys_requested);
    g_free(umemd.phys_received);

    /* protect againt postcopy_incoming_ram_free() */
    qemu_mutex_lock_ramlist();
    QLIST_FOREACH_SAFE(block, &umemd.blocks, next, block_next) {
        UMem *umem = block->umem;
        umem_unmap_shmem(umem);
        umem_close(umem);
        umem_destroy(umem);
        QLIST_REMOVE(block, next);
        g_free(block);
    }
    qemu_mutex_unlock_ramlist();

    DPRINTF("umemd done\n");
    return NULL;
}