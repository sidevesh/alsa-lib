/**
 * \file pcm/pcm.c
 * \brief PCM Interface
 * \author Jaroslav Kysela <perex@suse.cz>
 * \author Abramo Bagnara <abramo@alsa-project.org>
 * \date 2000-2001
 *
 * PCM Interface is designed to write or read digital audio frames. A
 * frame is the data unit converted into/from sound in one time unit
 * (1/rate seconds), by example if you set your playback PCM rate to
 * 44100 you'll hear 44100 frames per second. The size in bytes of a
 * frame may be obtained from bits needed to store a sample and
 * channels count.
 */
/*
 *  PCM Interface - main file
 *  Copyright (c) 1998 by Jaroslav Kysela <perex@suse.cz>
 *  Copyright (c) 2000 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <limits.h>
#include <dlfcn.h>
#include "pcm_local.h"

/**
 * \brief get identifier of PCM handle
 * \param pcm PCM handle
 * \return ascii identifier of PCM handle
 *
 * Returns the ASCII identifier of given PCM handle. It's the same
 * identifier specified in snd_pcm_open().
 */
const char *snd_pcm_name(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->name;
}

/**
 * \brief get type of PCM handle
 * \param pcm PCM handle
 * \return type of PCM handle
 *
 * Returns the type #snd_pcm_type_t of given PCM handle.
 */
snd_pcm_type_t snd_pcm_type(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->type;
}

/**
 * \brief get stream for a PCM handle
 * \param pcm PCM handle
 * \return stream of PCM handle
 *
 * Returns the type #snd_pcm_stream_t of given PCM handle.
 */
snd_pcm_stream_t snd_pcm_stream(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->stream;
}

/**
 * \brief close PCM handle
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 *
 * Closes the specified PCM handle and frees all associated
 * resources.
 */
int snd_pcm_close(snd_pcm_t *pcm)
{
	int err;
	assert(pcm);
	if (pcm->setup) {
		if (pcm->mode & SND_PCM_NONBLOCK || 
		    pcm->stream == SND_PCM_STREAM_CAPTURE)
			snd_pcm_drop(pcm);
		else
			snd_pcm_drain(pcm);
		err = snd_pcm_hw_free(pcm);
		if (err < 0)
			return err;
	}
	while (!list_empty(&pcm->async_handlers)) {
		snd_async_handler_t *h = list_entry(&pcm->async_handlers.next, snd_async_handler_t, hlist);
		snd_async_del_handler(h);
	}
	err = pcm->ops->close(pcm->op_arg);
	if (err < 0)
		return err;
	if (pcm->name)
		free(pcm->name);
	free(pcm);
	return 0;
}	

/**
 * \brief set nonblock mode
 * \param pcm PCM handle
 * \param nonblock 0 = block, 1 = nonblock mode
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock)
{
	int err;
	assert(pcm);
	if ((err = pcm->ops->nonblock(pcm->op_arg, nonblock)) < 0)
		return err;
	if (nonblock)
		pcm->mode |= SND_PCM_NONBLOCK;
	else
		pcm->mode &= ~SND_PCM_NONBLOCK;
	return 0;
}

#ifndef DOC_HIDDEN
/**
 * \brief set async mode
 * \param pcm PCM handle
 * \param sig Signal to raise: < 0 disable, 0 default (SIGIO)
 * \param pid Process ID to signal: 0 current
 * \return 0 on success otherwise a negative error code
 *
 * A signal is raised every period.
 */
int snd_pcm_async(snd_pcm_t *pcm, int sig, pid_t pid)
{
	assert(pcm);
	if (sig == 0)
		sig = SIGIO;
	if (pid == 0)
		pid = getpid();
	return pcm->ops->async(pcm->op_arg, sig, pid);
}
#endif

/**
 * \brief Obtain general (static) information for PCM handle
 * \param pcm PCM handle
 * \param info Information container
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_info(snd_pcm_t *pcm, snd_pcm_info_t *info)
{
	assert(pcm && info);
	return pcm->ops->info(pcm->op_arg, info);
}

/** \brief Install one PCM hardware configuration choosen from a configuration space and #snd_pcm_prepare it
 * \param pcm PCM handle
 * \param params Configuration space definition container
 * \return 0 on success otherwise a negative error code
 *
 * The configuration is choosen fixing single parameters in this order:
 * first access, first format, first subformat, min channels, min rate, 
 * min period time, max buffer size, min tick time
 */
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	int err;
	assert(pcm && params);
	err = _snd_pcm_hw_params(pcm, params);
	if (err < 0)
		return err;
	err = snd_pcm_prepare(pcm);
	return err;
}

/** \brief Remove PCM hardware configuration and free associated resources
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_hw_free(snd_pcm_t *pcm)
{
	int err;
	assert(pcm->setup);
	assert(snd_pcm_state(pcm) <= SND_PCM_STATE_PREPARED);
	if (pcm->mmap_channels) {
		err = snd_pcm_munmap(pcm);
		if (err < 0)
			return err;
	}
	err = pcm->ops->hw_free(pcm->op_arg);
	pcm->setup = 0;
	if (err < 0)
		return err;
	return 0;
}

/** \brief Install PCM software configuration defined by params
 * \param pcm PCM handle
 * \param params Configuration container
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
	int err;
	err = pcm->ops->sw_params(pcm->op_arg, params);
	if (err < 0)
		return err;
	pcm->tstamp_mode = snd_pcm_sw_params_get_tstamp_mode(params);
	pcm->period_step = params->period_step;
	pcm->sleep_min = params->sleep_min;
	pcm->avail_min = params->avail_min;
	pcm->xfer_align = params->xfer_align;
	pcm->start_threshold = params->start_threshold;
	pcm->stop_threshold = params->stop_threshold;
	pcm->silence_threshold = params->silence_threshold;
	pcm->silence_size = params->silence_size;
	pcm->boundary = params->boundary;
	return 0;
}

/**
 * \brief Obtain status (runtime) information for PCM handle
 * \param pcm PCM handle
 * \param status Status container
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_status(snd_pcm_t *pcm, snd_pcm_status_t *status)
{
	assert(pcm && status);
	return pcm->fast_ops->status(pcm->fast_op_arg, status);
}

/**
 * \brief Return PCM state
 * \param pcm PCM handle
 * \return PCM state #snd_pcm_state_t of given PCM handle
 */
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->fast_ops->state(pcm->fast_op_arg);
}

/**
 * \brief Obtain delay for a running PCM handle
 * \param pcm PCM handle
 * \param delayp Returned delay in frames
 * \return 0 on success otherwise a negative error code
 *
 * Delay is distance between current application frame position and
 * sound frame position.
 * It's positive and less than buffer size in normal situation,
 * negative on playback underrun and greater than buffer size on
 * capture overrun.
 */
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->delay(pcm->fast_op_arg, delayp);
}

/**
 * \brief Prepare PCM for use
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_prepare(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->prepare(pcm->fast_op_arg);
}

/**
 * \brief Reset PCM position
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 *
 * Reduce PCM delay to 0.
 */
int snd_pcm_reset(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->reset(pcm->fast_op_arg);
}

/**
 * \brief Start a PCM
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_start(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->start(pcm->fast_op_arg);
}

/**
 * \brief Stop a PCM dropping pending frames
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_drop(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->drop(pcm->fast_op_arg);
}

/**
 * \brief Stop a PCM preserving pending frames
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 *
 * For playback wait for all pending frames to be played and then stop
 * the PCM.
 * For capture stop PCM permitting to retrieve residual frames.
 */
int snd_pcm_drain(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->drain(pcm->fast_op_arg);
}

/**
 * \brief Pause/resume PCM
 * \param pcm PCM handle
 * \param pause 0 = resume, 1 = pause
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_pause(snd_pcm_t *pcm, int enable)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->pause(pcm->fast_op_arg, enable);
}

/**
 * \brief Move application frame position backward
 * \param pcm PCM handle
 * \param frames wanted displacement in frames
 * \return a positive number for actual displacement otherwise a
 * negative error code
 */
snd_pcm_sframes_t snd_pcm_rewind(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	assert(pcm);
	assert(pcm->setup);
	assert(frames > 0);
	return pcm->fast_ops->rewind(pcm->fast_op_arg, frames);
}

/**
 * \brief Write interleaved frames to a PCM
 * \param pcm PCM handle
 * \param buffer frames containing buffer
 * \param size frames to be written
 * \return a positive number of frames actually written otherwise a
 * negative error code
 * \retval -EBADFD PCM is not in the right state (#SND_PCM_STATE_PREPARED or #SND_PCM_STATE_RUNNING)
 * \retval -EPIPE an underrun occured
 *
 * If the blocking behaviour is selected, then routine waits until
 * all requested bytes are played or put to the playback ring buffer.
 * The count of bytes can be less only if a signal or underrun occured.
 *
 * If the non-blocking behaviour is selected, then routine doesn't wait at all.
 */ 
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || buffer);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_INTERLEAVED);
	return _snd_pcm_writei(pcm, buffer, size);
}

/**
 * \brief Write non interleaved frames to a PCM
 * \param pcm PCM handle
 * \param bufs frames containing buffers (one for each channel)
 * \param size frames to be written
 * \return a positive number of frames actually written otherwise a
 * negative error code
 * \retval -EBADFD PCM is not in the right state (#SND_PCM_STATE_PREPARED or #SND_PCM_STATE_RUNNING)
 * \retval -EPIPE an underrun occured
 *
 * If the blocking behaviour is selected, then routine waits until
 * all requested bytes are played or put to the playback ring buffer.
 * The count of bytes can be less only if a signal or underrun occured.
 *
 * If the non-blocking behaviour is selected, then routine doesn't wait at all.
 */ 
snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || bufs);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_NONINTERLEAVED);
	return _snd_pcm_writen(pcm, bufs, size);
}

/**
 * \brief Read interleaved frames from a PCM
 * \param pcm PCM handle
 * \param buffer frames containing buffer
 * \param size frames to be written
 * \return a positive number of frames actually read otherwise a
 * negative error code
 * \retval -EBADFD PCM is not in the right state (#SND_PCM_STATE_PREPARED or #SND_PCM_STATE_RUNNING)
 * \retval -EPIPE an overrun occured
 *
 * If the blocking behaviour was selected, then routine waits until
 * all requested bytes are filled. The count of bytes can be less only
 * if a signal or underrun occured.
 *
 * If the non-blocking behaviour is selected, then routine doesn't wait at all.
 */ 
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || buffer);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_INTERLEAVED);
	return _snd_pcm_readi(pcm, buffer, size);
}

/**
 * \brief Read non interleaved frames to a PCM
 * \param pcm PCM handle
 * \param bufs frames containing buffers (one for each channel)
 * \param size frames to be written
 * \return a positive number of frames actually read otherwise a
 * negative error code
 * \retval -EBADFD PCM is not in the right state (#SND_PCM_STATE_PREPARED or #SND_PCM_STATE_RUNNING)
 * \retval -EPIPE an overrun occured
 *
 * If the blocking behaviour was selected, then routine waits until
 * all requested bytes are filled. The count of bytes can be less only
 * if a signal or underrun occured.
 *
 * If the non-blocking behaviour is selected, then routine doesn't wait at all.
 */ 
snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || bufs);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_NONINTERLEAVED);
	return _snd_pcm_readn(pcm, bufs, size);
}

/**
 * \brief Link two PCMs
 * \param pcm1 first PCM handle
 * \param pcm2 first PCM handle
 * \return 0 on success otherwise a negative error code
 *
 * The two PCMs will start/stop/prepare in sync.
 */ 
int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
	int fd1 = _snd_pcm_link_descriptor(pcm1);
	int fd2 = _snd_pcm_link_descriptor(pcm2);
	if (fd1 < 0 || fd2 < 0)
		return -ENOSYS;
	if (ioctl(fd1, SNDRV_PCM_IOCTL_LINK, fd2) < 0) {
		SYSERR("SNDRV_PCM_IOCTL_LINK failed");
		return -errno;
	}
	return 0;
}

/**
 * \brief Remove a PCM from a linked group
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_unlink(snd_pcm_t *pcm)
{
	int fd;
	fd = _snd_pcm_link_descriptor(pcm);
	if (ioctl(fd, SNDRV_PCM_IOCTL_UNLINK) < 0) {
		SYSERR("SNDRV_PCM_IOCTL_UNLINK failed");
		return -errno;
	}
	return 0;
}

/**
 * \brief get count of poll descriptors for PCM handle
 * \param pcm PCM handle
 * \return count of poll descriptors
 */
