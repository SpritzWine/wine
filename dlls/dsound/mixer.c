/*  			DirectSound
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998 Rob Riggs
 * Copyright 2000-2002 TransGaming Technologies, Inc.
 * Copyright 2007 Peter Dons Tychsen
 * Copyright 2007 Maarten Lankhorst
 * Copyright 2011 Owen Rudge for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <math.h>	/* Insomnia - pow() function */

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "wingdi.h"
#include "mmreg.h"
#include "wine/debug.h"
#include "dsound.h"
#include "ks.h"
#include "ksmedia.h"
#include "dsound_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

void DSOUND_RecalcVolPan(PDSVOLUMEPAN volpan)
{
	double temp;
	TRACE("(%p)\n",volpan);

	TRACE("Vol=%ld Pan=%ld\n", volpan->lVolume, volpan->lPan);
	/* the AmpFactors are expressed in 16.16 fixed point */

	if (volpan->lVolume == DSBVOLUME_MIN)
	{
		for (unsigned int i = 0; i < DS_MAX_CHANNELS; i++)
			volpan->dwTotalAmpFactor[i] = 0;
		TRACE("setting all channel volumes to 0\n");
		return;
	}
	/* FIXME: use calculated vol and pan ampfactors */
	temp = (double) (volpan->lVolume - (volpan->lPan > 0 ? volpan->lPan : 0));
	volpan->dwTotalAmpFactor[0] = (ULONG) (pow(2.0, temp / 600.0) * 0xffff);
	temp = (double) (volpan->lVolume + (volpan->lPan < 0 ? volpan->lPan : 0));
	volpan->dwTotalAmpFactor[1] = (ULONG) (pow(2.0, temp / 600.0) * 0xffff);

	TRACE("left = %lx, right = %lx\n", volpan->dwTotalAmpFactor[0], volpan->dwTotalAmpFactor[1]);
}

void DSOUND_AmpFactorToVolPan(PDSVOLUMEPAN volpan)
{
    double left,right;
    TRACE("(%p)\n",volpan);

    TRACE("left=%lx, right=%lx\n",volpan->dwTotalAmpFactor[0],volpan->dwTotalAmpFactor[1]);
    if (volpan->dwTotalAmpFactor[0]==0)
        left=-10000;
    else
        left=600 * log(((double)volpan->dwTotalAmpFactor[0]) / 0xffff) / log(2);
    if (volpan->dwTotalAmpFactor[1]==0)
        right=-10000;
    else
        right=600 * log(((double)volpan->dwTotalAmpFactor[1]) / 0xffff) / log(2);
    if (left<right)
        volpan->lVolume=right;
    else
        volpan->lVolume=left;
    if (volpan->lVolume < -10000)
        volpan->lVolume=-10000;
    volpan->lPan=right-left;
    if (volpan->lPan < -10000)
        volpan->lPan=-10000;

    TRACE("Vol=%ld Pan=%ld\n", volpan->lVolume, volpan->lPan);
}

/* How many bits to reserve for the fractional part of position cursors in fixed point. */
#define POS_FRAC_SHIFT 16
#define POS_FRAC_MASK ((1u << POS_FRAC_SHIFT) - 1)

/* Length of the resampler filter. */
#define CUBIC_LEN 4
/* How many samples to save from one run of the resampler to the next. */
#define CUBIC_STATE_LEN 1

/* The number of distinct phase intervals within the cubic filter tables. */
#define CUBIC_PHASE_SHIFT 5
#define CUBIC_PHASE_COUNT (1u << CUBIC_PHASE_SHIFT)
#define CUBIC_PHASE_MASK (CUBIC_PHASE_COUNT - 1)

#define CUBIC_PHASE_DIFF_SHIFT (POS_FRAC_SHIFT - CUBIC_PHASE_SHIFT)
#define CUBIC_PHASE_DIFF_ONE  (1u << CUBIC_PHASE_DIFF_SHIFT)
#define CUBIC_PHASE_DIFF_MASK ((1u << CUBIC_PHASE_DIFF_SHIFT) - 1)

struct cubic_coefficients
{
    float coeffs[4];
    float deltas[4];
};

