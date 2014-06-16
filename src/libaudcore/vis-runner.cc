/*
 * vis_runner.c
 * Copyright 2009-2012 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "internal.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "list.h"
#include "mainloop.h"
#include "output.h"

#define INTERVAL 30 /* milliseconds */
#define FRAMES_PER_NODE 512

struct VisNode : public ListNode
{
    explicit VisNode (int channels) :
        channels (channels),
        data (new float[channels * FRAMES_PER_NODE]) {}

    ~VisNode ()
        { delete[] data; }

    const int channels;
    int time;
    float * data;
};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool enabled = false;
static bool playing = false, paused = false, active = false;
static VisNode * current_node = nullptr;
static int current_frames;
static List<VisNode> vis_list;
static List<VisNode> vis_pool;
static QueuedFunc queued_clear;
static QueuedFunc send_timer;

static void send_audio (void * unused)
{
    /* call before locking mutex to avoid deadlock */
    int outputted = output_get_raw_time ();

    pthread_mutex_lock (& mutex);

    if (! send_timer.running ())
    {
        pthread_mutex_unlock (& mutex);
        return;
    }

    VisNode * node = nullptr;
    VisNode * next;

    while ((next = vis_list.head ()))
    {
        /* If we are considering a node, stop searching and use it if it is the
         * most recent (that is, the next one is in the future).  Otherwise,
         * consider the next node if it is not in the future by more than the
         * length of an interval. */
        if (next->time > outputted + (node ? 0 : INTERVAL))
            break;

        if (node)
            vis_pool.prepend (node);

        node = next;
        vis_list.remove (node);
    }

    pthread_mutex_unlock (& mutex);

    if (! node)
        return;

    vis_send_audio (node->data, node->channels);

    pthread_mutex_lock (& mutex);
    vis_pool.prepend (node);
    pthread_mutex_unlock (& mutex);
}

static void send_clear (void * unused)
{
    vis_send_clear ();
}

static void flush_locked (void)
{
    delete current_node;
    current_node = nullptr;

    vis_list.clear ();
    vis_pool.clear ();

    queued_clear.queue (send_clear, nullptr);
}

void vis_runner_flush (void)
{
    pthread_mutex_lock (& mutex);
    flush_locked ();
    pthread_mutex_unlock (& mutex);
}

static void start_stop_locked (bool new_playing, bool new_paused)
{
    playing = new_playing;
    paused = new_paused;
    active = playing && enabled;

    send_timer.stop ();
    queued_clear.stop ();

    if (! active)
        flush_locked ();
    else if (! paused)
        send_timer.start (INTERVAL, send_audio, nullptr);
}

void vis_runner_start_stop (bool new_playing, bool new_paused)
{
    pthread_mutex_lock (& mutex);
    start_stop_locked (new_playing, new_paused);
    pthread_mutex_unlock (& mutex);
}

void vis_runner_pass_audio (int time, float * data, int samples, int
 channels, int rate)
{
    pthread_mutex_lock (& mutex);

    if (! active)
    {
        pthread_mutex_unlock (& mutex);
        return;
    }

    /* We can build a single node from multiple calls; we can also build
     * multiple nodes from the same call.  If current_node is present, it was
     * partly built in the last call and needs to be finished. */

    int at = 0;

    while (1)
    {
        if (current_node)
            assert (current_node->channels == channels);
        else
        {
            int node_time = time;

            /* There is no partly-built node, so start a new one.  Normally
             * there will be nodes in the queue already; if so, we want to copy
             * audio data from the signal starting at 30 milliseconds after the
             * beginning of the most recent node.  If there are no nodes in the
             * queue, we are at the beginning of the song or had an underrun,
             * and we want to copy the earliest audio data we have. */

            VisNode * tail = vis_list.tail ();
            if (tail)
                node_time = tail->time + INTERVAL;

            at = channels * (int) ((int64_t) (node_time - time) * rate / 1000);

            if (at < 0)
                at = 0;
            if (at >= samples)
                break;

            current_node = vis_pool.head ();

            if (current_node)
            {
                assert (current_node->channels == channels);
                vis_pool.remove (current_node);
            }
            else
                current_node = new VisNode (channels);

            current_node->time = node_time;
            current_frames = 0;
        }

        /* Copy as much data as we can, limited by how much we have and how much
         * space is left in the node.  If we cannot fill the node, we return and
         * wait for more data to be passed in the next call.  If we do fill the
         * node, we loop and start building a new one. */

        int copy = MIN (samples - at, channels * (FRAMES_PER_NODE - current_frames));
        memcpy (current_node->data + channels * current_frames, data + at, sizeof (float) * copy);
        current_frames += copy / channels;

        if (current_frames < FRAMES_PER_NODE)
            break;

        vis_list.append (current_node);
        current_node = nullptr;
    }

    pthread_mutex_unlock (& mutex);
}

void vis_runner_enable (bool enable)
{
    pthread_mutex_lock (& mutex);
    enabled = enable;
    start_stop_locked (playing, paused);
    pthread_mutex_unlock (& mutex);
}