int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm)
{
	assert(pcm);
	return 1;
}


/**
 * \brief get poll descriptors
 * \param pcm PCM handle
 * \param pfds array of poll descriptors
 * \param space space in the poll descriptor array
 * \return count of filled descriptors
 */
int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int space)
{
	assert(pcm);
	if (space >= 1) {
		pfds->fd = pcm->poll_fd;
		pfds->events = pcm->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
	}
	return 1;
}

#ifndef DOC_HIDDEN
#define STATE(v) [SND_PCM_STATE_##v] = #v
#define STREAM(v) [SND_PCM_STREAM_##v] = #v
#define READY(v) [SND_PCM_READY_##v] = #v
#define XRUN(v) [SND_PCM_XRUN_##v] = #v
#define SILENCE(v) [SND_PCM_SILENCE_##v] = #v
#define TSTAMP(v) [SND_PCM_TSTAMP_##v] = #v
#define ACCESS(v) [SND_PCM_ACCESS_##v] = #v
#define START(v) [SND_PCM_START_##v] = #v
#define HW_PARAM(v) [SND_PCM_HW_PARAM_##v] = #v
#define SW_PARAM(v) [SND_PCM_SW_PARAM_##v] = #v
#define FORMAT(v) [SND_PCM_FORMAT_##v] = #v
#define SUBFORMAT(v) [SND_PCM_SUBFORMAT_##v] = #v 

#define FORMATD(v, d) [SND_PCM_FORMAT_##v] = d
#define SUBFORMATD(v, d) [SND_PCM_SUBFORMAT_##v] = d 

static const char *snd_pcm_stream_names[] = {
	STREAM(PLAYBACK),
	STREAM(CAPTURE),
};

static const char *snd_pcm_state_names[] = {
	STATE(OPEN),
	STATE(SETUP),
	STATE(PREPARED),
	STATE(RUNNING),
	STATE(XRUN),
	STATE(PAUSED),
};

static const char *snd_pcm_access_names[] = {
	ACCESS(MMAP_INTERLEAVED), 
	ACCESS(MMAP_NONINTERLEAVED),
	ACCESS(MMAP_COMPLEX),
	ACCESS(RW_INTERLEAVED),
	ACCESS(RW_NONINTERLEAVED),
};

static const char *snd_pcm_format_names[] = {
	FORMAT(S8),
	FORMAT(U8),
	FORMAT(S16_LE),
	FORMAT(S16_BE),
	FORMAT(U16_LE),
	FORMAT(U16_BE),
	FORMAT(S24_LE),
	FORMAT(S24_BE),
	FORMAT(U24_LE),
	FORMAT(U24_BE),
	FORMAT(S32_LE),
	FORMAT(S32_BE),
	FORMAT(U32_LE),
	FORMAT(U32_BE),
	FORMAT(FLOAT_LE),
	FORMAT(FLOAT_BE),
	FORMAT(FLOAT64_LE),
	FORMAT(FLOAT64_BE),
	FORMAT(IEC958_SUBFRAME_LE),
	FORMAT(IEC958_SUBFRAME_BE),
	FORMAT(MU_LAW),
	FORMAT(A_LAW),
	FORMAT(IMA_ADPCM),
	FORMAT(MPEG),
	FORMAT(GSM),
	FORMAT(SPECIAL),
};

static const char *snd_pcm_format_descriptions[] = {
	FORMATD(S8, "Signed 8 bit"), 
	FORMATD(U8, "Unsigned 8 bit"),
	FORMATD(S16_LE, "Signed 16 bit Little Endian"),
	FORMATD(S16_BE, "Signed 16 bit Big Endian"),
	FORMATD(U16_LE, "Unsigned 16 bit Little Endian"),
	FORMATD(U16_BE, "Unsigned 16 bit Big Endian"),
	FORMATD(S24_LE, "Signed 24 bit Little Endian"),
	FORMATD(S24_BE, "Signed 24 bit Big Endian"),
	FORMATD(U24_LE, "Unsigned 24 bit Little Endian"),
	FORMATD(U24_BE, "Unsigned 24 bit Big Endian"),
	FORMATD(S32_LE, "Signed 32 bit Little Endian"),
	FORMATD(S32_BE, "Signed 32 bit Big Endian"),
	FORMATD(U32_LE, "Unsigned 32 bit Little Endian"),
	FORMATD(U32_BE, "Unsigned 32 bit Big Endian"),
	FORMATD(FLOAT_LE, "Float 32 bit Little Endian"),
	FORMATD(FLOAT_BE, "Float 32 bit Big Endian"),
	FORMATD(FLOAT64_LE, "Float 64 bit Little Endian"),
	FORMATD(FLOAT64_BE, "Float 64 bit Big Endian"),
	FORMATD(IEC958_SUBFRAME_LE, "IEC-958 Little Endian"),
	FORMATD(IEC958_SUBFRAME_BE, "IEC-958 Big Endian"),
	FORMATD(MU_LAW, "Mu-Law"),
	FORMATD(A_LAW, "A-Law"),
	FORMATD(IMA_ADPCM, "Ima-ADPCM"),
	FORMATD(MPEG, "MPEG"),
	FORMATD(GSM, "GSM"),
	FORMATD(SPECIAL, "Special"),
};

static const char *snd_pcm_subformat_names[] = {
	SUBFORMAT(STD), 
};

static const char *snd_pcm_subformat_descriptions[] = {
	SUBFORMATD(STD, "Standard"), 
};

static const char *snd_pcm_start_mode_names[] = {
	START(EXPLICIT),
	START(DATA),
};

static const char *snd_pcm_xrun_mode_names[] = {
	XRUN(NONE),
	XRUN(STOP),
};

static const char *snd_pcm_tstamp_mode_names[] = {
	TSTAMP(NONE),
	TSTAMP(MMAP),
};
#endif

/**
 * \brief get name of PCM stream
 * \param stream PCM stream
 * \return ascii name of PCM stream
 */
const char *snd_pcm_stream_name(snd_pcm_stream_t stream)
{
	assert(stream <= SND_PCM_STREAM_LAST);
	return snd_pcm_stream_names[stream];
}

/**
 * \brief get name of PCM access type
 * \param access PCM access type
 * \return ascii name of PCM access type
 */
const char *snd_pcm_access_name(snd_pcm_access_t acc)
{
	assert(acc <= SND_PCM_ACCESS_LAST);
	return snd_pcm_access_names[acc];
}

/**
 * \brief get name of PCM sample format
 * \param format PCM sample format
 * \return ascii name of PCM sample format
 */
const char *snd_pcm_format_name(snd_pcm_format_t format)
{
	assert(format <= SND_PCM_FORMAT_LAST);
	return snd_pcm_format_names[format];
}

/**
 * \brief get description of PCM sample format
 * \param format PCM sample format
 * \return ascii description of PCM sample format
 */
const char *snd_pcm_format_description(snd_pcm_format_t format)
{
	assert(format <= SND_PCM_FORMAT_LAST);
	return snd_pcm_format_descriptions[format];
}

/**
 * \brief get PCM sample format from name
 * \param name PCM sample format name (case insensitive)
 * \return PCM sample format
 */