static BOOL cubic_spline_table_init(struct cubic_coefficients **cubic_table)
{
    const double third = 1.0 / 3.0;
    const double sixth = 1.0 / 6.0;
    struct cubic_coefficients *table;
    unsigned int pi;

    /* This filter table is based on a Catmull-Rom spline. It retains more of
     * the original high-frequency content, at the cost of increased harmonics.
     */
    if (!*cubic_table)
        *cubic_table = malloc(sizeof(**cubic_table) * CUBIC_PHASE_COUNT);
    if (!*cubic_table)
        return FALSE;
    table = *cubic_table;
    for (pi = 0; pi < CUBIC_PHASE_COUNT; ++pi)
    {
        double mu = (double)pi / CUBIC_PHASE_COUNT;
        double mu2 = mu * mu;
        double mu3 = mu * mu2;

        table[pi].coeffs[0] =       -third*mu + 0.5*mu2  - sixth*mu3;
        table[pi].coeffs[1] = 1.0 -    0.5*mu -     mu2  +   0.5*mu3;
        table[pi].coeffs[2] =              mu + 0.5*mu2  -   0.5*mu3;
        table[pi].coeffs[3] =       -sixth*mu            + sixth*mu3;
    }

    for (pi = 0; pi < CUBIC_PHASE_COUNT - 1; ++pi)
    {
        table[pi].deltas[0] = table[pi + 1].coeffs[0] - table[pi].coeffs[0];
        table[pi].deltas[1] = table[pi + 1].coeffs[1] - table[pi].coeffs[1];
        table[pi].deltas[2] = table[pi + 1].coeffs[2] - table[pi].coeffs[2];
        table[pi].deltas[3] = table[pi + 1].coeffs[3] - table[pi].coeffs[3];
    }

    pi = CUBIC_PHASE_COUNT - 1;
    table[pi].deltas[0] =               0.0f - table[pi].coeffs[0];
    table[pi].deltas[1] = table[0].coeffs[0] - table[pi].coeffs[1];
    table[pi].deltas[2] = table[0].coeffs[1] - table[pi].coeffs[2];
    table[pi].deltas[3] = table[0].coeffs[2] - table[pi].coeffs[3];
    return TRUE;
}

/**
 * Recalculate the size for temporary buffer, and new writelead
 * Should be called when one of the following things occur:
 * - Primary buffer format is changed
 * - This buffer format (frequency) is changed
 */
