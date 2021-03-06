
/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <abt.h>
#include <abt-snoozer.h>
// =e
#include <sys/epoll.h>

#include "abt-io.h"

struct abt_io_instance
{
    ABT_pool progress_pool;
    ABT_xstream *progress_xstreams;
    int num_xstreams;
};

struct abt_io_op
{
    ABT_eventual e;
    void *state;
    void (*free_fn)(void*);
};

abt_io_instance_id abt_io_init(int backing_thread_count)
{
    struct abt_io_instance *aid;
    ABT_pool pool;
    ABT_xstream self_xstream;
    ABT_xstream *progress_xstreams = NULL;
    int ret;

    if (backing_thread_count < 0) return NULL;

    aid = malloc(sizeof(*aid));
    if (aid == NULL) return ABT_IO_INSTANCE_NULL;

    if (backing_thread_count == 0) {
        aid->num_xstreams = 0;
        ret = ABT_xstream_self(&self_xstream);
        if (ret != ABT_SUCCESS) { free(aid); return ABT_IO_INSTANCE_NULL; }
        ret = ABT_xstream_get_main_pools(self_xstream, 1, &pool);
        if (ret != ABT_SUCCESS) { free(aid); return ABT_IO_INSTANCE_NULL; }
    }
    else {
        aid->num_xstreams = backing_thread_count;
        progress_xstreams = malloc(
                backing_thread_count * sizeof(*progress_xstreams));
        if (progress_xstreams == NULL) {
            free(aid);
            return ABT_IO_INSTANCE_NULL;
        }
        ret = ABT_snoozer_xstream_create(backing_thread_count, &pool,
                progress_xstreams);
        if (ret != ABT_SUCCESS) {
            free(aid);
            free(progress_xstreams);
            return ABT_IO_INSTANCE_NULL;
        }
    }

    aid->progress_pool = pool;
    aid->progress_xstreams = progress_xstreams;

    return aid;
}

abt_io_instance_id abt_io_init_pool(ABT_pool progress_pool)
{
    struct abt_io_instance *aid;

    aid = malloc(sizeof(*aid));
    if(!aid) return(ABT_IO_INSTANCE_NULL);

    aid->progress_pool = progress_pool;
    aid->progress_xstreams = NULL;
    aid->num_xstreams = 0;

    return aid;
}

void abt_io_finalize(abt_io_instance_id aid)
{
    int i;

    if (aid->num_xstreams) {
        for (i = 0; i < aid->num_xstreams; i++) {
            ABT_xstream_join(aid->progress_xstreams[i]);
            ABT_xstream_free(&aid->progress_xstreams[i]);
        }
        free(aid->progress_xstreams);
        // pool gets implicitly freed
    }

    free(aid);
}

struct abt_io_open_state
{
    int *ret;
    const char *pathname;
    int flags;
    mode_t mode;
    ABT_eventual eventual;
};