snd_pcm_format_t snd_pcm_format_value(const char* name)
{
	snd_pcm_format_t format;
	for (format = 0; format <= SND_PCM_FORMAT_LAST; format++) {
		if (snd_pcm_format_names[format] &&
		    strcasecmp(name, snd_pcm_format_names[format]) == 0) {
			return format;
		}
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

/**
 * \brief get name of PCM sample subformat
 * \param format PCM sample subformat
 * \return ascii name of PCM sample subformat
 */
const char *snd_pcm_subformat_name(snd_pcm_subformat_t subformat)
{
	assert(subformat <= SND_PCM_SUBFORMAT_LAST);
	return snd_pcm_subformat_names[subformat];
}

/**
 * \brief get description of PCM sample subformat
 * \param subformat PCM sample subformat
 * \return ascii description of PCM sample subformat
 */
const char *snd_pcm_subformat_description(snd_pcm_subformat_t subformat)
{
	assert(subformat <= SND_PCM_SUBFORMAT_LAST);
	return snd_pcm_subformat_descriptions[subformat];
}

/**
 * \brief (DEPRECATED) get name of PCM start mode setting
 * \param mode PCM start mode
 * \return ascii name of PCM start mode setting
 */
const char *snd_pcm_start_mode_name(snd_pcm_start_t mode)
{
	assert(mode <= SND_PCM_START_LAST);
	return snd_pcm_start_mode_names[mode];
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_start_mode_name, "Warning: start_mode is deprecated, consider to use start_threshold");
#endif

/**
 * \brief (DEPRECATED) get name of PCM xrun mode setting
 * \param mode PCM xrun mode
 * \return ascii name of PCM xrun mode setting
 */
const char *snd_pcm_xrun_mode_name(snd_pcm_xrun_t mode)
{
	assert(mode <= SND_PCM_XRUN_LAST);
	return snd_pcm_xrun_mode_names[mode];
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_xrun_mode_name, "Warning: xrun_mode is deprecated, consider to use stop_threshold");
#endif

/**
 * \brief get name of PCM tstamp mode setting
 * \param mode PCM tstamp mode
 * \return ascii name of PCM tstamp mode setting
 */
const char *snd_pcm_tstamp_mode_name(snd_pcm_tstamp_t mode)
{
	assert(mode <= SND_PCM_TSTAMP_LAST);
	return snd_pcm_tstamp_mode_names[mode];
}

/**
 * \brief get name of PCM state
 * \param state PCM state
 * \return ascii name of PCM state
 */
const char *snd_pcm_state_name(snd_pcm_state_t state)
{
	assert(state <= SND_PCM_STATE_LAST);
	return snd_pcm_state_names[state];
}

/**
 * \brief Dump current hardware setup for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_dump_hw_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	assert(pcm->setup);
        snd_output_printf(out, "stream       : %s\n", snd_pcm_stream_name(pcm->stream));
	snd_output_printf(out, "access       : %s\n", snd_pcm_access_name(pcm->access));
	snd_output_printf(out, "format       : %s\n", snd_pcm_format_name(pcm->format));
	snd_output_printf(out, "subformat    : %s\n", snd_pcm_subformat_name(pcm->subformat));
	snd_output_printf(out, "channels     : %u\n", pcm->channels);
	snd_output_printf(out, "rate         : %u\n", pcm->rate);
	snd_output_printf(out, "exact rate   : %g (%u/%u)\n", (double) pcm->rate_num / pcm->rate_den, pcm->rate_num, pcm->rate_den);
	snd_output_printf(out, "msbits       : %u\n", pcm->msbits);
	snd_output_printf(out, "buffer_size  : %lu\n", pcm->buffer_size);
	snd_output_printf(out, "period_size  : %lu\n", pcm->period_size);
	snd_output_printf(out, "period_time  : %u\n", pcm->period_time);
	snd_output_printf(out, "tick_time    : %u\n", pcm->tick_time);
	return 0;
}

/**
 * \brief Dump current software setup for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_dump_sw_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	assert(pcm->setup);
	snd_output_printf(out, "tstamp_mode  : %s\n", snd_pcm_tstamp_mode_name(pcm->tstamp_mode));
	snd_output_printf(out, "period_step  : %d\n", pcm->period_step);
	snd_output_printf(out, "sleep_min    : %d\n", pcm->sleep_min);
	snd_output_printf(out, "avail_min    : %ld\n", pcm->avail_min);
	snd_output_printf(out, "xfer_align   : %ld\n", pcm->xfer_align);
	snd_output_printf(out, "start_threshold  : %ld\n", pcm->start_threshold);
	snd_output_printf(out, "stop_threshold   : %ld\n", pcm->stop_threshold);
	snd_output_printf(out, "silence_threshold: %ld\n", pcm->silence_threshold);
	snd_output_printf(out, "silence_size : %ld\n", pcm->silence_size);
	snd_output_printf(out, "boundary     : %ld\n", pcm->boundary);
	return 0;
}

/**
 * \brief Dump current setup (hardware and software) for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_dump_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	snd_pcm_dump_hw_setup(pcm, out);
	snd_pcm_dump_sw_setup(pcm, out);
	return 0;
}

/**
 * \brief Dump status
 * \param status Status container
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_status_dump(snd_pcm_status_t *status, snd_output_t *out)
{
	assert(status);
	snd_output_printf(out, "state       : %s\n", snd_pcm_state_name((snd_pcm_state_t) status->state));
	snd_output_printf(out, "trigger_time: %ld.%06ld\n",
		status->trigger_tstamp.tv_sec, status->trigger_tstamp.tv_usec);
	snd_output_printf(out, "tstamp      : %ld.%06ld\n",
		status->tstamp.tv_sec, status->tstamp.tv_usec);
	snd_output_printf(out, "delay       : %ld\n", (long)status->delay);
	snd_output_printf(out, "avail       : %ld\n", (long)status->avail);
	snd_output_printf(out, "avail_max   : %ld\n", (long)status->avail_max);
	return 0;
}

/**
 * \brief Dump PCM info
 * \param pcm PCM handle
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_dump(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	pcm->ops->dump(pcm->op_arg, out);
	return 0;
}

/**
 * \brief Convert bytes in frames for a PCM
 * \param pcm PCM handle
 * \param bytes quantity in bytes
 * \return quantity expressed in frames
 */
snd_pcm_sframes_t snd_pcm_bytes_to_frames(snd_pcm_t *pcm, ssize_t bytes)
{
	assert(pcm);
	assert(pcm->setup);
	return bytes * 8 / pcm->frame_bits;
}

/**
 * \brief Convert frames in bytes for a PCM
 * \param pcm PCM handle
 * \param frames quantity in frames
 * \return quantity expressed in bytes
 */
ssize_t snd_pcm_frames_to_bytes(snd_pcm_t *pcm, snd_pcm_sframes_t frames)
{
	assert(pcm);
	assert(pcm->setup);
	return frames * pcm->frame_bits / 8;
}

/**
 * \brief Convert bytes in samples for a PCM
 * \param pcm PCM handle
 * \param bytes quantity in bytes
 * \return quantity expressed in samples
 */
int snd_pcm_bytes_to_samples(snd_pcm_t *pcm, ssize_t bytes)
{
	assert(pcm);
	assert(pcm->setup);
	return bytes * 8 / pcm->sample_bits;
}

/**
 * \brief Convert samples in bytes for a PCM
 * \param pcm PCM handle
 * \param samples quantity in samples
 * \return quantity expressed in bytes
 */
ssize_t snd_pcm_samples_to_bytes(snd_pcm_t *pcm, int samples)
{
	assert(pcm);
	assert(pcm->setup);
	return samples * pcm->sample_bits / 8;
}

/**
 * \brief Add an async handler for a PCM
 * \param handler Returned handler handle
 * \param pcm PCM handle
 * \param callback Callback function
 * \param private_data Callback private data
 * \return 0 otherwise a negative error code on failure
 */
int snd_async_add_pcm_handler(snd_async_handler_t **handler, snd_pcm_t *pcm, 
			      snd_async_callback_t callback, void *private_data)
{
	int err;
	int was_empty;
	snd_async_handler_t *h;
	err = snd_async_add_handler(&h, _snd_pcm_async_descriptor(pcm),
				    callback, private_data);
	if (err < 0)
		return err;
	h->type = SND_ASYNC_HANDLER_PCM;
	h->u.pcm = pcm;
	was_empty = list_empty(&pcm->async_handlers);
	list_add_tail(&h->hlist, &pcm->async_handlers);
	if (was_empty) {
		err = snd_pcm_async(pcm, getpid(), snd_async_signo);
		if (err < 0) {
			snd_async_del_handler(h);
			return err;
		}
	}
	*handler = h;
	return 0;
}

/**
 * \brief Return PCM handle related to an async handler
 * \param handler Async handler handle
 * \return PCM handle
 */
snd_pcm_t *snd_async_handler_get_pcm(snd_async_handler_t *handler)
{
	assert(handler->type = SND_ASYNC_HANDLER_PCM);
	return handler->u.pcm;
}

static int snd_pcm_open_conf(snd_pcm_t **pcmp, const char *name,
			     snd_config_t *pcm_root, snd_config_t *pcm_conf,
			     snd_pcm_stream_t stream, int mode)
{
	const char *str;
	char buf[256];
	int err;
	snd_config_t *conf, *type_conf = NULL;
	snd_config_iterator_t i, next;
	const char *lib = NULL, *open_name = NULL;
	int (*open_func)(snd_pcm_t **, const char *, 
			 snd_config_t *, snd_config_t *, 
			 snd_pcm_stream_t, int) = NULL;
	void *h;
	if (snd_config_get_type(pcm_conf) != SND_CONFIG_TYPE_COMPOUND) {
		if (name)
			SNDERR("Invalid type for PCM %s definition", name);
		else
			SNDERR("Invalid type for PCM definition");
		return -EINVAL;
	}
	err = snd_config_search(pcm_conf, "type", &conf);
	if (err < 0) {
		SNDERR("type is not defined");
		return err;
	}
	err = snd_config_get_string(conf, &str);
	if (err < 0) {
		SNDERR("Invalid type for %s", snd_config_get_id(conf));
		return err;
	}
	err = snd_config_search_definition(pcm_root, "pcm_type", str, &type_conf);
	if (err >= 0) {
		if (snd_config_get_type(type_conf) != SND_CONFIG_TYPE_COMPOUND) {
			SNDERR("Invalid type for PCM type %s definition", str);
			goto _err;
		}
		snd_config_for_each(i, next, type_conf) {
			snd_config_t *n = snd_config_iterator_entry(i);
			const char *id = snd_config_get_id(n);
			if (strcmp(id, "comment") == 0)
				continue;
			if (strcmp(id, "lib") == 0) {
				err = snd_config_get_string(n, &lib);
				if (err < 0) {
					SNDERR("Invalid type for %s", id);
					goto _err;
				}
				continue;
			}
			if (strcmp(id, "open") == 0) {
				err = snd_config_get_string(n, &open_name);
				if (err < 0) {
					SNDERR("Invalid type for %s", id);
					goto _err;
				}
				continue;
			}
			SNDERR("Unknown field %s", id);
			err = -EINVAL;
			goto _err;
		}
	}
	if (!open_name) {
		open_name = buf;
		snprintf(buf, sizeof(buf), "_snd_pcm_%s_open", str);
	}
	if (!lib)
		lib = ALSA_LIB;
	h = dlopen(lib, RTLD_NOW);
	open_func = h ? dlsym(h, open_name) : NULL;
	err = 0;
	if (!h) {
		SNDERR("Cannot open shared library %s", lib);
		err = -ENOENT;
	} else if (!open_func) {
		SNDERR("symbol %s is not defined inside %s", open_name, lib);
		dlclose(h);
		err = -ENXIO;
	}
       _err:
	if (type_conf)
		snd_config_delete(type_conf);
	return err >= 0 ? open_func(pcmp, name, pcm_root, pcm_conf, stream, mode) : err;
}

static int snd_pcm_open_noupdate(snd_pcm_t **pcmp, snd_config_t *root,
				 const char *name, snd_pcm_stream_t stream, int mode)
{
	int err;
	snd_config_t *pcm_conf, *n;
	err = snd_config_search_definition(root, "pcm", name, &pcm_conf);
	if (err < 0) {
		SNDERR("Unknown PCM %s", name);
		return err;
	}
	if (snd_config_search(pcm_conf, "refer", &n) >= 0) {
		snd_config_t *refer;
		char *new_name;
		err = snd_config_refer_load(&refer, &new_name, root, n);
		if (err < 0) {
			SNDERR("Unable to load refered block in PCM %s: %s", name, snd_strerror(err));
			return err;
		}
		err = snd_pcm_open_noupdate(pcmp, refer, new_name, stream, mode);
		if (refer != root)
			snd_config_delete(refer);
		return err;
	}
	err = snd_pcm_open_conf(pcmp, name, root, pcm_conf, stream, mode);
	snd_config_delete(pcm_conf);
	return err;
}

/**
 * \brief Opens a PCM
 * \param pcmp Returned PCM handle
 * \param name ASCII identifier of the PCM handle
 * \param stream Wanted stream
 * \param mode Open mode (see #SND_PCM_NONBLOCK, #SND_PCM_ASYNC)
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_open(snd_pcm_t **pcmp, const char *name, 
		 snd_pcm_stream_t stream, int mode)
{
	int err;
	assert(pcmp && name);
	err = snd_config_update();
	if (err < 0)
		return err;
	return snd_pcm_open_noupdate(pcmp, snd_config, name, stream, mode);
}

#ifndef DOC_HIDDEN

int snd_pcm_new(snd_pcm_t **pcmp, snd_pcm_type_t type, const char *name,
		snd_pcm_stream_t stream, int mode)
{
	snd_pcm_t *pcm;
	pcm = calloc(1, sizeof(*pcm));
	if (!pcm)
		return -ENOMEM;
	pcm->type = type;
	if (name)
		pcm->name = strdup(name);
	pcm->stream = stream;
	pcm->mode = mode;
	pcm->op_arg = pcm;
	pcm->fast_op_arg = pcm;
	INIT_LIST_HEAD(&pcm->async_handlers);
	*pcmp = pcm;
	return 0;
}

int snd_pcm_open_slave(snd_pcm_t **pcmp, snd_config_t *root,
		       snd_config_t *conf, snd_pcm_stream_t stream,
		       int mode)
{
	const char *str;
	if (snd_config_get_string(conf, &str) >= 0)
		return snd_pcm_open_noupdate(pcmp, root, str, stream, mode);
	return snd_pcm_open_conf(pcmp, NULL, root, conf, stream, mode);
}
#endif

/**
 * \brief Wait for a PCM to become ready
 * \param pcm PCM handle
 * \param timeout maximum time in milliseconds to wait
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
	struct pollfd pfd;
	int err;
	err = snd_pcm_poll_descriptors(pcm, &pfd, 1);
	assert(err == 1);
	err = poll(&pfd, 1, timeout);
	if (err < 0)
		return -errno;
	return 0;
}

/**
 * \brief Return number of frames ready to be read/written
 * \param pcm PCM handle
 * \return a positive number of frames ready otherwise a negative
 * error code
 *
 * On capture does all the actions needed to transport to application
 * level all the ready frames across underlying layers.
 */
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
	return pcm->fast_ops->avail_update(pcm->fast_op_arg);
}

/**
 * \brief Silence an area
 * \param dst_area area specification
 * \param dst_offset offset in frames inside area
 * \param samples samples to silence
 * \param format PCM sample format
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_area_silence(const snd_pcm_channel_area_t *dst_area, snd_pcm_uframes_t dst_offset,
			 unsigned int samples, snd_pcm_format_t format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	char *dst;
	unsigned int dst_step;
	int width;
	u_int64_t silence;
	if (!dst_area->addr)
		return 0;
	dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
	width = snd_pcm_format_physical_width(format);
	silence = snd_pcm_format_silence_64(format);
	if (dst_area->step == (unsigned int) width) {
		unsigned int dwords = samples * width / 64;
		samples -= dwords * 64 / width;
		while (dwords-- > 0)
			*((u_int64_t*)dst)++ = silence;
		if (samples == 0)
			return 0;
	}
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		u_int8_t s0 = silence & 0xf0;
		u_int8_t s1 = silence & 0x0f;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			if (dstbit) {
				*dst &= 0xf0;
				*dst |= s1;
			} else {
				*dst &= 0x0f;
				*dst |= s0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		u_int8_t sil = silence;
		while (samples-- > 0) {
			*dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		u_int16_t sil = silence;
		while (samples-- > 0) {
			*(u_int16_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		u_int32_t sil = silence;
		while (samples-- > 0) {
			*(u_int32_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = silence;
			dst += dst_step;
		}
		break;
	}
	default:
		assert(0);
	}
	return 0;
}

/**
 * \brief Silence one or more areas
 * \param dst_areas areas specification (one for each channel)
 * \param dst_offset offset in frames inside area
 * \param channels channels count
 * \param frames frames to silence
 * \param format PCM sample format
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_areas_silence(const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
			  unsigned int channels, snd_pcm_uframes_t frames, snd_pcm_format_t format)
{
	int width = snd_pcm_format_physical_width(format);
	while (channels > 0) {
		void *addr = dst_areas->addr;
		unsigned int step = dst_areas->step;
		const snd_pcm_channel_area_t *begin = dst_areas;
		int channels1 = channels;
		unsigned int chns = 0;
		int err;
		while (1) {
			channels1--;
			chns++;
			dst_areas++;
			if (channels1 == 0 ||
			    dst_areas->addr != addr ||
			    dst_areas->step != step ||
			    dst_areas->first != dst_areas[-1].first + width)
				break;
		}
		if (chns > 1 && chns * width == step) {
			/* Collapse the areas */
			snd_pcm_channel_area_t d;
			d.addr = begin->addr;
			d.first = begin->first;
			d.step = width;
			err = snd_pcm_area_silence(&d, dst_offset * chns, frames * chns, format);
			channels -= chns;
		} else {
			err = snd_pcm_area_silence(begin, dst_offset, frames, format);
			dst_areas = begin + 1;
			channels--;
		}
		if (err < 0)
			return err;
	}
	return 0;
}