void DSOUND_RecalcFormat(IDirectSoundBufferImpl *dsb)
{
	DWORD ichannels = dsb->pwfx->nChannels;
	DWORD ochannels = dsb->device->pwfx->nChannels;
	LONG64 oldFreqAdjustDen = dsb->freqAdjustDen;
	WAVEFORMATEXTENSIBLE *pwfxe;
	BOOL ieee = FALSE;

	TRACE("(%p)\n",dsb);

	pwfxe = (WAVEFORMATEXTENSIBLE *) dsb->pwfx;
	dsb->freqAdjustNum = dsb->freq;
	dsb->freqAdjustDen = dsb->device->pwfx->nSamplesPerSec;

	if ((pwfxe->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) || ((pwfxe->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	    && (IsEqualGUID(&pwfxe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))))
		ieee = TRUE;

        if (!dsb->device->cubic_table && !cubic_spline_table_init(&dsb->device->cubic_table))
            ERR("Cubic spline resampler initialization failed!\n");

	/* calculate the 10ms write lead */
	dsb->writelead = (dsb->freq / 100) * dsb->pwfx->nBlockAlign;
	dsb->maxwritelead = (DSBFREQUENCY_MAX / 100) * dsb->pwfx->nBlockAlign;

	if (oldFreqAdjustDen)
		dsb->freqAccNum = (dsb->freqAccNum * dsb->freqAdjustDen + oldFreqAdjustDen / 2) / oldFreqAdjustDen;

	dsb->get_aux = ieee ? getbpp[4] : getbpp[dsb->pwfx->wBitsPerSample/8 - 1];
	dsb->put_aux = putieee32;

	dsb->get = dsb->get_aux;
	dsb->put = dsb->put_aux;

	if (ichannels == ochannels)
	{
		dsb->mix_channels = ichannels;
		if (ichannels > 32) {
			FIXME("Copying %lu channels is unsupported, limiting to first 32\n", ichannels);
			dsb->mix_channels = 32;
		}
	}
	else if (ichannels == 1)
	{
		dsb->mix_channels = 1;

		if (ochannels == 2)
			dsb->put = put_mono2stereo;
		else if (ochannels == 4)
			dsb->put = put_mono2quad;
		else if (ochannels == 6)
			dsb->put = put_mono2surround51;
	}
	else if (ochannels == 1)
	{
		dsb->mix_channels = 1;
		dsb->get = get_mono;
	}
	else if (ichannels == 2 && ochannels == 4)
	{
		dsb->mix_channels = 2;
		dsb->put = put_stereo2quad;
	}
	else if (ichannels == 2 && ochannels == 6)
	{
		dsb->mix_channels = 2;
		dsb->put = put_stereo2surround51;
	}
	else if (ichannels == 6 && ochannels == 2)
	{
		dsb->mix_channels = 6;
		dsb->put = put_surround512stereo;
		dsb->put_aux = putieee32_sum;
	}
	else if (ichannels == 8 && ochannels == 2)
	{
		dsb->mix_channels = 8;
		dsb->put = put_surround712stereo;
		dsb->put_aux = putieee32_sum;
	}
	else if (ichannels == 4 && ochannels == 2)
	{
		dsb->mix_channels = 4;
		dsb->put = put_quad2stereo;
		dsb->put_aux = putieee32_sum;
	}
	else
	{
		if (ichannels > 2)
			FIXME("Conversion from %lu to %lu channels is not implemented, falling back to stereo\n", ichannels, ochannels);
		dsb->mix_channels = 2;
	}
}

/**
 * Check for application callback requests for when the play position
 * reaches certain points.
 *
 * The offsets that will be triggered will be those between the recorded
 * "last played" position for the buffer (i.e. dsb->playpos) and "len" bytes
 * beyond that position.
 */
void DSOUND_CheckEvent(const IDirectSoundBufferImpl *dsb, DWORD playpos, int len)
{
    int first, left, right, check;

    if(dsb->nrofnotifies == 0)
        return;

    if(dsb->state == STATE_STOPPED){
        TRACE("Stopped...\n");
        /* DSBPN_OFFSETSTOP notifies are always at the start of the sorted array */
        for(left = 0; left < dsb->nrofnotifies; ++left){
            if(dsb->notifies[left].dwOffset != DSBPN_OFFSETSTOP)
                break;

            TRACE("Signalling %p\n", dsb->notifies[left].hEventNotify);
            SetEvent(dsb->notifies[left].hEventNotify);
        }
    }

    for(first = 0; first < dsb->nrofnotifies && dsb->notifies[first].dwOffset == DSBPN_OFFSETSTOP; ++first)
        ;

    if(first == dsb->nrofnotifies)
        return;

    check = left = first;
    right = dsb->nrofnotifies - 1;

    /* find leftmost notify that is greater than playpos */
    while(left != right){
        check = left + (right - left) / 2;
        if(dsb->notifies[check].dwOffset < playpos)
            left = check + 1;
        else if(dsb->notifies[check].dwOffset > playpos)
            right = check;
        else{
            left = check;
            break;
        }
    }

    TRACE("Not stopped: first notify: %u (%lu), left notify: %u (%lu), range: [%lu,%lu)\n",
            first, dsb->notifies[first].dwOffset,
            left, dsb->notifies[left].dwOffset,
            playpos, (playpos + len) % dsb->buflen);

    /* send notifications in range */
    if(dsb->notifies[left].dwOffset >= playpos){
        for(check = left; check < dsb->nrofnotifies; ++check){
            if(dsb->notifies[check].dwOffset >= playpos + len)
                break;

            TRACE("Signalling %p (%lu)\n", dsb->notifies[check].hEventNotify, dsb->notifies[check].dwOffset);
            SetEvent(dsb->notifies[check].hEventNotify);
        }
    }

    if(playpos + len > dsb->buflen){
        for(check = first; check < left; ++check){
            if(dsb->notifies[check].dwOffset >= (playpos + len) % dsb->buflen)
                break;

            TRACE("Signalling %p (%lu)\n", dsb->notifies[check].hEventNotify, dsb->notifies[check].dwOffset);
            SetEvent(dsb->notifies[check].hEventNotify);
        }
    }
}

static inline float get_current_sample(const IDirectSoundBufferImpl *dsb,
        BYTE *buffer, DWORD buflen, DWORD mixpos, DWORD channel)
{
    if (mixpos >= buflen && !(dsb->playflags & DSBPLAY_LOOPING))
        return 0.0f;
    return dsb->get(dsb, buffer + (mixpos % buflen), channel);
}

static UINT cp_fields_noresample(IDirectSoundBufferImpl *dsb, UINT count)
{
    UINT istride = dsb->pwfx->nBlockAlign;
    UINT ostride = dsb->device->pwfx->nChannels * sizeof(float);
    UINT committed_samples = 0;
    DWORD channel, i;

    if (!secondarybuffer_is_audible(dsb))
        return count;

    if(dsb->use_committed) {
        committed_samples = (dsb->writelead - dsb->committed_mixpos) / istride;
        committed_samples = committed_samples <= count ? committed_samples : count;
    }

    for (i = 0; i < committed_samples; i++)
        for (channel = 0; channel < dsb->mix_channels; channel++)
            dsb->put(dsb, i * ostride, channel, get_current_sample(dsb, dsb->committedbuff,
                dsb->writelead, dsb->committed_mixpos + i * istride, channel));

    for (; i < count; i++)
        for (channel = 0; channel < dsb->mix_channels; channel++)
            dsb->put(dsb, i * ostride, channel, get_current_sample(dsb, dsb->buffer->memory,
                dsb->buflen, dsb->sec_mixpos + i * istride, channel));

    return count;
}

/**
 * OpenAL cross platform audio library
 * Copyright (C) 2014 by Timothy Arceri <t_arceri@yahoo.com.au>.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

static void cubic_resample(IDirectSoundBufferImpl *dsb, UINT count, LONG64 freqAcc_start, float *intermediate, float *out_buffer)
{
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        unsigned int phase = ((freqAcc_start + i * dsb->freqAdjustNum) << POS_FRAC_SHIFT) / dsb->freqAdjustDen;
        unsigned int ipos = phase >> POS_FRAC_SHIFT;
        float *vals = &intermediate[ipos];
        float *fil, *phd;
        unsigned int pi;
        float pf, o;

        pi = (phase - (ipos << POS_FRAC_SHIFT)) >> CUBIC_PHASE_DIFF_SHIFT;
        pf = (phase & CUBIC_PHASE_DIFF_MASK) * (1.0f / CUBIC_PHASE_DIFF_ONE);

        fil = dsb->device->cubic_table[pi].coeffs;
        phd = dsb->device->cubic_table[pi].deltas;

        o = (fil[0] + pf * phd[0]) * vals[0] +
            (fil[1] + pf * phd[1]) * vals[1] +
            (fil[2] + pf * phd[2]) * vals[2] +
            (fil[3] + pf * phd[3]) * vals[3];
        out_buffer[i] = o;
    }
}

static UINT cp_fields_resample(IDirectSoundBufferImpl *dsb, UINT count, LONG64 *freqAccNum)
{
    UINT i, channel;
    UINT istride = dsb->pwfx->nBlockAlign;
    UINT ostride = dsb->device->pwfx->nChannels * sizeof(float);
    UINT committed_samples = 0;

    LONG64 freqAcc_start = *freqAccNum;
    LONG64 freqAcc_end = freqAcc_start + count * dsb->freqAdjustNum;
    UINT channels = dsb->mix_channels;
    UINT max_ipos = (freqAcc_start + count * dsb->freqAdjustNum) / dsb->freqAdjustDen;
    unsigned int required_input = max_ipos + CUBIC_LEN - 1;
    DWORD in_size = required_input * channels * sizeof(float);
    DWORD out_size = count * channels * sizeof(float);
    float *intermediate, *out_buffer, *itmp;
    unsigned int state_count;

    *freqAccNum = freqAcc_end % dsb->freqAdjustDen;

    if (!secondarybuffer_is_audible(dsb))
        return max_ipos;

    if (!dsb->device->cp_buffer)
    {
        dsb->device->cp_buffer = malloc(in_size);
        dsb->device->cp_buffer_len = in_size;
    }
    else if (in_size > dsb->device->cp_buffer_len)
    {
        dsb->device->cp_buffer = realloc(dsb->device->cp_buffer, in_size);
        dsb->device->cp_buffer_len = in_size;
    }
    intermediate = dsb->device->cp_buffer;

    if (!dsb->device->out_buffer)
    {
        dsb->device->out_buffer = malloc(out_size);
        dsb->device->out_buffer_len = out_size;
    }
    else if (out_size > dsb->device->out_buffer_len)
    {
        dsb->device->out_buffer = realloc(dsb->device->out_buffer, out_size);
        dsb->device->out_buffer_len = out_size;
    }
    out_buffer = dsb->device->out_buffer;

    state_count = CUBIC_STATE_LEN;

    if (dsb->use_committed)
    {
        committed_samples = (dsb->writelead - dsb->committed_mixpos) / istride;
        committed_samples = committed_samples <= required_input - state_count ? committed_samples : required_input - state_count;
    }

    itmp = intermediate;
    for (channel = 0; channel < channels; channel++)
    {
        if (dsb->first_resample)
        {
            for (i = 0; i < state_count; i++)
                *(itmp++) = 0.0f;
        }
        else
        {
            for (i = 0; i < state_count; i++)
                *(itmp++) = dsb->resampler_state[channel * RESAMPLER_MAX_STATE + i];
        }
        for (i = 0; i < committed_samples; i++)
            *(itmp++) = get_current_sample(dsb, dsb->committedbuff,
                dsb->writelead, dsb->committed_mixpos + i * istride, channel);
        for (; i < required_input - state_count; i++)
            *(itmp++) = get_current_sample(dsb, dsb->buffer->memory,
                    dsb->buflen, dsb->sec_mixpos + i * istride, channel);

        memcpy(dsb->resampler_state + channel * RESAMPLER_MAX_STATE,
               &intermediate[channel * required_input + max_ipos + 1 - CUBIC_STATE_LEN],
               CUBIC_STATE_LEN * sizeof(float));
    }

    for (channel = 0; channel < channels; channel++)
    {
        cubic_resample(dsb, count, freqAcc_start, &intermediate[channel * required_input], &out_buffer[channel * count]);
    }

    for (i = 0; i < count; ++i)
        for (channel = 0; channel < channels; channel++)
            dsb->put(dsb, i * ostride, channel, out_buffer[channel * count + i]);

    return max_ipos;
}

static void cp_fields(IDirectSoundBufferImpl *dsb, UINT count, LONG64 *freqAccNum)
{
    DWORD ipos, adv;

    if (dsb->freqAdjustNum == dsb->freqAdjustDen)
        adv = cp_fields_noresample(dsb, count); /* *freqAccNum is unmodified */
    else
        adv = cp_fields_resample(dsb, count, freqAccNum);

    ipos = dsb->sec_mixpos + adv * dsb->pwfx->nBlockAlign;
    if (ipos >= dsb->buflen) {
        if (dsb->playflags & DSBPLAY_LOOPING)
            ipos %= dsb->buflen;
        else {
            ipos = 0;
            dsb->state = STATE_STOPPED;
        }
    }

    dsb->sec_mixpos = ipos;

    if(dsb->use_committed) {
        dsb->committed_mixpos += adv * dsb->pwfx->nBlockAlign;
        if(dsb->committed_mixpos >= dsb->writelead)
            dsb->use_committed = FALSE;
    }
}

