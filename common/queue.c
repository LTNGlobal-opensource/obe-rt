#include "queue.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/** Add/Remove from queues */
void obe_init_queue(obe_queue_t *queue, char *name)
{
    pthread_mutex_init( &queue->mutex, NULL );
    pthread_cond_init( &queue->in_cv, NULL );
    pthread_cond_init( &queue->out_cv, NULL );
    snprintf(queue->name, sizeof(queue->name), "%s", name);
}

void obe_destroy_queue( obe_queue_t *queue )
{
    free( queue->queue );

    pthread_mutex_unlock( &queue->mutex );
    pthread_mutex_destroy( &queue->mutex );
    pthread_cond_destroy( &queue->in_cv );
    pthread_cond_destroy( &queue->out_cv );
}

int add_to_queue( obe_queue_t *queue, void *item )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size+1) );
    if( !tmp )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    queue->queue = tmp;
    queue->queue[queue->size++] = item;

    pthread_cond_signal( &queue->in_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

int remove_from_queue_without_lock(obe_queue_t *queue)
{
    void **tmp;

    if (queue->size > 1)
        memmove(&queue->queue[0], &queue->queue[1], sizeof(*queue->queue) * (queue->size-1));
    tmp = realloc(queue->queue, sizeof(*queue->queue) * (queue->size-1));
    queue->size--;
    if (!tmp && queue->size) {
        syslog(LOG_ERR, "Malloc failed\n");
        return -1;
    }
    queue->queue = tmp;

    return 0;
}

int remove_from_queue( obe_queue_t *queue )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    if( queue->size > 1 )
        memmove( &queue->queue[0], &queue->queue[1], sizeof(*queue->queue) * (queue->size-1) );
    tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size-1) );
    queue->size--;
    if( !tmp && queue->size )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return -1;
    }
    queue->queue = tmp;

    pthread_cond_signal( &queue->out_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

int remove_item_from_queue( obe_queue_t *queue, void *item )
{
    void **tmp;

    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        if( queue->queue[i] == item )
        {
            memmove( &queue->queue[i], &queue->queue[i+1], sizeof(*queue->queue) * (queue->size-1-i) );
            tmp = realloc( queue->queue, sizeof(*queue->queue) * (queue->size-1) );
            queue->size--;
            if( !tmp && queue->size )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            queue->queue = tmp;
            break;
        }
    }

    pthread_cond_signal( &queue->out_cv );
    pthread_mutex_unlock( &queue->mutex );

    return 0;
}

