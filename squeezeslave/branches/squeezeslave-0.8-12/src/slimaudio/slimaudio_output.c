/*
 *   SlimProtoLib Copyright (c) 2004,2006 Richard Titmuss
 *
 *   This file is part of SlimProtoLib.
 *
 *   SlimProtoLib is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   SlimProtoLib is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with SlimProtoLib; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#include <portaudio.h>
#include <portmixer.h>

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"


#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_output_debug) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
#endif


bool slimaudio_output_debug;

static void *output_thread(void *ptr);

static int pa_callback(  void *inputBuffer, void *outputBuffer,
                     unsigned long framesPerBuffer,
                     PaTimestamp outTime, void *userData );


static int audg_callback(slimproto_t *p, const unsigned char *buf, int buf_len, void *user_data);


int slimaudio_output_init(slimaudio_t *audio) {
	int i, err;
	
	err = Pa_Initialize();
	if (err != paNoError) {
		printf("PortAudio error: %s\n", Pa_GetErrorText(err) );	
		exit(-1);
	}	
	DEBUGF("slimaudio_output_init: PortAudio initialized\n");
	
	audio->num_device_names = Pa_CountDevices();
	audio->device_names = (char **)malloc(sizeof(char *) * audio->num_device_names);
	
	const PaDeviceInfo *pdi;
	for(i=0; i<audio->num_device_names; i++ ) {
		pdi = Pa_GetDeviceInfo( i );
		audio->device_names[i] = strdup(pdi->name);
	}

	audio->output_device_id = Pa_GetDefaultOutputDeviceID();
	audio->px_mixer = NULL;
	audio->volume_control = VOLUME_DRIVER;
	audio->volume = 1.F;
	audio->prev_volume = -1.F; // Signals prev = volume.
	audio->output_predelay_msec = 0;
	audio->output_predelay_frames = 0;
	audio->output_predelay_amplitude = 0;
	audio->keepalive_interval = -1;
	
	slimproto_add_command_callback(audio->proto, "audg", &audg_callback, audio);
	
	pthread_mutex_init(&(audio->output_mutex), NULL);
	pthread_cond_init(&(audio->output_cond), NULL);

	return 0;
}

void slimaudio_output_destroy(slimaudio_t *audio) {

	pthread_mutex_destroy(&(audio->output_mutex));
	pthread_cond_destroy(&(audio->output_cond));

	int i;
	
	// FIXME remove slimproto callback
	
	for (i=0; i<audio->num_device_names; i++) {
		free(audio->device_names[i]);	
	}
	free(audio->device_names);
	audio->num_device_names = 0;
	audio->device_names = NULL;
	
	Pa_Terminate();
}

void slimaudio_get_output_devices(slimaudio_t *audio, char ***device_names, int *num_device_names) {
	*device_names = audio->device_names;
	*num_device_names = audio->num_device_names;
}

void slimaudio_set_output_device(slimaudio_t *audio, int device_id) {
	audio->output_device_id = device_id;
}

int slimaudio_output_open(slimaudio_t *audio) {
	/* 
	 * We take the mutex here and release it in the output thread to prevent
	 * other threads from using the audio output information while it is
	 * being initialized.
	 */
	pthread_mutex_lock(&audio->output_mutex);

	audio->output_state = STOPPED;	

	if (pthread_create( &audio->output_thread, NULL, output_thread, (void*) audio) != 0) {
		fprintf(stderr, "Error creating output thread\n");
		return -1;	
	}	
	return 0;
}

int slimaudio_output_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	audio->output_state = QUIT;	
	pthread_cond_broadcast(&audio->output_cond);
	
	pthread_mutex_unlock(&audio->output_mutex);
	
	pthread_join(audio->output_thread, NULL);

	return 0;
}

