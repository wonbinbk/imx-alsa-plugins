/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2022 NXP
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/global.h>

#include <imx-mm/audio-codec/swpdm/imx-swpdm.h>

#define PLUG_NAME                               cicFilter

#define str(x)                                #x
#define ARRAY_SIZE(ary)                       (sizeof(ary)/sizeof(ary[0]))

#define MAX_IN_BUFFER_SIZE                    8192
#define MAX_IN_PERIOD_SIZE                    4096

#define PDM_CHANNELS                          4
#define FORMAT                                4
#define MAX_PCM_CHANNELS                      4
#define MIN_PCM_CHANNELS                      1
#define MAX_PERIODS                           8
#define DIV_BY_8(x)                           ((x) >> 3)

typedef struct snd_pcm_cic_filter {
	/* internal plug elements */
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	snd_pcm_hw_params_t *slave_params;
	snd_pcm_uframes_t ptr;
	snd_pcm_uframes_t boundary;
	/* afe elements */
	afe_t *afe;
	cic_t type;
	float gain;
	unsigned out_samples_per_channel;
	snd_pcm_uframes_t out_period_size;
	snd_pcm_uframes_t in_period_size;
	/* external plug elements */
	int inval_iterations;
	int iterations;
	unsigned int delay;
	unsigned int OSR;
	bool bit_reverse;
}snd_pcm_cic_filter_t;

static int cic_start(snd_pcm_ioplug_t *io);
static int cic_stop(snd_pcm_ioplug_t *io);
static snd_pcm_sframes_t cic_pointer(snd_pcm_ioplug_t *io);
static snd_pcm_sframes_t cic_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
                                      snd_pcm_uframes_t offset, snd_pcm_uframes_t size);
static int cic_close(snd_pcm_ioplug_t *io);
static int cic_hw(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params);
static int cic_hw_free(snd_pcm_ioplug_t *io);
static int cic_sw(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params);
static int cic_prepare(snd_pcm_ioplug_t *io);
static int cic_poll_descriptors_count(snd_pcm_ioplug_t *io);
static int cic_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space);
static int cic_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents);
static void cic_dump(snd_pcm_ioplug_t *io, snd_output_t *out);
static inline int parse_struct(snd_config_t **conf, const char **devname, snd_pcm_cic_filter_t *cic);
static void destroy(snd_pcm_cic_filter_t **cic);
static int constrains(snd_pcm_ioplug_t *io);
static inline int compute_delay(snd_pcm_hw_params_t *params, snd_pcm_cic_filter_t *cic);

static const snd_pcm_ioplug_callback_t cic_funcs  = {
	.start = cic_start,
	.stop = cic_stop,
	.pointer = cic_pointer,
	.transfer = cic_transfer,
	.close = cic_close,
	.hw_params = cic_hw,
	.hw_free = cic_hw_free,
	.sw_params = cic_sw,
	.prepare = cic_prepare,
	.poll_descriptors_count = cic_poll_descriptors_count,
	.poll_descriptors = cic_poll_descriptors,
	.poll_revents = cic_poll_revents,
	.dump = cic_dump
};

/* Trigger slave  PCM */
static inline int compute_delay(snd_pcm_hw_params_t *params, snd_pcm_cic_filter_t *cic) {
	unsigned int period_time;
	int err, dir;
	double n;

	err = snd_pcm_hw_params_get_period_time(params, &period_time, &dir);
	if(err < 0)
		return err;

	n = (double)((double)cic->delay / (double)period_time);
	cic->inval_iterations = (int)ceil(n);
	cic->iterations = cic->inval_iterations;

	return err;
}

static int cic_start(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;

	if(snd_pcm_state(cic->slave) == SND_PCM_STATE_RUNNING)
		return 0;

	return snd_pcm_start(cic->slave);
}

static int cic_stop(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	snd_pcm_drop(cic->slave);
	return 0;
}

static snd_pcm_sframes_t cic_pointer(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	snd_pcm_sframes_t avail;

	avail = snd_pcm_avail(cic->slave);
	if(avail < 0) {
		cic->inval_iterations = cic->iterations;
		return avail;
	}

	avail = avail * cic->out_period_size / cic->in_period_size;

	avail = (cic->ptr + avail) % cic->boundary;

	return avail;
}

unsigned int reverse(unsigned int x) {
	x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
	x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
	x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
	x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));
	return((x >> 16) | (x << 16));
}