/**
 * Calculate the distance between two buffer offsets, taking wraparound
 * into account.
 */
static inline DWORD DSOUND_BufPtrDiff(DWORD buflen, DWORD ptr1, DWORD ptr2)
{
/* If these asserts fail, the problem is not here, but in the underlying code */
	assert(ptr1 < buflen);
	assert(ptr2 < buflen);
	if (ptr1 >= ptr2) {
		return ptr1 - ptr2;
	} else {
		return buflen + ptr1 - ptr2;
	}
}
/**
 * Mix at most the given amount of data into the allocated temporary buffer
 * of the given secondary buffer, starting from the dsb's first currently
 * unsampled frame (writepos), translating frequency (pitch), stereo/mono
 * and bits-per-sample so that it is ideal for the primary buffer.
 * Doesn't perform any mixing - this is a straight copy/convert operation.
 *
 * dsb = the secondary buffer
 * writepos = Starting position of changed buffer
 * len = number of bytes to resample from writepos
 *
 * NOTE: writepos + len <= buflen. When called by mixer, MixOne makes sure of this.
 */
static void DSOUND_MixToTemporary(IDirectSoundBufferImpl *dsb, DWORD frames)
{
	UINT size_bytes = frames * sizeof(float) * dsb->device->pwfx->nChannels;
	HRESULT hr;
	int i;

	if (dsb->device->tmp_buffer_len < size_bytes || !dsb->device->tmp_buffer)
	{
		dsb->device->tmp_buffer_len = size_bytes;
		dsb->device->tmp_buffer = realloc(dsb->device->tmp_buffer, size_bytes);
	}
	if(dsb->put_aux == putieee32_sum)
		memset(dsb->device->tmp_buffer, 0, dsb->device->tmp_buffer_len);

	cp_fields(dsb, frames, &dsb->freqAccNum);

	if (size_bytes > 0) {
		for (i = 0; i < dsb->num_filters; i++) {
			if (dsb->filters[i].inplace) {
				hr = IMediaObjectInPlace_Process(dsb->filters[i].inplace, size_bytes, (BYTE*)dsb->device->tmp_buffer, 0, DMO_INPLACE_NORMAL);

				if (FAILED(hr))
					WARN("IMediaObjectInPlace_Process failed for filter %u\n", i);
			} else
				WARN("filter %u has no inplace object - unsupported\n", i);
		}
	}
}