/**
 * \brief Copy an area
 * \param dst_area destination area specification
 * \param dst_offset offset in frames inside destination area
 * \param src_area source area specification
 * \param src_offset offset in frames inside source area
 * \param samples samples to copy
 * \param format PCM sample format
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_area_copy(const snd_pcm_channel_area_t *dst_area, snd_pcm_uframes_t dst_offset,
		      const snd_pcm_channel_area_t *src_area, snd_pcm_uframes_t src_offset,
		      unsigned int samples, snd_pcm_format_t format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	const char *src;
	char *dst;
	int width;
	int src_step, dst_step;
	if (!src_area->addr)
		return snd_pcm_area_silence(dst_area, dst_offset, samples, format);
	src = snd_pcm_channel_area_addr(src_area, src_offset);
	if (!dst_area->addr)
		return 0;
	dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
	width = snd_pcm_format_physical_width(format);
	if (src_area->step == (unsigned int) width &&
	    dst_area->step == (unsigned int) width) {
		size_t bytes = samples * width / 8;
		samples -= bytes * 8 / width;
		memcpy(dst, src, bytes);
		if (samples == 0)
			return 0;
	}
	src_step = src_area->step / 8;
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		int srcbit = src_area->first % 8;
		int srcbit_step = src_area->step % 8;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			unsigned char srcval;
			if (srcbit)
				srcval = *src & 0x0f;
			else
				srcval = *src & 0xf0;
			if (dstbit)
				*dst &= 0xf0;
			else
				*dst &= 0x0f;
			*dst |= srcval;
			src += src_step;
			srcbit += srcbit_step;
			if (srcbit == 8) {
				src++;
				srcbit = 0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		while (samples-- > 0) {
			*dst = *src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		while (samples-- > 0) {
			*(u_int16_t*)dst = *(const u_int16_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		while (samples-- > 0) {
			*(u_int32_t*)dst = *(const u_int32_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = *(const u_int64_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	default:
		assert(0);
	}
	return 0;
}

/**
 * \brief Copy one or more areas
 * \param dst_areas destination areas specification (one for each channel)
 * \param dst_offset offset in frames inside destination area
 * \param src_areas source areas specification (one for each channel)
 * \param src_offset offset in frames inside source area
 * \param channels channels count
 * \param frames frames to copy
 * \param format PCM sample format
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_areas_copy(const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
		       const snd_pcm_channel_area_t *src_areas, snd_pcm_uframes_t src_offset,
		       unsigned int channels, snd_pcm_uframes_t frames, snd_pcm_format_t format)
{
	int width = snd_pcm_format_physical_width(format);
	assert(dst_areas);
	assert(src_areas);
	assert(channels > 0);
	assert(frames > 0);
	while (channels > 0) {
		unsigned int step = src_areas->step;
		void *src_addr = src_areas->addr;
		const snd_pcm_channel_area_t *src_start = src_areas;
		void *dst_addr = dst_areas->addr;
		const snd_pcm_channel_area_t *dst_start = dst_areas;
		int channels1 = channels;
		unsigned int chns = 0;
		while (dst_areas->step == step) {
			channels1--;
			chns++;
			src_areas++;
			dst_areas++;
			if (channels1 == 0 ||
			    src_areas->step != step ||
			    src_areas->addr != src_addr ||
			    dst_areas->addr != dst_addr ||
			    src_areas->first != src_areas[-1].first + width ||
			    dst_areas->first != dst_areas[-1].first + width)
				break;
		}
		if (chns > 1 && chns * width == step) {
			/* Collapse the areas */
			snd_pcm_channel_area_t s, d;
			s.addr = src_start->addr;
			s.first = src_start->first;
			s.step = width;
			d.addr = dst_start->addr;
			d.first = dst_start->first;
			d.step = width;
			snd_pcm_area_copy(&d, dst_offset * chns,
					  &s, src_offset * chns, 
					  frames * chns, format);
			channels -= chns;
		} else {
			snd_pcm_area_copy(dst_start, dst_offset,
					  src_start, src_offset,
					  frames, format);
			src_areas = src_start + 1;
			dst_areas = dst_start + 1;
			channels--;
		}
	}
	return 0;
}

/**
 * \brief Dump a PCM hardware configuration space
 * \param params Configuration space
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_hw_params_dump(snd_pcm_hw_params_t *params, snd_output_t *out)
{
	unsigned int k;
	for (k = 0; k <= SND_PCM_HW_PARAM_LAST; k++) {
		snd_output_printf(out, "%s: ", snd_pcm_hw_param_name(k));
		snd_pcm_hw_param_dump(params, k, out);
		snd_output_putc(out, '\n');
	}
	return 0;
}

/**
 * \brief Get rate exact info from a configuration space
 * \param params Configuration space
 * \param rate_num Pointer to returned rate numerator
 * \param rate_den Pointer to returned rate denominator
 * \return 0 otherwise a negative error code if the info is not available
 */
int snd_pcm_hw_params_get_rate_numden(const snd_pcm_hw_params_t *params,
				      unsigned int *rate_num, unsigned int *rate_den)
{
	if (params->rate_den == 0)
		return -EINVAL;
	*rate_num = params->rate_num;
	*rate_den = params->rate_den;
	return 0;
}

/**
 * \brief Get sample resolution info from a configuration space
 * \param params Configuration space
 * \return significative bits in sample otherwise a negative error code if the info is not available
 */
int snd_pcm_hw_params_get_sbits(const snd_pcm_hw_params_t *params)
{
	if (params->msbits == 0)
		return -EINVAL;
	return params->msbits;
}

/**
 * \brief Get hardare fifo size info from a configuration space
 * \param params Configuration space
 * \return fifo size in frames otherwise a negative error code if the info is not available
 */
int snd_pcm_hw_params_get_fifo_size(const snd_pcm_hw_params_t *params)
{
	if (params->fifo_size == 0)
		return -EINVAL;
	return params->fifo_size;
}

/**
 * \brief Fill params with a full configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 */
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	_snd_pcm_hw_params_any(params);
	return snd_pcm_hw_refine(pcm, params);
}

/**
 * \brief get size of #snd_pcm_access_mask_t
 * \return size in bytes
 */
size_t snd_pcm_access_mask_sizeof()
{
	return sizeof(snd_pcm_access_mask_t);
}

/**
 * \brief allocate an empty #snd_pcm_access_mask_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_access_mask_malloc(snd_pcm_access_mask_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_access_mask_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_access_mask_t
 * \param pointer to object to free
 */