int reverse_buffer(unsigned int *p, int size)
{
	int i;
	unsigned int *dst = p;

	for (i = 0; i < size; i++) {
		*dst++ = reverse(*p++);
		*dst++ = reverse(*p++);
		*dst++ = reverse(*p++);
		*dst++ = reverse(*p++);
	}

	return 0;
}

static snd_pcm_sframes_t cic_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, 
				      snd_pcm_uframes_t offset, snd_pcm_uframes_t size) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	unsigned int *pcm_samples;
	unsigned int *pdm_samples;
	snd_pcm_sframes_t slave_frames;
	unsigned int j;

	/* PCM output */
	pcm_samples = (unsigned int *)(areas->addr + DIV_BY_8(areas->first));
	pcm_samples = (void *)pcm_samples + offset * DIV_BY_8(areas->step);

	/* PDM output */
	pdm_samples = (unsigned int *)cic->afe->outputBuffer;

	/*Read from the slave and saved to the afe input buffer.*/
	slave_frames = snd_pcm_mmap_readi(cic->slave, cic->afe->inputBuffer, cic->in_period_size);
	if(slave_frames < 0)
		return slave_frames;

	if (cic->bit_reverse)
		reverse_buffer((unsigned int *)cic->afe->inputBuffer, cic->in_period_size);

	/*pdm2pcm*/
	processAfeCic(cic->afe);
	/*Save to the app buffer.*/
	if(cic->inval_iterations > 0) {
		cic->inval_iterations--;
		size = 0;
	} else {
		/*This work but the porcentage table with the -vv parameters doesnt work.*/
		if (io->channels == PDM_CHANNELS) {
			memcpy(pcm_samples, cic->afe->outputBuffer, cic->out_period_size * PDM_CHANNELS * FORMAT);
		} else if (io->channels == 1) {
			for(j=0; j < io->period_size; j++) {
					*pcm_samples++ = *pdm_samples++;
					pdm_samples = (void *)pdm_samples + 12;
			}
		}else if (io->channels == 2) {
			for(j=0; j < io->period_size; j++) {
					*pcm_samples++ = *pdm_samples++;
					*pcm_samples++ = *pdm_samples++;
					pdm_samples = (void *)pdm_samples + 8;
			}
		}else if (io->channels == 3) {
			for(j=0; j < io->period_size; j++) {
					*pcm_samples++ = *pdm_samples++;
					*pcm_samples++ = *pdm_samples++;
					*pcm_samples++ = *pdm_samples++;
					pdm_samples = (void *)pdm_samples + 4;
			}
		}
	}

	cic->ptr = cic->ptr + cic->out_period_size;
	cic->ptr %= cic->boundary;

	return size;
}

static int cic_close(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;

	destroy(&cic);
	return 0;
}