static void DSOUND_MixerVol(const IDirectSoundBufferImpl *dsb, INT frames)
{
	INT	i;
	float vols[DS_MAX_CHANNELS];
	UINT channels = dsb->device->pwfx->nChannels, chan;

	TRACE("(%p,%d)\n",dsb,frames);
	TRACE("left = %lx, right = %lx\n", dsb->volpan.dwTotalAmpFactor[0],
		dsb->volpan.dwTotalAmpFactor[1]);

	if ((!(dsb->dsbd.dwFlags & DSBCAPS_CTRLPAN) || (dsb->volpan.lPan == 0)) &&
	    (!(dsb->dsbd.dwFlags & DSBCAPS_CTRLVOLUME) || (dsb->volpan.lVolume == 0)) &&
	     !(dsb->dsbd.dwFlags & DSBCAPS_CTRL3D))
		return; /* Nothing to do */

	if (channels > DS_MAX_CHANNELS)
	{
		FIXME("There is no support for %u channels\n", channels);
		return;
	}

	for (i = 0; i < channels; ++i)
		vols[i] = dsb->volpan.dwTotalAmpFactor[i] / ((float)0xFFFF);

	for(i = 0; i < frames; ++i){
		for(chan = 0; chan < channels; ++chan){
			dsb->device->tmp_buffer[i * channels + chan] *= vols[chan];
		}
	}
}