// Wrapper to call slimaudio_stat from output_thread.  Because the
// output thread keeps the output mutex locked, this wrapper unlocks
// the mutex for the duration of the slimaudio_stat call.  The reason
// is that slimaudio_stat may find that the socket has been closed and
// will then try to stop the audio output by calling
// slimaudio_output_disconnect and this requires the mutex to be unlocked.
static void output_thread_stat(slimaudio_t* audio, char* code) {
	pthread_mutex_unlock(&audio->output_mutex);
	slimaudio_stat(audio, code);
	pthread_mutex_lock(&audio->output_mutex);
}

static void *output_thread(void *ptr) {
	int err;
	struct timeval  now;
	struct timespec timeout;
	
	slimaudio_t *audio = (slimaudio_t *) ptr;
	audio->output_STMu = false;
	audio->output_STMs = false;

	err = Pa_OpenStream(	&audio->pa_stream,	// stream
				paNoDevice,		// input device
				0,			// input channels
				0,			// input sample format
				NULL,			// input driver info
				audio->output_device_id,// output device				
				2,			// output channels
				paInt16,		// output sample format
				NULL,			// output driver info
				44100,			// sample rate
				256,			// frames per buffer
				0,			// number of buffers
				0,			// stream flags
				pa_callback,		// callback
				audio);			// user data
	if (err != paNoError) {
		printf("output_thread: PortAudio error1: %s\n", Pa_GetErrorText(err) );	
		exit(-1);
	}

	int num_mixers = Px_GetNumMixers(audio->pa_stream);
	while (--num_mixers >= 0) {
		DEBUGF("Mixer: %s\n", Px_GetMixerName(audio->pa_stream, num_mixers));
	}
	
	if (audio->volume_control == VOLUME_DRIVER) {
		DEBUGF("Opening mixer.\n" );
		audio->px_mixer = Px_OpenMixer(audio->pa_stream, 0);
	}
	else {
		DEBUGF("Mixer explicitly disabled.\n" );
	}
	DEBUGF("Px_mixer = %p\n", audio->px_mixer);

	if (audio->px_mixer != NULL) {
#if !defined(__SUNPRO_C)
		DEBUGF("PCM volume supported: %d.\n", 
		       Px_SupportsPCMOutputVolume(audio->px_mixer));
#endif
		const int nbVolumes = Px_GetNumOutputVolumes(audio->px_mixer);
		DEBUGF("Nb volumes supported: %d.\n", nbVolumes);
		int volumeIdx;
		for (volumeIdx=0; volumeIdx<nbVolumes; ++volumeIdx) {
			DEBUGF("Volume %d: %s\n", volumeIdx, 
				Px_GetOutputVolumeName(audio->px_mixer, 
						       volumeIdx));
		}
	}

	while (audio->output_state != QUIT) {				
		DEBUGF("output_thread: state %i\n", audio->output_state);

		switch (audio->output_state) {
			case STOPPED:
			case PAUSED:
				// We report ourselves to the server every few seconds
				// as a keep-alive.  This is required for SqueezeCenter version 
				// 6.5.x although technically, "stat" is not a valid event 
				// code for the STAT Client->Server message.  This was 
				// lifted by observing how a Squeezebox3 reports itself to 
				// the server using SqueezeCenter's d_slimproto and 
				// d_slimproto_v tracing services.  Note that Squeezebox3
			  	// seems to report every 1 second or so, but the server only
			  	// drops the connection after 15-20 seconds of inactivity.

			  	if (audio->keepalive_interval <= 0) {
					pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
				}
				else {
					gettimeofday(&now, NULL);
					timeout.tv_sec = now.tv_sec + audio->keepalive_interval;
					timeout.tv_nsec = now.tv_usec * 1000;				
					err = pthread_cond_timedwait(&audio->output_cond,
								     &audio->output_mutex, &timeout);
					if (err == ETIMEDOUT) {
						DEBUGF("Sending keepalive.  Interval=%d s.\n",
						       audio->keepalive_interval);
						output_thread_stat(audio, "stat");
					}
				}
				break;

			case PLAY:
/*
 * TODO I am not sure if it's best to start the stream in the play or buffering
 * state ... which has less latency for sync? If the stream is started in 
 * BUFFERING then pause/unpause breaks.
 */
		   
				audio->output_predelay_frames = 
					audio->output_predelay_msec * 44.1;
				err = Pa_StartStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error2: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}

				slimaudio_buffer_set_readopt(audio->output_buffer, BUFFER_NONBLOCKING);
				audio->pa_streamtime_offset = Pa_StreamTime(audio->pa_stream);

				audio->output_state = PLAYING;
				pthread_cond_broadcast(&audio->output_cond);
				break;

			case BUFFERING:
			case PLAYING:			
				gettimeofday(&now, NULL);
				timeout.tv_sec = now.tv_sec + 1;
				timeout.tv_nsec = now.tv_usec * 1000;
				err = pthread_cond_timedwait(&audio->output_cond, &audio->output_mutex, &timeout);

				if (err == ETIMEDOUT) {
					output_thread_stat(audio, "STMt");
				}
					
				if (audio->output_STMu) {
					audio->output_STMu = false;
					output_thread_stat(audio, "STMu");
				}
					
				if (audio->output_STMs) {
					audio->output_STMs = false;
					audio->pa_streamtime_offset = Pa_StreamTime(audio->pa_stream);
					output_thread_stat(audio, "STMs");
				}

				break;
		
			case STOP:
				err = Pa_AbortStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error3: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}
					
				audio->output_state = STOPPED;
				pthread_cond_broadcast(&audio->output_cond);
				break;
				
			case PAUSE:
				err = Pa_StopStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error3: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}					
					
				audio->output_state = PAUSED;	
				pthread_cond_broadcast(&audio->output_cond);
				break;

			case QUIT:
				break;
		}		
	}
	pthread_mutex_unlock(&audio->output_mutex);
	
	if (audio->px_mixer != NULL) {
		Px_CloseMixer(audio->px_mixer);
		audio->px_mixer = NULL;
	}
	
	err = Pa_CloseStream(audio->pa_stream);
	if (err != paNoError) {
		printf("output_thread: PortAudio error3: %s\n", Pa_GetErrorText(err) );	
		exit(-1);
	}
	audio->pa_stream = NULL;

	return 0;
}