static int cic_hw(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	snd_pcm_format_t format;
	unsigned int rate, refine_rate;
	unsigned int samples_per_channel, periods;
	int err;
	int dir;

	/* Filter configuration */
	/* The Cic Decoder request to divide the samples by 16. */
	samples_per_channel = io->period_size / 16;

	/* refine the gain 1 ~ 101.0 */
	cic->gain = (1 + cic->gain) * (double)(1 << 30) / pow(cic->OSR/4, 5);

	/* Init the afe object */
	err = constructAfeCicDecoder(cic->type, cic->afe, cic->gain, samples_per_channel);
	if (err == false) {
		SNDERR("Fail to create AfeCicDecoder");
		return SWPDM_ERR;
	}

	/* These values are in frame size. */
	cic->in_period_size = cic->afe->inputBufferSizePerChannel;
	cic->out_period_size = cic->afe->outputBufferSizePerChannel;
	if(cic->out_period_size != io->period_size) {
		SNDERR("Mismatch on AfeCicDecoder output buffer size and User buffer size."
		" Set a power of two to the --period_size parameter.");
		return SWPDM_ERR;
	}

	if(cic->slave_params == NULL) {
		err = snd_pcm_hw_params_malloc(&cic->slave_params);
		if (err < 0)
			return err;
	}

	err = snd_pcm_hw_params_any(cic->slave, cic->slave_params);
	if (err < 0) {
		SNDERR("Broken configuration for capture: no configurations available: %s\n", snd_strerror(err));
		return err;
	}

	/* set the interleaved read/write format */
	err = snd_pcm_hw_params_set_access(cic->slave, cic->slave_params, SND_PCM_ACCESS_MMAP_INTERLEAVED );
	if (err < 0) {
		SNDERR("Access type not available for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* set channels */
	err = snd_pcm_hw_params_set_channels(cic->slave, cic->slave_params, PDM_CHANNELS);
	if (err < 0) {
		SNDERR("Unable to set numbers of channels: %s\n", snd_strerror(err));
		return err;
	}

	/* set stream rate */
	err = snd_pcm_hw_params_get_rate(params, &rate, &dir);
	if (err < 0) {
		SNDERR("unable to get device rate\n");
		return err;
	}

	switch (cic->OSR) {
	case 48:
		format = SND_PCM_FORMAT_DSD_U32_LE;
		refine_rate = rate * 3 / 2;
		break;
	case 64:
		format = SND_PCM_FORMAT_DSD_U32_LE;
		refine_rate = rate * 2;
		break;
	case 96:
		format = SND_PCM_FORMAT_DSD_U32_LE;
		refine_rate = rate * 3;
		break;
	case 128:
		format = SND_PCM_FORMAT_DSD_U32_LE;
		refine_rate = rate * 4;
		break;
	case 192:
		format = SND_PCM_FORMAT_DSD_U32_LE;
		refine_rate = rate * 6;
		break;
	default:
		SNDERR("Unsupported OSR: %d\n", cic->OSR);
		return -EINVAL;
	}

	if (refine_rate * snd_pcm_format_width(format) > 4800000) {
		SNDERR("max frequency can't exceed 4.8MHz\n");
		return -EINVAL;
	}
	cic->bit_reverse = false;
	err = snd_pcm_hw_params_test_format(cic->slave, cic->slave_params, SND_PCM_FORMAT_DSD_U32_LE);
	if (err < 0) {
		err = snd_pcm_hw_params_test_format(cic->slave, cic->slave_params, SND_PCM_FORMAT_DSD_U32_BE);
		if (err < 0) {
			SNDERR("Unsupported format DSD_U32_LE or DSD_U32_BE\n");
			return -EINVAL;
		} else {
			format = SND_PCM_FORMAT_DSD_U32_BE;
			cic->bit_reverse = true;
		}
	} else {
		format = SND_PCM_FORMAT_DSD_U32_LE;
	}

	/* set the sample format */
	err = snd_pcm_hw_params_set_format(cic->slave, cic->slave_params, format);
	if (err < 0) {
		SNDERR("Sample format not available for capture: %s\n", snd_strerror(err));
		return err;
	}

	err = snd_pcm_hw_params_set_rate(cic->slave, cic->slave_params, refine_rate, dir);
	if(err < 0) {
		SNDERR("Unable to set rate: %s\n", snd_strerror(err));
		return err;
	}

	/* set the buffer size */
	snd_pcm_hw_params_get_periods(params, &periods, &dir);
	err = snd_pcm_hw_params_set_buffer_size(cic->slave, cic->slave_params, cic->in_period_size * periods);
	if (err < 0) {
		SNDERR("Unable to set buffer size.\n");
		return err;
	}

	/* set period size */
	err = snd_pcm_hw_params_set_period_size(cic->slave, cic->slave_params, cic->in_period_size, 0);
	if (err < 0) {
		SNDERR("Unable to set period time\n");
		return err;
	}

	err = snd_pcm_hw_params(cic->slave, cic->slave_params);
	if(err < 0) {
		SNDERR("Couldnt set hw params\n");
		return err;
	}

	if(cic->delay != 0) {
		if(compute_delay(params, cic) < 0)
			SNDERR("WARNING: Unable to set requested delay");
	}

	return err;
}

static int cic_hw_free(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;

	free(cic->slave_params);
	cic->slave_params = NULL;
	snd_pcm_hw_free(cic->slave);
	cic->slave = NULL;
	return 0;
}

static int cic_sw(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	snd_pcm_sw_params_t *sparams;
	int err;

	err = snd_pcm_sw_params_malloc(&sparams);
	if(err < 0) {
		//todo: move to hw_free;
		return err;
	}

	/* get the current swparams */
	err = snd_pcm_sw_params_current(cic->slave, sparams);
	if (err < 0) {
		SNDERR("Unable to determine current swparams for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* start the transfer when the buffer has one period to process: */
	err = snd_pcm_sw_params_set_start_threshold(cic->slave, sparams, cic->in_period_size);
	err = err == 0 ? snd_pcm_sw_params_set_start_threshold(io->pcm, params, cic->in_period_size) : err;
	if (err < 0) {
		SNDERR("Unable to set start threshold mode for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* allow the transfer when at least period_size samples can be processed */
	err = snd_pcm_sw_params_set_avail_min(cic->slave, sparams, cic->in_period_size);
	err = err == 0 ? snd_pcm_sw_params_set_avail_min(io->pcm, params, cic->out_period_size) : err;
	if (err < 0) {
		SNDERR("Unable to set avail min for capture: %s\n", snd_strerror(err));
		return err;
	}

	snd_pcm_sw_params_get_boundary(params, &cic->boundary);

	return snd_pcm_sw_params(cic->slave, sparams);
}

static int cic_prepare(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	cic->ptr = 0;
	return snd_pcm_prepare(cic->slave);
}

static int cic_poll_descriptors_count(snd_pcm_ioplug_t *io) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	return snd_pcm_poll_descriptors_count(cic->slave);
}

static int cic_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	return snd_pcm_poll_descriptors(cic->slave, pfd, space);
}

static int cic_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents) {
	snd_pcm_cic_filter_t *cic = io->private_data;
	return snd_pcm_poll_descriptors_revents(cic->slave, pfd, nfds, revents);
}

void cic_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
	char *filter[] = {
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_12),
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_16),
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_24),
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_32),
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_48),
		str(CIC_pdmToPcmType_cic_order_5_cic_downsample_unavailable)
	};
	snd_pcm_cic_filter_t *cic = io->private_data;

	snd_output_printf(out, "%s\n", io->name);
	snd_output_printf(out, "Its setup is:\n");
	snd_pcm_dump_setup(io->pcm, out);
	snd_output_printf(out, "Filter Settings: \n");
	snd_output_printf(out, "  Type:             %s\n", filter[cic->type]);
	snd_output_printf(out, "  Gain:             %f%s\n", cic->gain, cic->gain == 0 ? " (default)" : "");
	snd_output_printf(out, "  delay:            %u\n", cic->delay);
	snd_output_printf(out, "  exact delay:      %lu\n", cic->inval_iterations * io->period_size * 1000000 / io->rate);
	snd_output_printf(out, "  OSR:       %u\n", cic->OSR);
	snd_output_printf(out, "Slave: ");
	snd_pcm_dump(cic->slave, out);
}