void snd_pcm_access_mask_free(snd_pcm_access_mask_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_access_mask_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_access_mask_copy(snd_pcm_access_mask_t *dst, const snd_pcm_access_mask_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief reset all bits in a #snd_pcm_access_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_access_mask_none(snd_pcm_access_mask_t *mask)
{
	snd_mask_none((snd_mask_t *) mask);
}

/**
 * \brief set all bits in a #snd_pcm_access_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_access_mask_any(snd_pcm_access_mask_t *mask)
{
	snd_mask_any((snd_mask_t *) mask);
}

/**
 * \brief test the presence of an access type in a #snd_pcm_access_mask_t
 * \param mask pointer to mask
 * \param val access type
 */
int snd_pcm_access_mask_test(const snd_pcm_access_mask_t *mask, snd_pcm_access_t val)
{
	return snd_mask_test((const snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make an access type present in a #snd_pcm_access_mask_t
 * \param mask pointer to mask
 * \param val access type
 */
void snd_pcm_access_mask_set(snd_pcm_access_mask_t *mask, snd_pcm_access_t val)
{
	snd_mask_set((snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make an access type missing from a #snd_pcm_access_mask_t
 * \param mask pointer to mask
 * \param val access type
 */
void snd_pcm_access_mask_reset(snd_pcm_access_mask_t *mask, snd_pcm_access_t val)
{
	snd_mask_reset((snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief get size of #snd_pcm_format_mask_t
 * \return size in bytes
 */
size_t snd_pcm_format_mask_sizeof()
{
	return sizeof(snd_pcm_format_mask_t);
}

/**
 * \brief allocate an empty #snd_pcm_format_mask_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_format_mask_malloc(snd_pcm_format_mask_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_format_mask_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_format_mask_t
 * \param pointer to object to free
 */
void snd_pcm_format_mask_free(snd_pcm_format_mask_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_format_mask_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_format_mask_copy(snd_pcm_format_mask_t *dst, const snd_pcm_format_mask_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief reset all bits in a #snd_pcm_format_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_format_mask_none(snd_pcm_format_mask_t *mask)
{
	snd_mask_none((snd_mask_t *) mask);
}

/**
 * \brief set all bits in a #snd_pcm_format_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_format_mask_any(snd_pcm_format_mask_t *mask)
{
	snd_mask_any((snd_mask_t *) mask);
}

/**
 * \brief test the presence of a format in a #snd_pcm_format_mask_t
 * \param mask pointer to mask
 * \param val format
 */
int snd_pcm_format_mask_test(const snd_pcm_format_mask_t *mask, snd_pcm_format_t val)
{
	return snd_mask_test((const snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make a format present in a #snd_pcm_format_mask_t
 * \param mask pointer to mask
 * \param val format
 */
void snd_pcm_format_mask_set(snd_pcm_format_mask_t *mask, snd_pcm_format_t val)
{
	snd_mask_set((snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make a format missing from a #snd_pcm_format_mask_t
 * \param mask pointer to mask
 * \param val format
 */
void snd_pcm_format_mask_reset(snd_pcm_format_mask_t *mask, snd_pcm_format_t val)
{
	snd_mask_reset((snd_mask_t *) mask, (unsigned long) val);
}


/**
 * \brief get size of #snd_pcm_subformat_mask_t
 * \return size in bytes
 */
size_t snd_pcm_subformat_mask_sizeof()
{
	return sizeof(snd_pcm_subformat_mask_t);
}

/**
 * \brief allocate an empty #snd_pcm_subformat_mask_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_subformat_mask_malloc(snd_pcm_subformat_mask_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_subformat_mask_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_subformat_mask_t
 * \param pointer to object to free
 */
void snd_pcm_subformat_mask_free(snd_pcm_subformat_mask_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_subformat_mask_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_subformat_mask_copy(snd_pcm_subformat_mask_t *dst, const snd_pcm_subformat_mask_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief reset all bits in a #snd_pcm_subformat_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_subformat_mask_none(snd_pcm_subformat_mask_t *mask)
{
	snd_mask_none((snd_mask_t *) mask);
}

/**
 * \brief set all bits in a #snd_pcm_subformat_mask_t
 * \param mask pointer to mask
 */
void snd_pcm_subformat_mask_any(snd_pcm_subformat_mask_t *mask)
{
	snd_mask_any((snd_mask_t *) mask);
}

/**
 * \brief test the presence of a subformat in a #snd_pcm_subformat_mask_t
 * \param mask pointer to mask
 * \param val subformat
 */
int snd_pcm_subformat_mask_test(const snd_pcm_subformat_mask_t *mask, snd_pcm_subformat_t val)
{
	return snd_mask_test((const snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make a subformat present in a #snd_pcm_subformat_mask_t
 * \param mask pointer to mask
 * \param val subformat
 */
void snd_pcm_subformat_mask_set(snd_pcm_subformat_mask_t *mask, snd_pcm_subformat_t val)
{
	snd_mask_set((snd_mask_t *) mask, (unsigned long) val);
}

/**
 * \brief make a subformat missing from a #snd_pcm_subformat_mask_t
 * \param mask pointer to mask
 * \param val subformat
 */
void snd_pcm_subformat_mask_reset(snd_pcm_subformat_mask_t *mask, snd_pcm_subformat_t val)
{
	snd_mask_reset((snd_mask_t *) mask, (unsigned long) val);
}


/**
 * \brief get size of #snd_pcm_hw_params_t
 * \return size in bytes
 */
size_t snd_pcm_hw_params_sizeof()
{
	return sizeof(snd_pcm_hw_params_t);
}

/**
 * \brief allocate an invalid #snd_pcm_hw_params_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_hw_params_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_hw_params_t
 * \param pointer to object to free
 */
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_hw_params_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_hw_params_copy(snd_pcm_hw_params_t *dst, const snd_pcm_hw_params_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief Extract access type from a configuration space
 * \param params Configuration space
 * \return access type otherwise a negative error code if not exactly one is present
 */
int snd_pcm_hw_params_get_access(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_ACCESS, NULL);
}

/**
 * \brief Verify if an access type is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val access type
 * \return 1 if available 0 otherwise
 */
int snd_pcm_hw_params_test_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_ACCESS, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only one access type
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val access type
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_ACCESS, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only its first access type
 * \param pcm PCM handle
 * \param params Configuration space
 * \return access type
 */
snd_pcm_access_t snd_pcm_hw_params_set_access_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_ACCESS, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its last access type
 * \param pcm PCM handle
 * \param params Configuration space
 * \return access type
 */
snd_pcm_access_t snd_pcm_hw_params_set_access_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_ACCESS, NULL);
}

/**
 * \brief Restrict a configuration space to contain only a set of access types
 * \param pcm PCM handle
 * \param params Configuration space
 * \param mask Access mask
 * \return access type
 */
int snd_pcm_hw_params_set_access_mask(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_mask_t *mask)
{
	return snd_pcm_hw_param_set_mask(pcm, params, SND_TRY, SND_PCM_HW_PARAM_ACCESS, (snd_mask_t *) mask);
}

/**
 * \brief Get access mask from a configuration space
 * \param params Configuration space
 * \param mask Returned Access mask
 */
void snd_pcm_hw_params_get_access_mask(snd_pcm_hw_params_t *params, snd_pcm_access_mask_t *mask)
{
	snd_pcm_access_mask_copy(mask, snd_pcm_hw_param_get_mask(params, SND_PCM_HW_PARAM_ACCESS));
}


/**
 * \brief Extract format from a configuration space
 * \param params Configuration space
 * \return format otherwise a negative error code if not exactly one is present
 */
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_FORMAT, NULL);
}

/**
 * \brief Verify if a format is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val format
 * \return 1 if available 0 otherwise
 */
int snd_pcm_hw_params_test_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_FORMAT, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only one format
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val format
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_FORMAT, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only its first format
 * \param pcm PCM handle
 * \param params Configuration space
 * \return format
 */
snd_pcm_format_t snd_pcm_hw_params_set_format_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_FORMAT, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its last format
 * \param pcm PCM handle
 * \param params Configuration space
 * \return format
 */
snd_pcm_format_t snd_pcm_hw_params_set_format_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_FORMAT, NULL);
}

/**
 * \brief Restrict a configuration space to contain only a set of formats
 * \param pcm PCM handle
 * \param params Configuration space
 * \param mask Format mask
 * \return access type
 */
int snd_pcm_hw_params_set_format_mask(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_mask_t *mask)
{
	return snd_pcm_hw_param_set_mask(pcm, params, SND_TRY, SND_PCM_HW_PARAM_FORMAT, (snd_mask_t *) mask);
}

/**
 * \brief Get format mask from a configuration space
 * \param params Configuration space
 * \param mask Returned Format mask
 */
void snd_pcm_hw_params_get_format_mask(snd_pcm_hw_params_t *params, snd_pcm_format_mask_t *mask)
{
	snd_pcm_format_mask_copy(mask, snd_pcm_hw_param_get_mask(params, SND_PCM_HW_PARAM_FORMAT));
}


/**
 * \brief Verify if a subformat is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val subformat
 * \return 1 if available 0 otherwise
 */
int snd_pcm_hw_params_test_subformat(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_subformat_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_SUBFORMAT, val, 0);
}

/**
 * \brief Extract subformat from a configuration space
 * \param params Configuration space
 * \return subformat otherwise a negative error code if not exactly one is present
 */
int snd_pcm_hw_params_get_subformat(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_SUBFORMAT, NULL);
}

/**
 * \brief Restrict a configuration space to contain only one subformat
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val subformat
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_subformat(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_subformat_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_SUBFORMAT, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only its first subformat
 * \param pcm PCM handle
 * \param params Configuration space
 * \return subformat
 */
snd_pcm_subformat_t snd_pcm_hw_params_set_subformat_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_SUBFORMAT, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its last subformat
 * \param pcm PCM handle
 * \param params Configuration space
 * \return subformat
 */
snd_pcm_subformat_t snd_pcm_hw_params_set_subformat_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_SUBFORMAT, NULL);
}

/**
 * \brief Restrict a configuration space to contain only a set of subformats
 * \param pcm PCM handle
 * \param params Configuration space
 * \param mask Subformat mask
 * \return access type
 */
int snd_pcm_hw_params_set_subformat_mask(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_subformat_mask_t *mask)
{
	return snd_pcm_hw_param_set_mask(pcm, params, SND_TRY, SND_PCM_HW_PARAM_SUBFORMAT, (snd_mask_t *) mask);
}

/**
 * \brief Get subformat mask from a configuration space
 * \param params Configuration space
 * \param mask Returned Subformat mask
 */
void snd_pcm_hw_params_get_subformat_mask(snd_pcm_hw_params_t *params, snd_pcm_subformat_mask_t *mask)
{
	snd_pcm_subformat_mask_copy(mask, snd_pcm_hw_param_get_mask(params, SND_PCM_HW_PARAM_SUBFORMAT));
}


/**
 * \brief Extract channels from a configuration space
 * \param params Configuration space
 * \return channels count otherwise a negative error code if not exactly one is present
 */
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_CHANNELS, NULL);
}

/**
 * \brief Extract minimum channels count from a configuration space
 * \param params Configuration space
 * \return minimum channels count
 */
unsigned int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_CHANNELS, NULL);
}

/**
 * \brief Extract maximum channels count from a configuration space
 * \param params Configuration space
 * \return maximum channels count
 */
unsigned int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_CHANNELS, NULL);
}

/**
 * \brief Verify if a channels count is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val channels count
 * \return 1 if available 0 otherwise
 */
int snd_pcm_hw_params_test_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_CHANNELS, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only one channels count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val channels count
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_CHANNELS, val, 0);
}

/**
 * \brief Restrict a configuration space with a minimum channels count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val minimum channels count (on return filled with actual minimum)
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_channels_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_CHANNELS, val, NULL);
}

/**
 * \brief Restrict a configuration space with a maximum channels count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val maximum channels count (on return filled with actual maximum)
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_channels_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_CHANNELS, val, NULL);
}

/**
 * \brief Restrict a configuration space to have channels counts in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min minimum channels count (on return filled with actual minimum)
 * \param max maximum channels count (on return filled with actual maximum)
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_channels_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, unsigned int *max)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_CHANNELS, min, NULL, max, NULL);
}

/**
 * \brief Restrict a configuration space to have channels count nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val target channels count
 * \return choosen channels count
 */
unsigned int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_CHANNELS, val, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its minimum channels count
 * \param pcm PCM handle
 * \param params Configuration space
 * \return channels count
 */
unsigned int snd_pcm_hw_params_set_channels_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_CHANNELS, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its maximum channels count
 * \param pcm PCM handle
 * \param params Configuration space
 * \return channels count
 */
unsigned int snd_pcm_hw_params_set_channels_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_CHANNELS, NULL);
}


/**
 * \brief Extract rate from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate rate otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_RATE, dir);
}

/**
 * \brief Extract minimum rate from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum rate
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_rate_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_RATE, dir);
}

/**
 * \brief Extract maximum rate from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum rate
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_rate_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_RATE, dir);
}

/**
 * \brief Verify if a rate is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate rate
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_RATE, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one rate
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate rate
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_RATE, val, dir);
}

/**
 * \brief Restrict a configuration space with a minimum rate
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum rate (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_rate_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_RATE, val, dir);
}

/**
 * \brief Restrict a configuration space with a maximum rate
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum rate (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact maximum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_rate_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_RATE, val, dir);
}

/**
 * \brief Restrict a configuration space to have rates in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum rate (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum rate (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_rate_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, int *mindir, unsigned int *max, int *maxdir)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_RATE, min, mindir, max, maxdir);
}

/**
 * \brief Restrict a configuration space to have rate nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target rate
 * \return approximate choosen rate
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_RATE, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum rate
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate rate
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_rate_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_RATE, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum rate
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate rate
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_rate_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_RATE, dir);
}


/**
 * \brief Extract period time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period duration in us otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
int snd_pcm_hw_params_get_period_time(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_PERIOD_TIME, dir);
}

/**
 * \brief Extract minimum period time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum period duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_period_time_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_PERIOD_TIME, dir);
}

/**
 * \brief Extract maximum period time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum period duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_period_time_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_PERIOD_TIME, dir);
}

/**
 * \brief Verify if a period time is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate period duration in us
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_period_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_PERIOD_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one period time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate period duration in us
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_TIME, val, dir);
}


/**
 * \brief Restrict a configuration space with a minimum period time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum period duration in us (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_time_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space with a maximum period time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum period duration in us (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact maximum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_time_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to have period times in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum period duration in us (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum period duration in us (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_time_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, int *mindir, unsigned int *max, int *maxdir)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_TIME, min, mindir, max, maxdir);
}

/**
 * \brief Restrict a configuration space to have period time nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target period duration in us
 * \return approximate choosen period duration in us
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_PERIOD_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum period time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_period_time_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_PERIOD_TIME, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum period time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_period_time_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_PERIOD_TIME, dir);
}


/**
 * \brief Extract period size from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period size in frames otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
snd_pcm_sframes_t snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_PERIOD_SIZE, dir);
}

/**
 * \brief Extract minimum period size from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum period size in frames
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_get_period_size_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_PERIOD_SIZE, dir);
}

/**
 * \brief Extract maximum period size from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum period size in frames
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_get_period_size_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_PERIOD_SIZE, dir);
}

/**
 * \brief Verify if a period size is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate period size in frames
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_PERIOD_SIZE, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one period size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate period size in frames
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_SIZE, val, dir);
}

/**
 * \brief Restrict a configuration space with a minimum period size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum period size in frames (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_size_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
	unsigned int _val = *val;
	int err = snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_SIZE, &_val, dir);
	*val = _val;
	return err;
}

/**
 * \brief Restrict a configuration space with a maximum period size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum period size in frames (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_size_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
	unsigned int _val = *val;
	int err = snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_SIZE, &_val, dir);
	*val = _val;
	return err;
}

/**
 * \brief Restrict a configuration space to have period sizes in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum period size in frames (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum period size in frames (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_period_size_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *min, int *mindir, snd_pcm_uframes_t *max, int *maxdir)
{
	unsigned int _min = *min;
	unsigned int _max = *max;
	int err = snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_SIZE, &_min, mindir, &_max, maxdir);
	*min = _min;
	*max = _max;
	return err;
}

/**
 * \brief Restrict a configuration space to have period size nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target period size in frames
 * \return approximate choosen period size in frames
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_PERIOD_SIZE, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum period size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period size in frames
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_period_size_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_PERIOD_SIZE, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum period size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate period size in frames
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_period_size_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_PERIOD_SIZE, dir);
}

/**
 * \brief Restrict a configuration space to contain only integer period sizes
 * \param pcm PCM handle
 * \param params Configuration space
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_period_size_integer(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_integer(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIOD_SIZE);
}


/**
 * \brief Extract periods from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate periods per buffer otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
int snd_pcm_hw_params_get_periods(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_PERIODS, dir);
}

/**
 * \brief Extract minimum periods count from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum periods per buffer
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_periods_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_PERIODS, dir);
}

/**
 * \brief Extract maximum periods count from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum periods per buffer
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_periods_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_PERIODS, dir);
}

/**
 * \brief Verify if a periods count is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate periods per buffer
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_periods(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_PERIODS, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one periods count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate periods per buffer
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_periods(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIODS, val, dir);
}

/**
 * \brief Restrict a configuration space with a minimum periods count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum periods per buffer (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_periods_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIODS, val, dir);
}

/**
 * \brief Restrict a configuration space with a maximum periods count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum periods per buffer (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact maximum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_periods_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIODS, val, dir);
}

/**
 * \brief Restrict a configuration space to have periods counts in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum periods per buffer (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum periods per buffer (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_periods_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, int *mindir, unsigned int *max, int *maxdir)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIODS, min, mindir, max, maxdir);
}

/**
 * \brief Restrict a configuration space to have periods count nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target periods per buffer
 * \return approximate choosen periods per buffer
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_PERIODS, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum periods count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate periods per buffer
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_periods_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_PERIODS, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum periods count
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate periods per buffer
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_periods_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_PERIODS, dir);
}

/**
 * \brief Restrict a configuration space to contain only integer periods counts
 * \param pcm PCM handle
 * \param params Configuration space
 * \return 0 otherwise a negative error code if configuration space would become empty
 */
int snd_pcm_hw_params_set_periods_integer(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_integer(pcm, params, SND_TRY, SND_PCM_HW_PARAM_PERIODS);
}


/**
 * \brief Extract buffer time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate buffer duration in us otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
int snd_pcm_hw_params_get_buffer_time(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_BUFFER_TIME, dir);
}

/**
 * \brief Extract minimum buffer time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum buffer duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_buffer_time_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_BUFFER_TIME, dir);
}

/**
 * \brief Extract maximum buffer time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum buffer duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_buffer_time_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_BUFFER_TIME, dir);
}

/**
 * \brief Verify if a buffer time is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate buffer duration in us
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_buffer_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_BUFFER_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one buffer time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate buffer duration in us
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space with a minimum buffer time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum buffer duration in us (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_time_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space with a maximum buffer time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum buffer duration in us (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact maximum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_time_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to have buffer times in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum buffer duration in us (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum buffer duration in us (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_time_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, int *mindir, unsigned int *max, int *maxdir)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_TIME, min, mindir, max, maxdir);
}

/**
 * \brief Restrict a configuration space to have buffer time nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target buffer duration in us
 * \return approximate choosen buffer duration in us
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_BUFFER_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum buffer time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate buffer duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_buffer_time_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_BUFFER_TIME, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum bufferd time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate buffer duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_buffer_time_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_BUFFER_TIME, dir);
}


/**
 * \brief Extract buffer size from a configuration space
 * \param params Configuration space
 * \return buffer size in frames otherwise a negative error code if not exactly one is present
 */
snd_pcm_sframes_t snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_BUFFER_SIZE, NULL);
}

/**
 * \brief Extract minimum buffer size from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum buffer size in frames
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_get_buffer_size_min(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_BUFFER_SIZE, NULL);
}

/**
 * \brief Extract maximum buffer size from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum buffer size in frames
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_get_buffer_size_max(const snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_BUFFER_SIZE, NULL);
}

/**
 * \brief Verify if a buffer size is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val buffer size in frames
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_BUFFER_SIZE, val, 0);
}

/**
 * \brief Restrict a configuration space to contain only one buffer size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val buffer size in frames
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_SIZE, val, 0);
}

/**
 * \brief Restrict a configuration space with a minimum buffer size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum buffer size in frames (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_size_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
	unsigned int _val = *val;
	int err = snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_SIZE, &_val, NULL);
	*val = _val;
	return err;
}

/**
 * \brief Restrict a configuration space with a maximum buffer size
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum buffer size in frames (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_size_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
	unsigned int _val = *val;
	int err = snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_SIZE, &_val, NULL);
	*val = _val;
	return err;
}

/**
 * \brief Restrict a configuration space to have buffer sizes in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum buffer size in frames (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum buffer size in frames (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_buffer_size_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *min, snd_pcm_uframes_t *max)
{
	unsigned int _min = *min;
	unsigned int _max = *max;
	int err = snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_BUFFER_SIZE, &_min, NULL, &_max, NULL);
	*min = _min;
	*max = _max;
	return err;
}

/**
 * \brief Restrict a configuration space to have buffer size nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target buffer size in frames
 * \return approximate choosen buffer size in frames
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_BUFFER_SIZE, val, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its minimum buffer size
 * \param pcm PCM handle
 * \param params Configuration space
 * \return buffer size in frames
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_buffer_size_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_BUFFER_SIZE, NULL);
}

/**
 * \brief Restrict a configuration space to contain only its maximum buffer size
 * \param pcm PCM handle
 * \param params Configuration space
 * \return buffer size in frames
 */
snd_pcm_uframes_t snd_pcm_hw_params_set_buffer_size_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_BUFFER_SIZE, NULL);
}


