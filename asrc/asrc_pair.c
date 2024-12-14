/*
 * Copyright (c) 2012, Freescale Semiconductor, Inc. All rights reserved.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA. 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sound/compress_offload.h>

#include "asrc_pair.h"

#define DMA_MAX_BYTES   (32768)

#define LINEAR_RATE         (20)
#define LINEAR_PITCH_BITS   (16)
#define LINEAR_PITCH        (1 << LINEAR_PITCH_BITS)


struct asrc_task {
	struct snd_compr_task task;
	struct snd_compr_task_status status;
	void *bufin_start;
	void *bufout_start;
};

static uint32_t get_max_divider(uint32_t x, uint32_t y)
{
	uint32_t t;

	while(y != 0)
	{
		t = x % y;
		x = y;
		y = t;
	}

	return x;
}

static void calculate_num_den(asrc_pair *pair)
{
	uint32_t div;

	div = get_max_divider(pair->in_rate, pair->out_rate);
	pair->num = pair->in_rate / div;
	pair->den = pair->out_rate / div;
}

static int asrc_start_conversion(asrc_pair *pair)
{
	pair->is_converting = 1;
	return 0;
}

static int asrc_stop_conversion(asrc_pair *pair)
{
	pair->is_converting = 0;
	return 0;
}

static void get_dma_buffer_segments(unsigned int channels, uint32_t frames, uint32_t *seg_size, uint32_t *seg_num)
{
	uint32_t frame_bytes = frames << 1;
	uint32_t seg_bytes = frame_bytes;
	uint32_t alignment = channels << 1;
	int num = 1;

	while (seg_bytes > DMA_MAX_BYTES)
	{
		num++;
		seg_bytes = (frame_bytes + (alignment * num - 1)) / num;
		seg_bytes = seg_bytes - seg_bytes % alignment;
	}

	*seg_size = seg_bytes;
	*seg_num = num;
}

asrc_pair *asrc_pair_create(unsigned int channels, ssize_t in_period_frames,
        ssize_t out_period_frames, unsigned int in_rate, unsigned int out_rate, int type)
{
	struct snd_compr_params params;
	uint32_t dma_buffer_size;
	uint32_t buf_num;
	int fd;
	int err;
	asrc_pair *pair = NULL;
	struct asrc_task *asrc_task = NULL;
	char path[64];
	int i;

	for (i = 0; i < 10; i++) {
		memset(path, 0, 64);
		sprintf(path, "/dev/snd/comprC%uD0", i);
		if (access(path, F_OK) == 0)
			break;
	}
	if (i == 10) {
		fprintf(stderr, "no asrc sound card found\n");
		goto end;
	}

	fd = open(path, O_RDWR);
	if (fd < 0)
	{
		fprintf(stderr, "Unable to open device %s\n", path);
		goto end;
	}

	pair = calloc(1, sizeof(*pair));
	if (!pair)
		goto close_fd;

	pair->asrc_task = calloc(1,  sizeof(struct asrc_task));
	if (!pair->asrc_task)
		goto release_pair;

	asrc_task = pair->asrc_task;
	get_dma_buffer_segments(channels, in_period_frames, &dma_buffer_size, &buf_num);

	params.buffer.fragment_size = 4096;
	params.buffer.fragments = 1;
	params.codec.id = SND_AUDIOCODEC_PCM;
	params.codec.ch_in  = channels;
	params.codec.ch_out = channels;
	params.codec.format = SNDRV_PCM_FORMAT_S16_LE;
	params.codec.sample_rate = in_rate;
	params.codec.pcm_format = SNDRV_PCM_FORMAT_S16_LE;
	params.codec.options.src_d.out_sample_rate = out_rate;
	if ((err = ioctl(fd, SNDRV_COMPRESS_SET_PARAMS, &params)) < 0)
	{
		fprintf(stderr, "%s: set params failed\n", __func__);
		goto release_asrc_task;
	}

	if ((err = ioctl(fd, SNDRV_COMPRESS_TASK_CREATE, &asrc_task->task)) < 0) {
		fprintf(stderr, "%s: task create failed %d\n", __func__, err);
		goto release_asrc_task;
	}

	asrc_task->status.seqno = asrc_task->task.seqno;
	asrc_task->bufin_start = mmap(NULL,
				 512 * 1024, /* set by the driver */
				 PROT_READ | PROT_WRITE,
				 MAP_SHARED,
				 asrc_task->task.input_fd,
				 0);
	if (asrc_task->bufin_start == MAP_FAILED) {
		fprintf(stderr, "MMAP CAP err\n");
		goto map_err;
	}
	asrc_task->bufout_start = mmap(NULL,
				  512 * 1024, /* set by the driver */
				  PROT_READ | PROT_WRITE,
				  MAP_SHARED,
				  asrc_task->task.output_fd,
				  0);
	if (asrc_task->bufout_start == MAP_FAILED) {
		fprintf(stderr, "MMAP OUT err\n");
		goto map_err;
	}

	pair->fd = fd;
	pair->type = type;
	pair->channels = channels;
	pair->in_rate = in_rate;
	pair->out_rate = out_rate;
	pair->in_period_frames = in_period_frames;
	pair->out_period_frames = out_period_frames;
	pair->buf_size = dma_buffer_size;
	calculate_num_den(pair);
    
	goto end;

