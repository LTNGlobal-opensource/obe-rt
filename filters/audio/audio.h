/*****************************************************************************
 * audio.h : OBE audio filter headers
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#ifndef OBE_FILTERS_AUDIO_H
#define OBE_FILTERS_AUDIO_H
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

typedef struct
{
    void* (*start_filter)( void *ptr );
} obe_aud_filter_func_t;

typedef struct
{
    obe_t *h;
    obe_filter_t *filter;
} obe_aud_filter_params_t;

extern const obe_aud_filter_func_t audio_filter;

#endif