/**
 * \brief Extract tick time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate tick duration in us otherwise a negative error code if not exactly one is present
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
int snd_pcm_hw_params_get_tick_time(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get(params, SND_PCM_HW_PARAM_TICK_TIME, dir);
}

/**
 * \brief Extract minimum tick time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate minimum tick duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_tick_time_min(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_min(params, SND_PCM_HW_PARAM_TICK_TIME, dir);
}

/**
 * \brief Extract maximum tick time from a configuration space
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate maximum tick duration in us
 *
 * Exact value is <,=,> the returned one following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_get_tick_time_max(const snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_get_max(params, SND_PCM_HW_PARAM_TICK_TIME, dir);
}

/**
 * \brief Verify if a tick time is available inside a configuration space for a PCM
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate tick duration in us
 * \param dir Sub unit direction
 * \return 1 if available 0 otherwise
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_test_tick_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TEST, SND_PCM_HW_PARAM_TICK_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only one tick time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate tick duration in us
 * \param dir Sub unit direction
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted exact value is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_tick_time(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
	return snd_pcm_hw_param_set(pcm, params, SND_TRY, SND_PCM_HW_PARAM_TICK_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space with a minimum tick time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate minimum tick duration in us (on return filled with actual minimum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact minimum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_tick_time_min(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_min(pcm, params, SND_TRY, SND_PCM_HW_PARAM_TICK_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space with a maximum tick time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate maximum tick duration in us (on return filled with actual maximum)
 * \param dir Sub unit direction (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact maximum is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_tick_time_max(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
	return snd_pcm_hw_param_set_max(pcm, params, SND_TRY, SND_PCM_HW_PARAM_TICK_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to have tick times in a given range
 * \param pcm PCM handle
 * \param params Configuration space
 * \param min approximate minimum tick duration in us (on return filled with actual minimum)
 * \param mindir Sub unit direction for minimum (on return filled with actual direction)
 * \param max approximate maximum tick duration in us (on return filled with actual maximum)
 * \param maxdir Sub unit direction for maximum (on return filled with actual direction)
 * \return 0 otherwise a negative error code if configuration space would become empty
 *
 * Wanted/actual exact min/max is <,=,> val following dir (-1,0,1)
 */
int snd_pcm_hw_params_set_tick_time_minmax(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *min, int *mindir, unsigned int *max, int *maxdir)
{
	return snd_pcm_hw_param_set_minmax(pcm, params, SND_TRY, SND_PCM_HW_PARAM_TICK_TIME, min, mindir, max, maxdir);
}

/**
 * \brief Restrict a configuration space to have tick time nearest to a target
 * \param pcm PCM handle
 * \param params Configuration space
 * \param val approximate target tick duration in us
 * \return approximate choosen tick duration in us
 *
 * target/choosen exact value is <,=,> val following dir (-1,0,1)
 */
unsigned int snd_pcm_hw_params_set_tick_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int *dir)
{
	return snd_pcm_hw_param_set_near(pcm, params, SND_PCM_HW_PARAM_TICK_TIME, val, dir);
}

/**
 * \brief Restrict a configuration space to contain only its minimum tick time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate tick duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_tick_time_first(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_first(pcm, params, SND_PCM_HW_PARAM_TICK_TIME, dir);
}

/**
 * \brief Restrict a configuration space to contain only its maximum tick time
 * \param pcm PCM handle
 * \param params Configuration space
 * \param dir Sub unit direction
 * \return approximate tick duration in us
 *
 * Actual exact value is <,=,> the approximate one following dir (-1, 0, 1)
 */
unsigned int snd_pcm_hw_params_set_tick_time_last(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int *dir)
{
	return snd_pcm_hw_param_set_last(pcm, params, SND_PCM_HW_PARAM_TICK_TIME, dir);
}

/**
 * \brief Return current software configuration for a PCM
 * \param pcm PCM handle
 * \param params Software configuration container
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
	assert(pcm && params);
	assert(pcm->setup);
	params->tstamp_mode = pcm->tstamp_mode;
	params->period_step = pcm->period_step;
	params->sleep_min = pcm->sleep_min;
	params->avail_min = pcm->avail_min;
	params->xfer_align = pcm->xfer_align;
	params->start_threshold = pcm->start_threshold;
	params->stop_threshold = pcm->stop_threshold;
	params->silence_threshold = pcm->silence_threshold;
	params->silence_size = pcm->silence_size;
	params->boundary = pcm->boundary;
	return 0;
}

/**
 * \brief Dump a software configuration
 * \param params Software configuration container
 * \param out Output handle
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_sw_params_dump(snd_pcm_sw_params_t *params, snd_output_t *out)
{
	snd_output_printf(out, "start_mode: %s\n", snd_pcm_start_mode_name(snd_pcm_sw_params_get_start_mode(params)));
	snd_output_printf(out, "xrun_mode: %s\n", snd_pcm_xrun_mode_name(snd_pcm_sw_params_get_xrun_mode(params)));
	snd_output_printf(out, "tstamp_mode: %s\n", snd_pcm_tstamp_mode_name(snd_pcm_sw_params_get_tstamp_mode(params)));
	snd_output_printf(out, "period_step: %u\n", params->period_step);
	snd_output_printf(out, "sleep_min: %u\n", params->sleep_min);
	snd_output_printf(out, "avail_min: %lu\n", params->avail_min);
	snd_output_printf(out, "xfer_align: %lu\n", params->xfer_align);
	snd_output_printf(out, "silence_threshold: %lu\n", params->silence_threshold);
	snd_output_printf(out, "silence_size: %lu\n", params->silence_size);
	snd_output_printf(out, "boundary: %lu\n", params->boundary);
	return 0;
}

/**
 * \brief get size of #snd_pcm_sw_params_t
 * \return size in bytes
 */
size_t snd_pcm_sw_params_sizeof()
{
	return sizeof(snd_pcm_sw_params_t);
}