map_err:
	if ((err = ioctl(fd, SNDRV_COMPRESS_TASK_FREE, &asrc_task->task.seqno)) < 0)
		fprintf(stderr, "%s: task free failed %d\n", __func__, err);
release_asrc_task:
	free(pair->asrc_task);
release_pair:
	free(pair);
close_fd:
	close(fd);

end:
	return pair;
}

void asrc_pair_destroy(asrc_pair *pair)
{
	struct asrc_task *asrc_task = NULL;
	int err;

	asrc_stop_conversion(pair);
	asrc_task = pair->asrc_task;
	if ((err = ioctl(pair->fd, SNDRV_COMPRESS_TASK_FREE, &asrc_task->task.seqno)) < 0)
		fprintf(stderr, "%s: task free failed %d\n", __func__, err);

	close(pair->fd);
	free(pair->asrc_task);
	free(pair);
}

void asrc_pair_get_ratio(asrc_pair *pair, uint32_t *num, uint32_t *den)
{
	*num = pair->num;
	*den = pair->den;
}

int asrc_pair_set_rate(asrc_pair *pair, ssize_t in_period_frames,
        ssize_t out_period_frames, unsigned int in_rate, unsigned int out_rate)
{
	struct snd_compr_params params;
	uint32_t dma_buffer_size;
	uint32_t buf_num;
	int is_converting;
	int err;

	if (in_rate == pair->in_rate &&
	    out_rate == pair->out_rate &&
	    in_period_frames == pair->in_period_frames &&
	    out_period_frames == pair->out_period_frames)
		return 0;

	is_converting = pair->is_converting;
	asrc_stop_conversion(pair);

	get_dma_buffer_segments(pair->channels, in_period_frames, &dma_buffer_size, &buf_num);

	params.buffer.fragment_size = 4096;
	params.buffer.fragments = 1;
	params.codec.id = SND_AUDIOCODEC_PCM;
	params.codec.ch_in  = pair->channels;
	params.codec.ch_out = pair->channels;
	params.codec.format = SNDRV_PCM_FORMAT_S16_LE;
	params.codec.sample_rate = in_rate;
	params.codec.pcm_format = SNDRV_PCM_FORMAT_S16_LE;
	params.codec.options.src_d.out_sample_rate = out_rate;
	if ((err = ioctl(pair->fd, SNDRV_COMPRESS_SET_PARAMS, &params)) < 0) {
		fprintf(stderr, "%s: set params failed\n", __func__);
	} else {
		pair->buf_size = dma_buffer_size;
		pair->buf_num = buf_num;
		pair->in_rate = in_rate;
		pair->out_rate = out_rate;
		pair->in_period_frames = in_period_frames;
		pair->out_period_frames = out_period_frames;
		calculate_num_den(pair);
	}

	if (is_converting)
		asrc_start_conversion(pair);

	return err;
}