/*
 * Callback for the 'audg' command
 */
static int audg_callback(slimproto_t *proto, const unsigned char *buf, int buf_len, void *user_data) {
	slimproto_msg_t msg;
	
	slimaudio_t* const audio = (slimaudio_t *) user_data;
	slimproto_parse_command(buf, buf_len, &msg);

	const float vol = (float)msg.audg.left_gain / (float)0x10000;
	audio->volume = vol > 1.F ? 1.F : vol;
	DEBUGF("audg cmd %04x %f\n", msg.audg.left_gain, audio->volume);
	DEBUGF("old_left_gain:  %u\n", msg.audg.old_left_gain);
	DEBUGF("old_right_gain: %u\n", msg.audg.old_right_gain);
	DEBUGF("left_gain:      %u\n", msg.audg.left_gain);
	DEBUGF("right_gain:     %u\n", msg.audg.right_gain);
	DEBUGF("digital_volume_control: %hhu\n", msg.audg.digital_volume_control);
	DEBUGF("preamp:                 %hhu\n", msg.audg.preamp);

	if (audio->px_mixer != NULL) {
		Px_SetPCMOutputVolume(audio->px_mixer, (PxVolume)audio->volume);
		DEBUGF("pcm volume %f\n", Px_GetPCMOutputVolume(audio->px_mixer));	
	}
		
	return 0;
}


void slimaudio_output_connect(slimaudio_t *audio, slimproto_msg_t *msg) {
	pthread_mutex_lock(&audio->output_mutex);

	if (audio->output_state == PLAYING) {
		pthread_mutex_unlock(&audio->output_mutex);
		return;
	}
	
	DEBUGF("slimaudio_output_connect: state=%i\n", audio->output_state);
	audio->output_state = BUFFERING;
	pthread_cond_broadcast(&audio->output_cond);

	pthread_mutex_unlock(&audio->output_mutex);	
}