static void destroy(snd_pcm_cic_filter_t **cic) {
	if(*cic != NULL) {
		if((*cic)->afe != NULL) {
			deleteAfeCicDecoder((*cic)->afe);
			free((*cic)->afe);
			(*cic)->afe = NULL;
		}
		if((*cic)->slave != NULL) {
			snd_pcm_close((*cic)->slave);
			(*cic)->slave = NULL;
		}
		free(*cic);
		*cic = NULL;
	}
}

static int constrains(snd_pcm_ioplug_t *io) {
	int err;

	static unsigned int accesses[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED
	};

	static unsigned int formats[] = {
		SND_PCM_FORMAT_S32_LE
	};

	static unsigned int rates[] = {
		8000, 11025, 16000, 22050, 24000,
		32000, 44100, 48000, 64000, 88200,
		96000
	};

	static unsigned int period_bytes[] = {
		192, 384, 768, 1536
	};

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, ARRAY_SIZE(accesses), accesses);
	if (err < 0) {
		SNDERR("ioplug cannot set hw access mode");
		return err;
	}

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, ARRAY_SIZE(formats), formats);
	if (err < 0) {
		SNDERR("ioplug cannot set hw format");
		return err;
	}

	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, MIN_PCM_CHANNELS, MAX_PCM_CHANNELS);
	if (err < 0) {
		SNDERR("ioplug cannot set hw channels");
		return err;
	}

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE, ARRAY_SIZE(rates), rates);
	if (err < 0) {
		SNDERR("ioplug cannot set hw rates");
		return err;
	}

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, ARRAY_SIZE(period_bytes), period_bytes);
	if (err < 0) {
		SNDERR("ioplug cannot set hw period bytes");
		return err;
	}

	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2, MAX_PERIODS);
	if (err < 0) {
		SNDERR("ioplug cannot set periods");
		return err;
	}

	return err;
}