/**
 * Mix (at most) the given number of bytes into the given position of the
 * device buffer, from the secondary buffer "dsb" (starting at the current
 * mix position for that buffer).
 *
 * Returns the number of bytes actually mixed into the device buffer. This
 * will match fraglen unless the end of the secondary buffer is reached
 * (and it is not looping).
 *
 * dsb  = the secondary buffer to mix from
 * fraglen = number of bytes to mix
 */
static DWORD DSOUND_MixInBuffer(IDirectSoundBufferImpl *dsb, float *mix_buffer, DWORD frames)
{
	float *ibuf;
	DWORD oldpos;

	TRACE("sec_mixpos=%ld/%ld\n", dsb->sec_mixpos, dsb->buflen);
	TRACE("(%p, frames=%ld)\n",dsb,frames);

	/* Resample buffer to temporary buffer specifically allocated for this purpose, if needed */
	oldpos = dsb->sec_mixpos;
	DSOUND_MixToTemporary(dsb, frames);
	ibuf = dsb->device->tmp_buffer;

	if (secondarybuffer_is_audible(dsb)) {
		/* Apply volume if needed */
		DSOUND_MixerVol(dsb, frames);

		mixieee32(ibuf, mix_buffer, frames * dsb->device->pwfx->nChannels);
	}

	/* check for notification positions */
	if (dsb->dsbd.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY) {
		INT ilen = DSOUND_BufPtrDiff(dsb->buflen, dsb->sec_mixpos, oldpos);
		DSOUND_CheckEvent(dsb, oldpos, ilen);
	}

	return frames;
}