static void abt_io_open_fn(void *foo)
{
    struct abt_io_open_state *state = foo;

    *state->ret = open(state->pathname, state->flags, state->mode);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_open(ABT_pool pool, abt_io_op_t *op, const char* pathname, int flags, mode_t mode, int *ret)
{
    struct abt_io_open_state state;
    struct abt_io_open_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->pathname = pathname;
    pstate->flags = flags;
    pstate->mode = mode;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_open_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

int abt_io_open(abt_io_instance_id aid, const char* pathname, int flags, mode_t mode)
{
    int ret;
    issue_open(aid->progress_pool, NULL, pathname, flags, mode, &ret);
    return ret;
}

abt_io_op_t* abt_io_open_nb(abt_io_instance_id aid, const char* pathname, int flags, mode_t mode, int *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_open(aid->progress_pool, op, pathname, flags, mode, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}

struct abt_io_pread_state
{
    ssize_t *ret;
    int fd;
    void *buf;
    size_t count;
    off_t offset;
    ABT_eventual eventual;
};

static void abt_io_pread_fn(void *foo)
{
    struct abt_io_pread_state *state = foo;

    *state->ret = pread(state->fd, state->buf, state->count, state->offset);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_pread(ABT_pool pool, abt_io_op_t *op, int fd, void *buf,
        size_t count, off_t offset, ssize_t *ret)
{
    struct abt_io_pread_state state;
    struct abt_io_pread_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->fd = fd;
    pstate->buf = buf;
    pstate->count = count;
    pstate->offset = offset;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_pread_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

ssize_t abt_io_pread(abt_io_instance_id aid, int fd, void *buf,
        size_t count, off_t offset)
{
    ssize_t ret = -1;
    issue_pread(aid->progress_pool, NULL, fd, buf, count, offset, &ret);
    return ret;
}

abt_io_op_t* abt_io_pread_nb(abt_io_instance_id aid, int fd, void *buf,
        size_t count, off_t offset, ssize_t *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_pread(aid->progress_pool, op, fd, buf, count, offset, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}


struct abt_io_pwrite_state
{
    ssize_t *ret;
    int fd;
    const void *buf;
    size_t count;
    off_t offset;
    ABT_eventual eventual;
};

static void abt_io_pwrite_fn(void *foo)
{
    struct abt_io_pwrite_state *state = foo;

    *state->ret = pwrite(state->fd, state->buf, state->count, state->offset);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_pwrite(ABT_pool pool, abt_io_op_t *op, int fd, const void *buf,
        size_t count, off_t offset, ssize_t *ret)
{
    struct abt_io_pwrite_state state;
    struct abt_io_pwrite_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->fd = fd;
    pstate->buf = buf;
    pstate->count = count;
    pstate->offset = offset;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_pwrite_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

ssize_t abt_io_pwrite(abt_io_instance_id aid, int fd, const void *buf,
        size_t count, off_t offset)
{
    ssize_t ret = -1;
    issue_pwrite(aid->progress_pool, NULL, fd, buf, count, offset, &ret);
    return ret;
}

abt_io_op_t* abt_io_pwrite_nb(abt_io_instance_id aid, int fd, const void *buf,
        size_t count, off_t offset, ssize_t *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_pwrite(aid->progress_pool, op, fd, buf, count, offset, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}

struct abt_io_mkostemp_state
{
    int *ret;
    char *template;
    int flags;
    ABT_eventual eventual;
};

static void abt_io_mkostemp_fn(void *foo)
{
    struct abt_io_mkostemp_state *state = foo;

#ifdef HAVE_MKOSTEMP
    *state->ret = mkostemp(state->template, state->flags);
#else
    *state->ret = mkstemp(state->template);
#endif
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_mkostemp(ABT_pool pool, abt_io_op_t *op, char* template, int flags, int *ret)
{
    struct abt_io_mkostemp_state state;
    struct abt_io_mkostemp_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->template = template;
    pstate->flags = flags;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_mkostemp_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

int abt_io_mkostemp(abt_io_instance_id aid, char *template, int flags)
{
    int ret = -1;
    issue_mkostemp(aid->progress_pool, NULL, template, flags, &ret);
    return ret;
}

abt_io_op_t* abt_io_mkostemp_nb(abt_io_instance_id aid, char *template, int flags, int *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_mkostemp(aid->progress_pool, op, template, flags, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}

struct abt_io_unlink_state
{
    int *ret;
    const char *pathname;
    ABT_eventual eventual;
};

static void abt_io_unlink_fn(void *foo)
{
    struct abt_io_unlink_state *state = foo;

    *state->ret = unlink(state->pathname);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_unlink(ABT_pool pool, abt_io_op_t *op, const char* pathname, int *ret)
{
    struct abt_io_unlink_state state;
    struct abt_io_unlink_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->pathname = pathname;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_unlink_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}


int abt_io_unlink(abt_io_instance_id aid, const char *pathname)
{
    int ret = -1;
    issue_unlink(aid->progress_pool, NULL, pathname, &ret);
    return ret;
}

abt_io_op_t* abt_io_unlink_nb(abt_io_instance_id aid, const char *pathname, int *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_unlink(aid->progress_pool, op, pathname, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}

struct abt_io_close_state
{
    int *ret;
    int fd;
    ABT_eventual eventual;
};

static void abt_io_close_fn(void *foo)
{
    struct abt_io_close_state *state = foo;

    *state->ret = close(state->fd);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_close(ABT_pool pool, abt_io_op_t *op, int fd, int *ret)
{
    struct abt_io_close_state state;
    struct abt_io_close_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->fd = fd;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_close_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

int abt_io_close(abt_io_instance_id aid, int fd)
{
    int ret = -1;
    issue_close(aid->progress_pool, NULL, fd, &ret);
    return ret;
}

abt_io_op_t* abt_io_close_nb(abt_io_instance_id aid, int fd, int *ret)
{
    abt_io_op_t *op;
    int iret;

    op = malloc(sizeof(*op));
    if (op == NULL) return NULL;

    iret = issue_close(aid->progress_pool, op, fd, ret);
    if (iret != 0) { free(op); return NULL; }
    else return op;
}

int abt_io_op_wait(abt_io_op_t* op)
{
    int ret;

    ret = ABT_eventual_wait(op->e, NULL);
    return ret == ABT_SUCCESS ? 0 : -1;
}

void abt_io_op_free(abt_io_op_t* op)
{
    ABT_eventual_free(&op->e);
    op->free_fn(op->state);
    free(op);
}


// =e
// READ syscall
struct abt_io_read_state
{
    ssize_t *ret;
    int fd;
    void *buf;
    size_t count;
    ABT_eventual eventual;
};

static void abt_io_read_fn(void *foo)
{
    struct abt_io_read_state *state = foo;

    *state->ret = read(state->fd, state->buf, state->count);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
}

static int issue_read(ABT_pool pool, abt_io_op_t *op, int fd, void *buf,
        size_t count, ssize_t *ret)
{
    struct abt_io_read_state state;
    struct abt_io_read_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->fd = fd;
    pstate->buf = buf;
    pstate->count = count;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_read_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

ssize_t abt_io_read(abt_io_instance_id aid, int fd, void *buf, size_t count)
{
    ssize_t ret = -1;
    issue_read(aid->progress_pool, NULL, fd, buf, count, &ret);
    return ret;
}


// WRITE system call

struct abt_io_write_state
{
    ssize_t *ret;
    int fd;
    const void *buf;
    size_t count;
    ABT_eventual eventual;
};

static void abt_io_write_fn(void *foo)
{
    struct abt_io_write_state *state = foo;

    *state->ret = write(state->fd, state->buf, state->count);
    if(*state->ret < 0)
        *state->ret = -errno;

    ABT_eventual_set(state->eventual, NULL, 0);
    return;
};

static int issue_write(ABT_pool pool, abt_io_op_t *op, int fd, const void *buf,
        size_t count, ssize_t *ret)
{
    struct abt_io_write_state state;
    struct abt_io_write_state *pstate = NULL;
    int rc;

    if (op == NULL) pstate = &state;
    else
    {
        pstate = malloc(sizeof(*pstate));
        if (pstate == NULL) { *ret = -ENOMEM; goto err; }
    }

    *ret = -ENOSYS;
    pstate->ret = ret;
    pstate->fd = fd;
    pstate->buf = buf;
    pstate->count = count;
    pstate->eventual = NULL;
    rc = ABT_eventual_create(0, &pstate->eventual);
    if (rc != ABT_SUCCESS) { *ret = -ENOMEM; goto err; }

    if (op != NULL) op->e = pstate->eventual;

    rc = ABT_task_create(pool, abt_io_write_fn, pstate, NULL);
    if(rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }

    if (op == NULL) {
        rc = ABT_eventual_wait(pstate->eventual, NULL);
        // what error should we use here?
        if (rc != ABT_SUCCESS) { *ret = -EINVAL; goto err; }
    }
    else {
        op->e = pstate->eventual;
        op->state = pstate;
        op->free_fn = free;
    }

    return 0;
err:
    if (pstate->eventual != NULL) ABT_eventual_free(&pstate->eventual);
    if (pstate != NULL && op != NULL) free(pstate);
    return -1;
}

ssize_t abt_io_write(abt_io_instance_id aid, int fd, const void *buf,
        size_t count)
{
    ssize_t ret = -1;
    issue_write(aid->progress_pool, NULL, fd, buf, count, &ret);
    return ret;
}

////////////////////////////////////////////


void event_listener(void* foo)
{
    int epfd = *(int*) foo;
    int numOpenFDs = 1, ready, j;
    int max_events = 1000;
    struct epoll_event evlist[max_events];  // TODO: efficient memory allocation
    //printf(" event listener: epfd %d \n", epfd);
    while(numOpenFDs > 0) {
        //printf("%s\n", "about to epoll_wait");
        ready = epoll_wait(epfd, evlist, max_events, -1);
        if(ready == -1) {
            if(errno == EINTR)
                continue;
            else if(errno == EINVAL)
            {
                printf("epoll_wait invalid fd");
                exit(-1);
            }
            else if (errno == EBADF){
                printf("epoll_wait bad fd");
                exit(-1);
            }
            else
            {
                perror("epoll_wait");
                exit(-1);
            }
        }
        //printf("ready: %d\n", ready);
        struct thread_args *ta;
        for(j=0; j< ready;j++){
            ta = (struct thread_args*) evlist[j].data.ptr;
            /*printf(" fd=%d; events: %s%s%s\n", ta->fd,
                (evlist[j].events & EPOLLIN) ? "EPOLLIN " : "",
                (evlist[j].events & EPOLLHUP) ? "EPOLLHUP " : "",
                (evlist[j].events & EPOLLERR) ? "EPOLLERR " : "");
                */
            if(evlist[j].events & EPOLLIN){
                ABT_cond_signal(ta->cond);
            }
            else if(evlist[j].events & (EPOLLHUP | EPOLLERR)){
                printf("    closing fd %d \n", ta->fd);
                if(close(ta->fd) == -1)
                    printf("close error");
                numOpenFDs--;
            }
        }
    }
}

int abt_io_socket_initialize(int events)
{
    ABT_pool g_pool;
    ABT_xstream xstream;
    ABT_thread listener;
    ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC, ABT_TRUE, &g_pool);
    ABT_xstream_create_basic(ABT_SCHED_DEFAULT, 1, &g_pool, ABT_SCHED_CONFIG_NULL, &xstream);
    ABT_xstream_start(xstream);
    int* epfd = (int*) malloc(sizeof(int));
    *epfd = epoll_create(1);
    //printf(" epoll created fd %d \n", epfd);
    ABT_thread_create(g_pool, event_listener, epfd, ABT_THREAD_ATTR_NULL, listener);
    return *epfd;
}

io_instance_t* abt_io_register_thread(struct thread_args* ta)
{
    // register to epoll handler
    io_instance_t* instance = (io_instance_t*) malloc(sizeof(instance));
    struct epoll_event ev;
    ABT_mutex mutex;
    ABT_cond cond;
    mutex = (ABT_mutex) malloc(sizeof(mutex));
    cond = (ABT_cond) malloc(sizeof(cond));
    ABT_cond_create(&cond);
    //printf("Lib: register: client fd = %d\n", ta->fd);
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)ta;
    //printf("Lib: register: epoll fd = %d\n", ta->epfd);
    //printf("Lib: register: client 2fd = %d\n", ta->fd);
    ta->cond = cond;
    if(epoll_ctl(ta->epfd, EPOLL_CTL_ADD, ta->fd, &ev) == -1){
        return NULL;
    }
    instance->epfd = ta->epfd;
    instance->mutex = mutex;
    instance->cond = cond;
    return instance;
}

ssize_t abt_io_epoll_read(io_instance_t* instance, int fd, const void *buf, size_t count)
{
    // ABT_eventual_wait()
    // read()
    // return
    ssize_t ret = -1;
    // how to attain mutex and cond??
    //printf("about to cond_wait %d", fd);
    ABT_cond_wait(instance->cond, instance->mutex);
    int r = read(fd, (char *)buf, count);
    if(r < 0)
        ret = -errno;
    else
        ret = r;
    return ret;
}

//
//