static inline int parse_struct(snd_config_t **conf, const char **str, snd_pcm_cic_filter_t *cic) {
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	long val;
	int err = 1;

	snd_config_for_each(i, next, *conf) {
		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		if ((strcmp(id, "comment") == 0) || (strcmp(id, "type") == 0))
			continue;

		if(strcmp(id, "slave") == 0) {
			if(snd_config_get_string(n, str) < 0) {
				SNDERR("slave must be a string");
				err = -EINVAL;
				break;
			}
			continue;
		}

		if (strcmp(id, "delay") == 0) {
			if (snd_config_get_integer(n, &val) < 0) {
				SNDERR("'delay' must be a int");
				err = -EINVAL;
				break;
			}
			if(val >= 1000 && val <= 1000000)
				cic->delay = (unsigned int)val;
			else {
				SNDERR("'delay' must be in range of: [1,000us, 1,000,000us].");
				err = -EINVAL;
				break;
			}
			continue;
		}

		if(strcmp(id, "OSR")==0) {
			if(snd_config_get_integer(n, &val) < 0) {
				SNDERR("'OSR' must be a int");
				err = -EINVAL;
				break;
			}
			switch (val) {
			case 48:
				cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_12;
				cic->OSR = 48;
				break;
			case 64:
				cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_16;
				cic->OSR = 64;
				break;
			case 96:
				cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_24;
				cic->OSR = 96;
				break;
			case 128:
				cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_32;
				cic->OSR = 128;
				break;
			case 192:
				cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_48;
				cic->OSR = 192;
				break;
			default:
				SNDERR("OSR value read %ld\n", val);
				SNDERR("Valid 'OSR' values are 48, 64, 96, 128, 192.");
				err = -EINVAL;
				break;
			}
			continue;
		}

		if(strcmp(id, "gain") == 0) {
			if(snd_config_get_integer(n, &val) < 0) {
				SNDERR("'gain' must be a int");
				err = -EINVAL;
				break;
			}
			if(val >= 0 && val <= 100) {
				cic->gain = (float)val;
			} else {
				SNDERR("'gain' must be in range of: [0, 100]. Default set to 0.");
				err = -EINVAL;
				break;
			}
			continue;
		}

		SNDERR("Unknow field %s", id);
		err = -EINVAL;
		break;
	}

	if(err > 0)
		err = 0;

	return err;
}

SND_PCM_PLUGIN_DEFINE_FUNC(PLUG_NAME) {
	snd_pcm_cic_filter_t *cic;
	const char *devname;
	int err;

	if(stream != SND_PCM_STREAM_CAPTURE)
		SNDERR("cicFilter is only for caputure");

	cic = calloc(1, sizeof(*cic));
	if(cic == NULL) {
		SNDERR("Cannot allocate");
		return -ENOMEM;
	}

	cic->afe = malloc(sizeof(*cic->afe));
	if (cic->afe == NULL) {
		SNDERR("Cant create afe structure");
		destroy(&cic);
		return -ENOMEM;
	}

	/* Set default values. */
	cic->type = CIC_pdmToPcmType_cic_order_5_cic_downsample_16;
	cic->OSR = 64;
	cic->gain = 0.0f;

	err = parse_struct(&conf, &devname, cic);
	if(err != 0){
		destroy(&cic);
		return err;
	}

	err = snd_pcm_open(&cic->slave, devname, stream, mode);
	if(err < 0) {
		SNDERR("Cant open slave");
		destroy(&cic);
		return err;
	}

	cic->io.version = SND_PCM_IOPLUG_VERSION;
	cic->io.name = "Digital conversion from PDM 2 PCM";
	cic->io.mmap_rw = 0; /*still having doubts on this field.*/
	cic->io.callback = &cic_funcs;
	cic->io.private_data = cic;
	cic->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;

	err = snd_pcm_ioplug_create(&cic->io, name, stream, mode);
	if(err < 0) {
		destroy(&cic);
		return err;
	}

	/*Check if the afe fields can be aligned with the alsa buffers.*/
	err = constrains(&cic->io);
	if(err < 0) {
		destroy(&cic);
		return err;
	}

	*pcmp = cic->io.pcm;

	return 0;
}

SND_PCM_PLUGIN_SYMBOL(PLUG_NAME);