/**
 * \brief allocate an invalid #snd_pcm_sw_params_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_sw_params_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_sw_params_t
 * \param pointer to object to free
 */
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_sw_params_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_sw_params_copy(snd_pcm_sw_params_t *dst, const snd_pcm_sw_params_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief (DEPRECATED) Set start mode inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Start mode
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_start_mode(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_start_t val)
{
	assert(pcm && params);
	switch (val) {
	case SND_PCM_START_DATA:
		params->start_threshold = 1;
		break;
	case SND_PCM_START_EXPLICIT:
		params->start_threshold = pcm->boundary;
		break;
	default:
		assert(0);
		break;
	}
	return 0;
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_sw_params_set_start_mode, "Warning: start_mode is deprecated, consider to use start_threshold");
#endif

/**
 * \brief (DEPRECATED) Get start mode from a software configuration container
 * \param params Software configuration container
 * \return start mode
 */
snd_pcm_start_t snd_pcm_sw_params_get_start_mode(const snd_pcm_sw_params_t *params)
{
	assert(params);
	/* FIXME: Ugly */
	return params->start_threshold > 1024 * 1024 ? SND_PCM_START_EXPLICIT : SND_PCM_START_DATA;
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_sw_params_get_start_mode, "Warning: start_mode is deprecated, consider to use start_threshold");
#endif

/**
 * \brief (DEPRECATED) Set xrun mode inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Xrun mode
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_xrun_mode(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_xrun_t val)
{
	assert(pcm && params);
	switch (val) {
	case SND_PCM_XRUN_STOP:
		params->stop_threshold = pcm->buffer_size;
		break;
	case SND_PCM_XRUN_NONE:
		params->stop_threshold = pcm->boundary;
		break;
	default:
		assert(0);
		break;
	}
	return 0;
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_sw_params_set_xrun_mode, "Warning: xrun_mode is deprecated, consider to use stop_threshold");
#endif

/**
 * \brief (DEPRECATED) Get xrun mode from a software configuration container
 * \param params Software configuration container
 * \return xrun mode
 */
snd_pcm_xrun_t snd_pcm_sw_params_get_xrun_mode(const snd_pcm_sw_params_t *params)
{
	assert(params);
	/* FIXME: Ugly */
	return params->stop_threshold > 1024 * 1024 ? SND_PCM_XRUN_NONE : SND_PCM_XRUN_STOP;
}

#ifndef DOC_HIDDEN
link_warning(snd_pcm_sw_params_get_xrun_mode, "Warning: xrun_mode is deprecated, consider to use stop_threshold");
#endif

/**
 * \brief Set timestamp mode inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Timestamp mode
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_tstamp_mode(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_tstamp_t val)
{
	assert(pcm && params);
	assert(val <= SND_PCM_TSTAMP_LAST);
	params->tstamp_mode = val;
	return 0;
}

/**
 * \brief Get timestamp mode from a software configuration container
 * \param params Software configuration container
 * \return timestamp mode
 */
snd_pcm_tstamp_t snd_pcm_sw_params_get_tstamp_mode(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->tstamp_mode;
}


#if 0
int snd_pcm_sw_params_set_period_step(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, unsigned int val)
{
	assert(pcm && params);
	params->period_step = val;
	return 0;
}

unsigned int snd_pcm_sw_params_get_period_step(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->period_step;
}
#endif


/**
 * \brief Set minimum number of ticks to sleep inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Minimum ticks to sleep or 0 to disable the use of tick timer
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_sleep_min(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, unsigned int val)
{
	assert(pcm && params);
	params->sleep_min = val;
	return 0;
}

/**
 * \brief Get minimum numbers of ticks to sleep from a software configuration container
 * \param params Software configuration container
 * \return minimum number of ticks to sleep or 0 if tick timer is disabled
 */
unsigned int snd_pcm_sw_params_get_sleep_min(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->sleep_min;
}

/**
 * \brief Set avail min inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Minimum avail frames to consider PCM ready
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	params->avail_min = val;
	return 0;
}

/**
 * \brief Get avail min from a software configuration container
 * \param params Software configuration container
 * \return minimum available frames to consider PCM ready
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_avail_min(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->avail_min;
}


/**
 * \brief Set xfer align inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Chunk size (frames are attempted to be transferred in chunks)
 * \return 0 otherwise a negative error code
 */
int snd_pcm_sw_params_set_xfer_align(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	assert(val % pcm->min_align == 0);
	params->xfer_align = val;
	return 0;
}

/**
 * \brief Get xfer align from a software configuration container
 * \param params Software configuration container
 * \return Chunk size (frames are attempted to be transferred in chunks)
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_xfer_align(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->xfer_align;
}


/**
 * \brief Set start threshold inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Start threshold in frames 
 * \return 0 otherwise a negative error code
 *
 * PCM is automatically started when playback frames available to PCM 
 * are >= threshold or when requested capture frames are >= threshold
 */
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	params->start_threshold = val;
	return 0;
}

/**
 * \brief Get start threshold from a software configuration container
 * \param params Software configuration container
 * \return Start threshold in frames
 *
 * PCM is automatically started when playback frames available to PCM 
 * are >= threshold or when requested capture frames are >= threshold
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_start_threshold(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->start_threshold;
}

/**
 * \brief Set stop threshold inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Stop threshold in frames
 * \return 0 otherwise a negative error code
 *
 * PCM is automatically stopped in #SND_PCM_STATE_XRUN state when available
 * frames is >= threshold
 */
int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	params->stop_threshold = val;
	return 0;
}

/**
 * \brief Get stop threshold from a software configuration container
 * \param params Software configuration container
 * \return Stop threshold in frames
 *
 * PCM is automatically stopped in #SND_PCM_STATE_XRUN state when available
 * frames is >= threshold
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_stop_threshold(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->stop_threshold;
}

/**
 * \brief Set silence threshold inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Silence threshold in frames 
 * \return 0 otherwise a negative error code
 *
 * A portion of playback buffer is overwritten with silence (see 
 * #snd_pcm_sw_params_set_silence_size) when playback underrun is nearer
 * than silence threshold
 */
int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	assert(val + params->silence_size <= pcm->buffer_size);
	params->silence_threshold = val;
	return 0;
}

/**
 * \brief Get silence threshold from a software configuration container
 * \param params Software configuration container
 * \return Silence threshold in frames
 *
 * A portion of playback buffer is overwritten with silence (see 
 * #snd_pcm_sw_params_get_silence_size) when playback underrun is nearer
 * than silence threshold
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_silence_threshold(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->silence_threshold;
}


/**
 * \brief Set silence size inside a software configuration container
 * \param pcm PCM handle
 * \param params Software configuration container
 * \param val Silence size in frames (0 for disabled)
 * \return 0 otherwise a negative error code
 *
 * A portion of playback buffer is overwritten with silence when playback
 * underrun is nearer than silence threshold (see 
 * #snd_pcm_sw_params_set_silence_threshold)
 */
int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
	assert(pcm && params);
	assert(val + params->silence_threshold <= pcm->buffer_size);
	params->silence_size = val;
	return 0;
}

/**
 * \brief Get silence size from a software configuration container
 * \param params Software configuration container
 * \return Silence size in frames (0 for disabled)
 *
 * A portion of playback buffer is overwritten with silence when playback
 * underrun is nearer than silence threshold (see 
 * #snd_pcm_sw_params_set_silence_threshold)
 */
snd_pcm_uframes_t snd_pcm_sw_params_get_silence_size(const snd_pcm_sw_params_t *params)
{
	assert(params);
	return params->silence_size;
}


/**
 * \brief get size of #snd_pcm_status_t
 * \return size in bytes
 */
size_t snd_pcm_status_sizeof()
{
	return sizeof(snd_pcm_status_t);
}

/**
 * \brief allocate an invalid #snd_pcm_status_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_status_malloc(snd_pcm_status_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_status_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_status_t
 * \param pointer to object to free
 */
void snd_pcm_status_free(snd_pcm_status_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_status_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_status_copy(snd_pcm_status_t *dst, const snd_pcm_status_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/** 
 * \brief Get state from a PCM status container (see #snd_pcm_state)
 * \return PCM state
 */
snd_pcm_state_t snd_pcm_status_get_state(const snd_pcm_status_t *obj)
{
	assert(obj);
	return obj->state;
}

/** 
 * \brief Get trigger timestamp from a PCM status container
 * \param ptr Pointer to returned timestamp
 */
void snd_pcm_status_get_trigger_tstamp(const snd_pcm_status_t *obj, snd_timestamp_t *ptr)
{
	assert(obj && ptr);
	*ptr = obj->trigger_tstamp;
}

/** 
 * \brief Get "now" timestamp from a PCM status container
 * \param ptr Pointer to returned timestamp
 */
void snd_pcm_status_get_tstamp(const snd_pcm_status_t *obj, snd_timestamp_t *ptr)
{
	assert(obj && ptr);
	*ptr = obj->tstamp;
}

/** 
 * \brief Get delay from a PCM status container (see #snd_pcm_delay)
 * \return Delay in frames
 *
 * Delay is distance between current application frame position and
 * sound frame position.
 * It's positive and less than buffer size in normal situation,
 * negative on playback underrun and greater than buffer size on
 * capture overrun.
 */
snd_pcm_sframes_t snd_pcm_status_get_delay(const snd_pcm_status_t *obj)
{
	assert(obj);
	return obj->delay;
}

/** 
 * \brief Get number of frames available from a PCM status container (see #snd_pcm_avail_update)
 * \return Number of frames ready to be read/written
 */
snd_pcm_uframes_t snd_pcm_status_get_avail(const snd_pcm_status_t *obj)
{
	assert(obj);
	return obj->avail;
}

/** 
 * \brief Get maximum number of frames available from a PCM status container after last #snd_pcm_status call
 * \return Maximum number of frames ready to be read/written
 */
snd_pcm_uframes_t snd_pcm_status_get_avail_max(const snd_pcm_status_t *obj)
{
	assert(obj);
	return obj->avail_max;
}

/**
 * \brief get size of #snd_pcm_info_t
 * \return size in bytes
 */
size_t snd_pcm_info_sizeof()
{
	return sizeof(snd_pcm_info_t);
}

/**
 * \brief allocate an invalid #snd_pcm_info_t using standard malloc
 * \param ptr returned pointer
 * \return 0 on success otherwise negative error code
 */
int snd_pcm_info_malloc(snd_pcm_info_t **ptr)
{
	assert(ptr);
	*ptr = calloc(1, sizeof(snd_pcm_info_t));
	if (!*ptr)
		return -ENOMEM;
	return 0;
}

/**
 * \brief frees a previously allocated #snd_pcm_info_t
 * \param pointer to object to free
 */
void snd_pcm_info_free(snd_pcm_info_t *obj)
{
	free(obj);
}

/**
 * \brief copy one #snd_pcm_info_t to another
 * \param dst pointer to destination
 * \param src pointer to source
 */
void snd_pcm_info_copy(snd_pcm_info_t *dst, const snd_pcm_info_t *src)
{
	assert(dst && src);
	*dst = *src;
}

/**
 * \brief Get device from a PCM info container
 * \param obj PCM info container
 * \return device number
 */
unsigned int snd_pcm_info_get_device(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->device;
}

/**
 * \brief Get subdevice from a PCM info container
 * \param obj PCM info container
 * \return subdevice number
 */
unsigned int snd_pcm_info_get_subdevice(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->subdevice;
}

/**
 * \brief Get stream (direction) from a PCM info container
 * \param obj PCM info container
 * \return stream
 */
snd_pcm_stream_t snd_pcm_info_get_stream(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->stream;
}

/**
 * \brief Get card from a PCM info container
 * \param obj PCM info container
 * \return card number otherwise a negative error code if not associable to a card
 */
int snd_pcm_info_get_card(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->card;
}

/**
 * \brief Get id from a PCM info container
 * \param obj PCM info container
 * \return short id of PCM
 */
const char *snd_pcm_info_get_id(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->id;
}

/**
 * \brief Get name from a PCM info container
 * \param obj PCM info container
 * \return name of PCM
 */
const char *snd_pcm_info_get_name(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->name;
}

/**
 * \brief Get subdevice name from a PCM info container
 * \param obj PCM info container
 * \return name of used PCM subdevice
 */
const char *snd_pcm_info_get_subdevice_name(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->subname;
}

/**
 * \brief Get class from a PCM info container
 * \param obj PCM info container
 * \return class of PCM
 */
snd_pcm_class_t snd_pcm_info_get_class(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->dev_class;
}

/**
 * \brief Get subclass from a PCM info container
 * \param obj PCM info container
 * \return subclass of PCM
 */
snd_pcm_subclass_t snd_pcm_info_get_subclass(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->dev_subclass;
}

/**
 * \brief Get subdevices count from a PCM info container
 * \param obj PCM info container
 * \return subdevices total count of PCM
 */
unsigned int snd_pcm_info_get_subdevices_count(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->subdevices_count;
}

/**
 * \brief Get available subdevices count from a PCM info container
 * \param obj PCM info container
 * \return available subdevices count of PCM
 */
unsigned int snd_pcm_info_get_subdevices_avail(const snd_pcm_info_t *obj)
{
	assert(obj);
	return obj->subdevices_avail;
}

/**
 * \brief Set wanted device inside a PCM info container (see #snd_ctl_pcm_info)
 * \param obj PCM info container
 * \param val Device number
 */
void snd_pcm_info_set_device(snd_pcm_info_t *obj, unsigned int val)
{
	assert(obj);
	obj->device = val;
}

/**
 * \brief Set wanted subdevice inside a PCM info container (see #snd_ctl_pcm_info)
 * \param obj PCM info container
 * \param val Subdevice number
 */
void snd_pcm_info_set_subdevice(snd_pcm_info_t *obj, unsigned int val)
{
	assert(obj);
	obj->subdevice = val;
}

/**
 * \brief Set wanted stream inside a PCM info container (see #snd_ctl_pcm_info)
 * \param obj PCM info container
 * \param val Stream
 */
void snd_pcm_info_set_stream(snd_pcm_info_t *obj, snd_pcm_stream_t val)
{
	assert(obj);
	obj->stream = val;
}

/**
 * \brief Application request to access a portion of mmap area
 * \param pcm PCM handle
 * \param areas Returned mmap channel areas
 * \param offset Returned mmap area offset
 * \param size mmap area portion size (wanted on entry, contiguous
available on exit)
 * \return 0 on success otherwise a negative error code
 */
int snd_pcm_mmap_begin(snd_pcm_t *pcm,
		       const snd_pcm_channel_area_t **areas,
		       snd_pcm_uframes_t *offset,
		       snd_pcm_uframes_t *frames)
{
	snd_pcm_uframes_t cont;
	snd_pcm_uframes_t avail;
	snd_pcm_uframes_t f;
	assert(pcm && areas && offset && frames);
	if (pcm->stopped_areas &&
	    snd_pcm_state(pcm) != SND_PCM_STATE_RUNNING) 
		*areas = pcm->stopped_areas;
	else
		*areas = pcm->running_areas;
	*offset = *pcm->appl_ptr % pcm->buffer_size;
	cont = pcm->buffer_size - *offset;
	avail = snd_pcm_mmap_avail(pcm);
	f = *frames;
	if (f > avail)
		f = avail;
	if (f > cont)
		f = cont;
	*frames = f;
	return 0;
}

/**
 * \brief Application has completed the access to area requested with
#snd_pcm_mmap_begin
 * \param pcm PCM handle
 * \return 0 on success otherwise a negative error code
 *
 * To call this with offset/frames values different from that returned
 * by snd_pcm_mmap_begin has undefined effects and it has to be avoided.
 */
int snd_pcm_mmap_commit(snd_pcm_t *pcm, snd_pcm_uframes_t offset,
			snd_pcm_uframes_t frames)
{
	assert(pcm);
	assert(offset == *pcm->appl_ptr % pcm->buffer_size);
	assert(frames <= snd_pcm_mmap_avail(pcm));
	return pcm->fast_ops->mmap_commit(pcm->fast_op_arg, offset, frames);
}

#ifndef DOC_HIDDEN

int _snd_pcm_poll_descriptor(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->poll_fd;
}

void snd_pcm_areas_from_buf(snd_pcm_t *pcm, snd_pcm_channel_area_t *areas, 
			    void *buf)
{
	unsigned int channel;
	unsigned int channels = pcm->channels;
	for (channel = 0; channel < channels; ++channel, ++areas) {
		areas->addr = buf;
		areas->first = channel * pcm->sample_bits;
		areas->step = pcm->frame_bits;
	}
}

void snd_pcm_areas_from_bufs(snd_pcm_t *pcm, snd_pcm_channel_area_t *areas, 
			     void **bufs)
{
	unsigned int channel;
	unsigned int channels = pcm->channels;
	for (channel = 0; channel < channels; ++channel, ++areas, ++bufs) {
		areas->addr = *bufs;
		areas->first = 0;
		areas->step = pcm->sample_bits;
	}
}

snd_pcm_sframes_t snd_pcm_read_areas(snd_pcm_t *pcm, const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset, snd_pcm_uframes_t size,
				     snd_pcm_xfer_areas_func_t func)
{
	snd_pcm_uframes_t xfer = 0;
	int err = 0;
	snd_pcm_state_t state = snd_pcm_state(pcm);

	if (size == 0)
		return 0;
	if (size > pcm->xfer_align)
		size -= size % pcm->xfer_align;

	switch (state) {
	case SND_PCM_STATE_PREPARED:
		if (size >= pcm->start_threshold) {
			err = snd_pcm_start(pcm);
			if (err < 0)
				goto _end;
		}
		break;
	case SND_PCM_STATE_DRAINING:
	case SND_PCM_STATE_RUNNING:
		break;
	case SND_PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADFD;
	}

	while (size > 0) {
		snd_pcm_uframes_t frames;
		snd_pcm_sframes_t avail;
	_again:
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0) {
			err = -EPIPE;
			goto _end;
		}
		if (state == SND_PCM_STATE_DRAINING) {
			if (avail == 0) {
				err = -EPIPE;
				goto _end;
			}
		} else if (avail == 0 ||
			   (size >= pcm->xfer_align && 
			    (snd_pcm_uframes_t) avail < pcm->xfer_align)) {
			if (pcm->mode & SND_PCM_NONBLOCK) {
				err = -EAGAIN;
				goto _end;
			}

			err = snd_pcm_wait(pcm, -1);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
			goto _again;
			
		}
		if ((snd_pcm_uframes_t) avail > pcm->xfer_align)
			avail -= avail % pcm->xfer_align;
		frames = size;
		if (frames > (snd_pcm_uframes_t) avail)
			frames = avail;
		assert(frames != 0);
		err = func(pcm, areas, offset, frames);
		if (err < 0)
			break;
		assert((snd_pcm_uframes_t)err == frames);
		offset += frames;
		size -= frames;
		xfer += frames;
#if 0
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = -EPIPE;
			goto _end;
		}
#endif
	}
 _end:
	return xfer > 0 ? (snd_pcm_sframes_t) xfer : err;
}