/**
 * Mix some frames from the given secondary buffer "dsb" into the device
 * primary buffer.
 *
 * dsb = the secondary buffer
 * playpos = the current play position in the device buffer (primary buffer)
 * frames = the maximum number of frames in the primary buffer to mix, from the
 *          current writepos.
 *
 * Returns: the number of frames beyond the writepos that were mixed.
 */
static DWORD DSOUND_MixOne(IDirectSoundBufferImpl *dsb, float *mix_buffer, DWORD frames)
{
	DWORD primary_done = 0;

	TRACE("(%p, frames=%ld)\n",dsb,frames);
	TRACE("looping=%ld\n", dsb->playflags);

	/* First try to mix to the end of the buffer if possible
	 * Theoretically it would allow for better optimization
	*/
	primary_done += DSOUND_MixInBuffer(dsb, mix_buffer, frames);

	TRACE("total mixed data=%ld\n", primary_done);

	/* Report back the total prebuffered amount for this buffer */
	return primary_done;
}

/**
 * For a DirectSoundDevice, go through all the currently playing buffers and
 * mix them in to the device buffer.
 *
 * frames = the maximum amount to mix into the primary buffer
 * all_stopped = reports back if all buffers have stopped
 *
 * Returns:  the length beyond the writepos that was mixed to.
 */

static void DSOUND_MixToPrimary(const DirectSoundDevice *device, float *mix_buffer, DWORD frames, BOOL *all_stopped)
{
	INT i;
	IDirectSoundBufferImpl	*dsb;

	/* unless we find a running buffer, all have stopped */
	*all_stopped = TRUE;

	TRACE("(frames %ld)\n", frames);
	for (i = 0; i < device->nrofbuffers; i++) {
		dsb = device->buffers[i];

		TRACE("MixToPrimary for %p, state=%ld\n", dsb, dsb->state);

		if (dsb->buflen && dsb->state) {
			TRACE("Checking %p, frames=%ld\n", dsb, frames);
			AcquireSRWLockShared(&dsb->lock);
			if (dsb->state != STATE_STOPPED) {

				/* if the buffer was starting, it must be playing now */
				if (dsb->state == STATE_STARTING)
                                {
					dsb->state = STATE_PLAYING;
                                        dsb->first_resample = TRUE;
                                }
                                else
                                    dsb->first_resample = FALSE;

				/* mix next buffer into the main buffer */
				DSOUND_MixOne(dsb, mix_buffer, frames);

				*all_stopped = FALSE;
			}
			ReleaseSRWLockShared(&dsb->lock);
		}
	}
}

/**
 * Add buffers to the emulated wave device system.
 *
 * device = The current dsound playback device
 * force = If TRUE, the function will buffer up as many frags as possible,
 *         even though and will ignore the actual state of the primary buffer.
 *
 * Returns:  None
 */

static void DSOUND_WaveQueue(DirectSoundDevice *device, LPBYTE pos, DWORD bytes)
{
	BYTE *buffer;
	HRESULT hr;

	TRACE("(%p)\n", device);

	hr = IAudioRenderClient_GetBuffer(device->render, bytes / device->pwfx->nBlockAlign, &buffer);
	if(FAILED(hr)){
		WARN("GetBuffer failed: %08lx\n", hr);
		return;
	}

	memcpy(buffer, pos, bytes);

	hr = IAudioRenderClient_ReleaseBuffer(device->render, bytes / device->pwfx->nBlockAlign, 0);
	if(FAILED(hr)) {
		ERR("ReleaseBuffer failed: %08lx\n", hr);
		IAudioRenderClient_ReleaseBuffer(device->render, 0, 0);
		return;
	}

	device->pad += bytes;
}

/**
 * Perform mixing for a Direct Sound device. That is, go through all the
 * secondary buffers (the sound bites currently playing) and mix them in
 * to the primary buffer (the device buffer).
 *
 * The mixing procedure goes:
 *
 * secondary->buffer (secondary format)
 *   =[Resample]=> device->tmp_buffer (float format)
 *   =[Volume]=> device->tmp_buffer (float format)
 *   =[Reformat]=> device->buffer (device format, skipped on float)
 */