int slimaudio_output_disconnect(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	DEBUGF("slimaudio_output_disconnect: state=%i\n", audio->output_state);

	if (audio->output_state == STOPPED || audio->output_state == QUIT) {
		pthread_mutex_unlock(&audio->output_mutex);	
		return -1;	
	}	

	audio->output_state = STOP;	
	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == STOP) {				
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}

	pthread_mutex_unlock(&audio->output_mutex);	
	return 0;
}


void slimaudio_output_pause(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);
	
	DEBUGF("slimaudio_output_pause: state=%i\n", audio->output_state);

	audio->output_state = PAUSE;	
	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == PAUSE) {				
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}

	pthread_mutex_unlock(&audio->output_mutex);	
}


void slimaudio_output_unpause(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	if (audio->output_state == PLAYING) {
		pthread_mutex_unlock(&audio->output_mutex);	
		return;	
	}

	DEBUGF("slimaudio_output_unpause: state=%i\n", audio->output_state);

	audio->output_state = PLAY;
	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == PLAY) {
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}


	pthread_mutex_unlock(&audio->output_mutex);	
}


int slimaudio_output_streamtime(slimaudio_t *audio) {
	if (audio->output_state != PLAYING)
		return 0;
		
	PaTimestamp numSamples = Pa_StreamTime(audio->pa_stream);
	const int msec =
		(int)((numSamples - audio->pa_streamtime_offset) / 44.100) +
		audio->output_predelay_msec;

	return msec < 0 ? 0 : msec;
}


// Applies software volume to buffers being sent to the output device.  It is
// important we apply volume changes here rather than, for example, in the 
// decoder, to avoid latency between volume modification and audible change.
//
// FIXME:
// There are a couple of shortcuts taken in the name of simplicity here.
// 1. We assume 16-bits stereo.
// 2. We perform this in a separate loop, which will consume more 
//    frontside bus bandwidth than if we were performing this task as
//    part of, for example, the buffer copy.  But this would imply
//    changing audio buffers so they are format-aware.
static void apply_software_volume(slimaudio_t* const audio, void* outputBuffer,
				  int nbFrames) {
	if (audio->prev_volume == -1.F) {
		// A value of -1 indicates it's the first time we pass here.
		// Copy volume into prev_volume to start immediatly at the right
		// volume.
		audio->prev_volume = audio->volume;
	}

	if (audio->volume == 1.F && audio->prev_volume == 1.F) {
		return;
	}

	// Deglitch volume changes by going from the old to the new value over
	// the course of the whole buffer being sent to the output device.  We
	// read 'audio->volume' exactly once (assuming this operation is atomic)
	// to make sure that volume changes performed while we are here will not
	// cause any glitches.
	const float newVolume = audio->volume;
	float curVolume = audio->prev_volume;
	audio->prev_volume = newVolume;
	const float volumeIncr = (newVolume - curVolume) / (float)nbFrames;
	short* const samples = (short*)outputBuffer;
	const int nbSamples = nbFrames * 2;
	int i;
	for (i = 0; i < nbSamples; i += 2, curVolume += volumeIncr) {
		samples[i]     = ((float)samples[i])     * curVolume;
		samples[i + 1] = ((float)samples[i + 1]) * curVolume;
	}
}