snd_pcm_sframes_t snd_pcm_write_areas(snd_pcm_t *pcm, const snd_pcm_channel_area_t *areas,
				      snd_pcm_uframes_t offset, snd_pcm_uframes_t size,
				      snd_pcm_xfer_areas_func_t func)
{
	snd_pcm_uframes_t xfer = 0;
	int err = 0;
	snd_pcm_state_t state = snd_pcm_state(pcm);

	if (size == 0)
		return 0;
	if (size > pcm->xfer_align)
		size -= size % pcm->xfer_align;

	switch (state) {
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		break;
	case SND_PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADFD;
	}

	while (size > 0) {
		snd_pcm_uframes_t frames;
		snd_pcm_sframes_t avail;
	_again:
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0) {
			err = -EPIPE;
			goto _end;
		}
		if (state == SND_PCM_STATE_PREPARED) {
			if (avail == 0) {
				err = -EPIPE;
				goto _end;
			}
		} else if (avail == 0 ||
			   (size >= pcm->xfer_align && 
			    (snd_pcm_uframes_t) avail < pcm->xfer_align)) {
			if (pcm->mode & SND_PCM_NONBLOCK) {
				err = -EAGAIN;
				goto _end;
			}

			err = snd_pcm_wait(pcm, -1);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
			goto _again;
			
		}
		if ((snd_pcm_uframes_t) avail > pcm->xfer_align)
			avail -= avail % pcm->xfer_align;
		frames = size;
		if (frames > (snd_pcm_uframes_t) avail)
			frames = avail;
		assert(frames != 0);
		err = func(pcm, areas, offset, frames);
		if (err < 0)
			break;
		assert((snd_pcm_uframes_t)err == frames);
		offset += frames;
		size -= frames;
		xfer += frames;
#if 0
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = -EPIPE;
			goto _end;
		}
#endif
		if (state == SND_PCM_STATE_PREPARED) {
			snd_pcm_sframes_t hw_avail = pcm->buffer_size - avail;
			hw_avail += frames;
			if (hw_avail >= (snd_pcm_sframes_t) pcm->start_threshold) {
				err = snd_pcm_start(pcm);
				if (err < 0)
					goto _end;
			}
		}
	}
 _end:
	return xfer > 0 ? (snd_pcm_sframes_t) xfer : err;
}

snd_pcm_uframes_t _snd_pcm_mmap_hw_ptr(snd_pcm_t *pcm)
{
	return *pcm->hw_ptr;
}

snd_pcm_uframes_t _snd_pcm_boundary(snd_pcm_t *pcm)
{
	return pcm->boundary;
}

static const char *names[SND_PCM_HW_PARAM_LAST + 1] = {
	[SND_PCM_HW_PARAM_FORMAT] = "format",
	[SND_PCM_HW_PARAM_CHANNELS] = "channels",
	[SND_PCM_HW_PARAM_RATE] = "rate",
	[SND_PCM_HW_PARAM_PERIOD_TIME] = "period_time",
	[SND_PCM_HW_PARAM_BUFFER_TIME] = "buffer_time"
};

int snd_pcm_slave_conf(snd_config_t *root, snd_config_t *conf,
		       snd_config_t **_pcm_conf, unsigned int count, ...)
{
	snd_config_iterator_t i, next;
	const char *str;
	struct {
		unsigned int index;
		int flags;
		void *ptr;
		int present;
	} fields[count];
	unsigned int k;
	snd_config_t *pcm_conf = NULL;
	int err;
	int to_free = 0;
	va_list args;
	assert(root);
	assert(conf);
	assert(_pcm_conf);
	if (snd_config_get_string(conf, &str) >= 0) {
		err = snd_config_search_definition(root, "pcm_slave", str, &conf);
		if (err < 0) {
			SNDERR("Invalid slave definition");
			return -EINVAL;
		}
		to_free = 1;
	}
	if (snd_config_get_type(conf) != SND_CONFIG_TYPE_COMPOUND) {
		SNDERR("Invalid slave definition");
		err = -EINVAL;
		goto _err;
	}
	va_start(args, count);
	for (k = 0; k < count; ++k) {
		fields[k].index = va_arg(args, int);
		fields[k].flags = va_arg(args, int);
		fields[k].ptr = va_arg(args, void *);
		fields[k].present = 0;
	}
	va_end(args);
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id = snd_config_get_id(n);
		if (strcmp(id, "comment") == 0)
			continue;
		if (strcmp(id, "pcm") == 0) {
			if (pcm_conf != NULL)
				snd_config_delete(pcm_conf);
			if ((err = snd_config_copy(&pcm_conf, n)) < 0)
				goto _err;
			continue;
		}
		for (k = 0; k < count; ++k) {
			unsigned int idx = fields[k].index;
			long v;
			assert(idx < SND_PCM_HW_PARAM_LAST);
			assert(names[idx]);
			if (strcmp(id, names[idx]) != 0)
				continue;
			switch (idx) {
			case SND_PCM_HW_PARAM_FORMAT:
			{
				snd_pcm_format_t f;
				err = snd_config_get_string(n, &str);
				if (err < 0) {
				_invalid:
					SNDERR("invalid type for %s", id);
					goto _err;
				}
				if ((fields[k].flags & SCONF_UNCHANGED) &&
				    strcasecmp(str, "unchanged") == 0) {
					*(snd_pcm_format_t*)fields[k].ptr = (snd_pcm_format_t) -2;
					break;
				}
				f = snd_pcm_format_value(str);
				if (f == SND_PCM_FORMAT_UNKNOWN) {
					SNDERR("unknown format");
					err = -EINVAL;
					goto _err;
				}
				*(snd_pcm_format_t*)fields[k].ptr = f;
				break;
			}
			default:
				if ((fields[k].flags & SCONF_UNCHANGED)) {
					err = snd_config_get_string(n, &str);
					if (err >= 0 &&
					    strcasecmp(str, "unchanged") == 0) {
						*(int*)fields[k].ptr = -2;
						break;
					}
				}
				err = snd_config_get_integer(n, &v);
				if (err < 0)
					goto _invalid;
				*(int*)fields[k].ptr = v;
				break;
			}
			fields[k].present = 1;
			break;
		}
		if (k < count)
			continue;
		SNDERR("Unknown field %s", id);
		err = -EINVAL;
		goto _err;
	}
	if (!pcm_conf) {
		SNDERR("missing field pcm");
		err = -EINVAL;
		goto _err;
	}
	for (k = 0; k < count; ++k) {
		if ((fields[k].flags & SCONF_MANDATORY) && !fields[k].present) {
			SNDERR("missing field %s", names[fields[k].index]);
			err = -EINVAL;
			goto _err;
		}
	}
	*_pcm_conf = pcm_conf;
	pcm_conf = NULL;
	err = 0;
 _err:
 	if (pcm_conf)
 		snd_config_delete(pcm_conf);
	if (to_free)
		snd_config_delete(conf);
	return err;
}
		

int snd_pcm_conf_generic_id(const char *id)
{
	static const char *ids[] = { "comment", "type" };
	unsigned int k;
	for (k = 0; k < sizeof(ids) / sizeof(ids[0]); ++k) {
		if (strcmp(id, ids[k]) == 0)
			return 1;
	}
	return 0;
}

#endif