static void DSOUND_PerformMix(DirectSoundDevice *device)
{
	DWORD block, pad_bytes, frames;
	UINT32 pad_frames;
	HRESULT hr;

	TRACE("(%p)\n", device);

	/* **** */
	EnterCriticalSection(&device->mixlock);

	hr = IAudioClient_GetCurrentPadding(device->client, &pad_frames);
	if(FAILED(hr)){
		WARN("GetCurrentPadding failed: %08lx\n", hr);
		LeaveCriticalSection(&device->mixlock);
		return;
	}
	block = device->pwfx->nBlockAlign;
	pad_bytes = pad_frames * block;
	device->playpos += device->pad - pad_bytes;
	device->playpos %= device->buflen;
	device->pad = pad_bytes;

	frames = device->ac_frames - pad_frames;
	if(!frames){
		/* nothing to do! */
		LeaveCriticalSection(&device->mixlock);
		return;
	}
	if (frames > device->frag_frames * 3)
		frames = device->frag_frames * 3;

	if (device->priolevel != DSSCL_WRITEPRIMARY) {
		BOOL all_stopped = FALSE;
		int nfiller;
		void *buffer = NULL;

		/* the sound of silence */
		nfiller = device->pwfx->wBitsPerSample == 8 ? 128 : 0;

		/* check for underrun. underrun occurs when the write position passes the mix position
		 * also wipe out just-played sound data */
		if (!pad_frames)
			WARN("Probable buffer underrun\n");

		hr = IAudioRenderClient_GetBuffer(device->render, frames, (BYTE **)&buffer);
		if(FAILED(hr)){
			WARN("GetBuffer failed: %08lx\n", hr);
			LeaveCriticalSection(&device->mixlock);
			return;
		}

		memset(buffer, nfiller, frames * block);

		if (!device->normfunction)
			DSOUND_MixToPrimary(device, buffer, frames, &all_stopped);
		else {
			memset(device->buffer, nfiller, device->buflen);

			/* do the mixing */
			DSOUND_MixToPrimary(device, (float*)device->buffer, frames, &all_stopped);

			device->normfunction(device->buffer, buffer, frames * device->pwfx->nChannels);
		}

		hr = IAudioRenderClient_ReleaseBuffer(device->render, frames, 0);
		if(FAILED(hr))
			ERR("ReleaseBuffer failed: %08lx\n", hr);

		device->pad += frames * block;
	} else if (!device->stopped) {
                DWORD writepos = (device->playpos + pad_bytes) % device->buflen;
                DWORD bytes = frames * block;

		if (bytes > device->buflen)
			bytes = device->buflen;
		if (writepos + bytes > device->buflen) {
			DSOUND_WaveQueue(device, device->buffer + writepos, device->buflen - writepos);
			DSOUND_WaveQueue(device, device->buffer, writepos + bytes - device->buflen);
		} else
			DSOUND_WaveQueue(device, device->buffer + writepos, bytes);
	}

	LeaveCriticalSection(&(device->mixlock));
	/* **** */
}

DWORD CALLBACK DSOUND_mixthread(void *p)
{
	DirectSoundDevice *dev = p;

	TRACE("(%p)\n", dev);
	SetThreadDescription(GetCurrentThread(), L"wine_dsound_mixer");
        _controlfp_s(NULL, _DN_FLUSH, _MCW_DN);

	while (dev->ref) {
		DWORD ret;

		/*
		 * Some audio drivers are retarded and won't fire after being
		 * stopped, add a timeout to handle this.
		 */
		ret = WaitForSingleObject(dev->sleepev, dev->sleeptime);
		if (ret == WAIT_FAILED)
			WARN("wait returned error %lu %08lx!\n", GetLastError(), GetLastError());
		else if (ret != WAIT_OBJECT_0)
			WARN("wait returned %08lx!\n", ret);
		if (!dev->ref)
			break;

		AcquireSRWLockShared(&dev->buffer_list_lock);
		DSOUND_PerformMix(dev);
		ReleaseSRWLockShared(&dev->buffer_list_lock);
	}
	return 0;
}