void asrc_pair_reset(asrc_pair *pair)
{
}

static void linear_pad_s16(asrc_pair *pair, int16_t *samples, int frames)
{
	unsigned int ch = pair->channels;
	unsigned int c;
	int i;

	int src_frames = frames * LINEAR_RATE;
	int dst_frames = frames * (LINEAR_RATE + 1);
	int16_t *src, *s, *d;
	int32_t pos;
	int32_t step = LINEAR_PITCH * (src_frames - 1) / (dst_frames - 1);

	src = (int16_t *) malloc((dst_frames << 1) * ch);

	/* first, copy samples to src buffer */
	memcpy(src, samples, (src_frames << 1) * ch);

	for (c = 0; c < ch; c++)
	{
		pos = 0;
		s = src + c;
		d = samples + c;
		for (i = 0; i < dst_frames; i++)
		{
			*d = ((LINEAR_PITCH - pos) * (*s) + pos * (*(s + ch))) >> LINEAR_PITCH_BITS;
			d += ch;
			pos += step;
			if (pos >= LINEAR_PITCH)
			{
				pos -= LINEAR_PITCH;
				s += ch;
			}
		}
	}

	free(src);
}

void asrc_pair_convert_s16(asrc_pair *pair, const int16_t *src, unsigned int src_frames,
        int16_t *dst, unsigned int dst_frames)
{
	struct asrc_task *asrc_task = NULL;
	unsigned int src_left = src_frames << 1;
	unsigned int dst_left = dst_frames << 1;
	char *s = (void *)src;
	char *d = (void *)dst;
	unsigned int in_len;
	int frames;
	int16_t *samples;

	asrc_start_conversion(pair);
	asrc_task = pair->asrc_task;

	while (src_left > 0)
	{
		if (src_left > pair->buf_size) {
			in_len = pair->buf_size;
		} else {
			in_len = src_left;
		}

		memcpy(asrc_task->bufin_start, s, in_len);
		asrc_task->task.input_size = in_len;

		if (ioctl(pair->fd, SNDRV_COMPRESS_TASK_START, &asrc_task->task) < 0) {
			fprintf(stderr, "task start failed\n");
			goto end_err;
		}

		if (ioctl(pair->fd, SNDRV_COMPRESS_TASK_STOP, &asrc_task->task.seqno) < 0) {
			fprintf(stderr, "task stop failed\n");
			goto end_err;
		}

                if (ioctl(pair->fd, SNDRV_COMPRESS_TASK_STATUS, &asrc_task->status) < 0) {
                        printf("task status FAILED\n");
                        goto end_err;
                }

		s += in_len;
		src_left -= in_len;

		if (dst_left < asrc_task->status.output_size) {
			memcpy(d, asrc_task->bufout_start, dst_left);
			dst_left = 0;
		} else {
			memcpy(d, asrc_task->bufout_start, asrc_task->status.output_size);
			d += asrc_task->status.output_size;
			dst_left -= asrc_task->status.output_size;
		}
	}

	if (dst_left > 0)
	{
		frames = (dst_left >> 1) / pair->channels;
		/* we use LINEAR_RATE * N frames to generate (LINEAR_RATE+1)*N frames */
		samples = (int16_t *)d - frames * LINEAR_RATE * pair->channels;
		if (frames > 0 && samples >= dst)
		{
			/* try insert samples by linear alg */
			linear_pad_s16(pair, samples, frames);
		}
	}
end_err:
    return;
}