// Writes pre-delay sample-frames into the output buffer passed in.
// Pre-delay will most often be silence but can also be a tone at
// SamplingFrequency/2 in case the output device needs some non-silent
// samples to turn on.
static int produce_predelay_frames(slimaudio_t* audio, void* outputBuffer, unsigned int nbBytes)
{
	// FIXME: Asuming 2 channels, 16 bit samples (i.e. 2 bytes)
	const int frameSize = 2 * 2;
	unsigned int predelayFrames = audio->output_predelay_frames;
	const unsigned int maxFrames = nbBytes / frameSize;
	
	if (predelayFrames > maxFrames) {
	  	predelayFrames = maxFrames;
	}
	
	DEBUGF("slimaudio_output: producing %i predelay frames\n",
	       predelayFrames);
	
	const unsigned int predelayBytes = predelayFrames * frameSize;
	if (audio->output_predelay_amplitude == 0) {
		memset((char*)outputBuffer, 0, predelayBytes);
	}
	else {
		// Producing a low-volume high-frequency tone that will wake up
		// stubborn DACs.  Not very dog-friendly, but some DACs will 
		// only turn on (too late) at the presence of sound of a certain
		// level.
		// FIXME: This code assumes stereo signed 16-bit sample frames.
		short* const samples = (short*)outputBuffer;
		short val = audio->output_predelay_amplitude <= SHRT_MAX ? 
			audio->output_predelay_amplitude : SHRT_MAX;
		if ( val & 1 ) {
			// Make sure we alternate positive and negative values
			// even if the pre-delay is broken-down over multiple
			// calls.
			val = -val;
		}
		int i;
		for ( i = 0; i < predelayFrames; ++i, val = -val ) {
			samples[ 2 * i ] = samples[ 2 * i + 1] = val;
		}
	}
	
	audio->output_predelay_frames -= predelayFrames;
	return predelayBytes;
}

static int pa_callback(void *inputBuffer, void *outputBuffer,
			unsigned long framesPerBuffer,
			PaTimestamp outTime, void *userData)
{
	slimaudio_t * const audio = (slimaudio_t *) userData;

	// FIXME: Asuming 2 channels, 16 bit samples (i.e. 2 bytes)
	const int frameSize = 2 * 2;
	const int len = framesPerBuffer * frameSize; 
	
	int off = 0;
	while (audio->output_state == PLAYING && (len - off) > 0 ) {
		if (audio->output_predelay_frames > 0) {
			off += produce_predelay_frames(
				audio, (char*)outputBuffer + off, len - off);
			continue;
		}

		int data_len = len-off;
		const slimaudio_buffer_status ok = 
			slimaudio_buffer_read(
				audio->output_buffer, 
				(char *) outputBuffer+off, &data_len);

		if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
			/* stream closed */
				
			if (slimaudio_buffer_available(audio->output_buffer) 
			    == 0) {
				pthread_mutex_lock(&audio->output_mutex);

				// Send buffer underrun to SqueezeCenter. During
				// normal play this indicates the end of the
				// playlist. During sync this starts the next 
				// track.
				audio->output_STMu = true;

				pthread_cond_broadcast(&audio->output_cond);
				pthread_mutex_unlock(&audio->output_mutex);
			}
		}
		else if (ok == SLIMAUDIO_BUFFER_STREAM_START) {
			pthread_mutex_lock(&audio->output_mutex);
				
			// Send track start to SqueezeCenter. During normal play
			// this advances the playlist.
			audio->output_STMs = true;

			pthread_cond_broadcast(&audio->output_cond);
			pthread_mutex_unlock(&audio->output_mutex);
		}

		off += data_len;

		/* if we have underrun fill remaining buffer with silence */
		if (data_len == 0) {
			memset((char *)outputBuffer+off, 0, len-off);
			off = len;
		}
	}

	const int uninitSize = len - off;
	if (uninitSize > 0) {
	 	DEBUGF("slimaudio_output: pa_callback uninitialized bytes: %i\n", 
		       uninitSize);
		memset((char *)outputBuffer+off, 0, uninitSize);
	}

	if (audio->volume_control == VOLUME_SOFTWARE) {
		apply_software_volume(audio, outputBuffer, framesPerBuffer);
	}

//	printf("pa_callback complete framePerBuffer=%i n=%i\n", framesPerBuffer, n);
	
	return 0;
}


