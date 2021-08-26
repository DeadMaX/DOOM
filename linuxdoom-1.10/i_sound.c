// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	System interface for sound.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>

#include <math.h>

#ifdef WIN32

#include <XAudio2.h>

#else

#include <sys/time.h>
#include <sys/types.h>

#ifndef LINUX
#include <sys/filio.h>
#endif

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// Linux voxware output.
#include <linux/soundcard.h>

#endif

// Timer stuff. Experimental.
#include <time.h>
#include <signal.h>

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"

// UNIX hack, to be removed.
#ifdef SNDSERV
// Separate sound server process.
FILE*	sndserver=0;
char*	sndserver_filename = "./sndserver ";
#elif SNDINTR

// Update all 30 millisecs, approx. 30fps synchronized.
// Linux resolution is allegedly 10 millisecs,
//  scale is microseconds.
#define SOUND_INTERVAL     500

// Get the interrupt. Set duration in millisecs.
int I_SoundSetTimer( int duration_of_tick );
void I_SoundDelTimer( void );
#else
// None?
#endif


// A quick hack to establish a protocol between
// synchronous mix buffer updates and asynchronous
// audio writes. Probably redundant with gametic.
static int flag = 0;

// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.


// Needed for calling the actual sound output.
#define SAMPLERATE		11025	// Hz
#define SAMPLESIZE		2   	// 16bit

#define SAMPLECOUNT		(SAMPLERATE / TICRATE)
#define NUM_CHANNELS		8
// It is 2 for 16bit, and 2 for two channels.
#define BUFMUL                  2
#define MIXBUFFERSIZE		(SAMPLECOUNT*BUFMUL)



// The actual lengths of all sound effects.
int 		lengths[NUMSFX];

// The actual output device.
#ifdef WIN32
IXAudio2* pXAudio2;
IXAudio2MasteringVoice* pMasterVoice;
IXAudio2SourceVoice* pSourceVoice;
XAUDIO2_BUFFER audio_buffer[2];
#else
int	audio_fd;
#endif

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
static signed short	mixbuffer[2][MIXBUFFERSIZE];
static unsigned int curr_mixbuffer = 0;


// The channel step amount...
unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int	channelstepremainder[NUM_CHANNELS];


// The channel data pointers, start and end.
unsigned char*	channels[NUM_CHANNELS];
unsigned char*	channelsend[NUM_CHANNELS];


// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
int 		channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
int		steptable[256];

// Volume lookups.
int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int*		channelleftvol_lookup[NUM_CHANNELS];
int*		channelrightvol_lookup[NUM_CHANNELS];


#ifndef WIN32

//
// Safe ioctl, convenience.
//
void
myioctl
( int	fd,
  int	command,
  int*	arg )
{   
    int		rc;
    
    rc = ioctl(fd, command, arg);  
    if (rc < 0)
    {
	fprintf(stderr, "ioctl(dsp,%d,arg) failed\n", command);
	fprintf(stderr, "errno=%d\n", errno);
	exit(-1);
    }
}


#endif


//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
      sfxlump = W_GetNumForName("dspistol");
    else
      sfxlump = W_GetNumForName(name);
    
    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    Z_Free( sfx );
    
    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}





//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int
addsfx
( int		sfxid,
  int		volume,
  int		step,
  int		seperation )
{
    static unsigned short	handlenums = 0;
 
    int		i;
    int		rc = -1;
    
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	// Loop all channels, check.
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    // Active, and using the same SFX?
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		// Reset.
		channels[i] = 0;
		// We are sure that iff,
		//  there will only be one.
		break;
	    }
	}
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
	if (channelstart[i] < oldest)
	{
	    oldestnum = i;
	    oldest = channelstart[i];
	}
    }

    // Tales from the cryptic.
    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    // Probably only happens at startup.
    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];

    // Reset current handle number, limited to 0..100.
    if (!handlenums)
	handlenums = 100;

    // Assign current handle number.
    // Preserved so sounds could be stopped (unused).
    channelhandles[slot] = rc = handlenums++;

    // Set stepping???
    // Kinda getting the impression this is never used.
    channelstep[slot] = step;
    // ???
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}





//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  int		i;
  int		j;
    
  int*	steptablemid = steptable + 128;
  
  // Okay, reset internal mixing channels to zero.
  /*for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }*/

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (i=-128 ; i<128 ; i++)
    steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);
  
  
  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  for (i=0 ; i<128 ; i++)
    for (j=0 ; j<256 ; j++)
      vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}	

 
void I_SetSfxVolume(int volume)
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  snd_SfxVolume = volume;
}

// MUSIC API - dummy. Some code from DOS version.
void I_SetMusicVolume(int volume)
{
  // Internal state variable.
  snd_MusicVolume = volume;
  // Now set volume on output device.
  // Whatever( snd_MusciVolume );
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{

  // UNUSED
  priority = 0;
  
#ifdef SNDSERV 
    if (sndserver)
    {
	fprintf(sndserver, "p%2.2x%2.2x%2.2x%2.2x\n", id, pitch, vol, sep);
	fflush(sndserver);
    }
    // warning: control reaches end of non-void function.
    return id;
#else
    // Debug.
    //fprintf( stderr, "starting sound %d", id );
    
    // Returns a handle (not used).
    id = addsfx( id, vol, steptable[pitch], sep );

    // fprintf( stderr, "/handle is %d\n", id );
    
    return id;
#endif
}



void I_StopSound (int handle)
{
  // You need the handle returned by StartSound.
  // Would be looping all channels,
  //  tracking down the handle,
  //  an setting the channel to zero.
  
  // UNUSED.
  handle = 0;
}


int I_SoundIsPlaying(int handle)
{
    // Ouch.
    return gametic < handle;
}




//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//
void I_UpdateSound( void )
{
#ifdef SNDINTR
  // Debug. Count buffer misses with interrupt.
  static int misses = 0;
#endif

  
  // Mix current sound data.
  // Data, from raw sound, for right and left.
  register unsigned int	sample;
  register int		dl;
  register int		dr;
  
  // Pointers in global mixbuffer, left, right, end.
  signed short*		leftout;
  signed short*		rightout;
  signed short*		leftend;
  // Step in mixbuffer, left and right, thus two.
  int				step;

  // Mixing channel index.
  int				chan;
    
    // Left and right channel
    //  are in global mixbuffer, alternating.
    leftout = mixbuffer[curr_mixbuffer];
    rightout = leftout+1;
    step = 2;

    // Determine end, for left channel only
    //  (right channel is implicit).
    leftend = leftout + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT,
    //  that is 512 values for two channels.
    while (leftout != leftend)
    {
	// Reset left/right value. 
	dl = 0;
	dr = 0;

	// Love thy L2 chache - made this a loop.
	// Now more channels could be set at compile time
	//  as well. Thus loop those  channels.
	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    // Check channel, if active.
	    if (channels[ chan ])
	    {
		// Get the raw data from the channel. 
		sample = *channels[ chan ];
		// Add left and right part
		//  for this channel (sound)
		//  to the current data.
		// Adjust volume accordingly.
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		// Increment index ???
		channelstepremainder[ chan ] += channelstep[ chan ];
		// MSB is next sample???
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		// Limit to LSB???
		channelstepremainder[ chan ] &= 65536-1;

		// Check whether we are done.
		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}
	
	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff)
	    *leftout = 0x7fff;
	else if (dl < -0x8000)
	    *leftout = -0x8000;
	else
	    *leftout = dl;

	// Same for right hardware channel.
	if (dr > 0x7fff)
	    *rightout = 0x7fff;
	else if (dr < -0x8000)
	    *rightout = -0x8000;
	else
	    *rightout = dr;

	// Increment current pointers in mixbuffer.
	leftout += step;
	rightout += step;
    }

    curr_mixbuffer = 1 - curr_mixbuffer;
#ifdef SNDINTR
    // Debug check.
    if ( flag )
    {
      misses += flag;
      flag = 0;
    }
    
    if ( misses > 10 )
    {
      fprintf( stderr, "I_SoundUpdate: missed 10 buffer writes\n");
      misses = 0;
    }
    
    // Increment flag for update.
    flag++;
#endif
}


// 
// This would be used to write out the mixbuffer
//  during each game loop update.
// Updates sound buffer and audio device at runtime. 
// It is called during Timer interrupt with SNDINTR.
// Mixing now done synchronous, and
//  only output be done asynchronous?
//
void
I_SubmitSound(void)
{
#ifdef WIN32

    XAUDIO2_VOICE_STATE soundState = { 0 };

    HRESULT res = IXAudio2SourceVoice_SubmitSourceBuffer(pSourceVoice, &audio_buffer[1 - curr_mixbuffer], NULL);
    if (FAILED(res))
    {
        exit(-1);
    }

    do
    {
        IXAudio2SourceVoice_GetState(pSourceVoice, &soundState, 0);
    } while (soundState.BuffersQueued > 1);
#else
  // Write it to DSP device.
  write(audio_fd, mixbuffer, SAMPLECOUNT*BUFMUL);
#endif
}



void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch)
{
  // I fail too see that this is used.
  // Would be using the handle to identify
  //  on which channel the sound might be active,
  //  and resetting the channel parameters.

  // UNUSED.
  handle = vol = sep = pitch = 0;
}




void I_ShutdownSound(void)
{    
#ifdef SNDSERV
  if (sndserver)
  {
    // Send a "quit" command.
    fprintf(sndserver, "q\n");
    fflush(sndserver);
  }
#else
  // Wait till all pending sounds are finished.
  int done = 0;
  int i;
  

  // FIXME (below).
  fprintf( stderr, "I_ShutdownSound: NOT finishing pending sounds\n");
  fflush( stderr );
  
  while ( !done )
  {
    for( i=0 ; i<8 && !channels[i] ; i++);
    
    // FIXME. No proper channel output.
    //if (i==8)
    done=1;
  }
#ifdef SNDINTR
  I_SoundDelTimer();
#endif
  
#ifdef WIN32
  IXAudio2SourceVoice_Stop(pSourceVoice, 0, XAUDIO2_COMMIT_NOW);
  IXAudio2SourceVoice_DestroyVoice(pSourceVoice);
  pSourceVoice = NULL;
  IXAudio2Voice_DestroyVoice(pMasterVoice);
  pMasterVoice = NULL;
  IXAudio2_Release(pXAudio2);
  pXAudio2 = NULL;
#else
  // Cleaning up -releasing the DSP device.
  close ( audio_fd );
#endif
#endif

  // Done.
  return;
}






void
I_InitSound()
{ 
#ifdef SNDSERV
  char buffer[256];
  
  if (getenv("DOOMWADDIR"))
    sprintf(buffer, "%s/%s",
	    getenv("DOOMWADDIR"),
	    sndserver_filename);
  else
    sprintf(buffer, "%s", sndserver_filename);
  
  // start sound process
  if ( !access(buffer, X_OK) )
  {
    strcat(buffer, " -quiet");
    sndserver = popen(buffer, "w");
  }
  else
    fprintf(stderr, "Could not start sound server [%s]\n", buffer);
#else
    
  int i;
  
#ifdef SNDINTR
  fprintf( stderr, "I_SoundSetTimer: %d microsecs\n", SOUND_INTERVAL );
  I_SoundSetTimer( SOUND_INTERVAL );
#endif
    
  // Secure and configure sound device first.
  fprintf( stderr, "I_InitSound: ");

#ifdef WIN32

  audio_buffer[0].pAudioData = mixbuffer[0];
  audio_buffer[0].Flags = 0;
  audio_buffer[0].AudioBytes = sizeof(mixbuffer[0]);
  audio_buffer[1].pAudioData = mixbuffer[1];
  audio_buffer[1].Flags = 0;
  audio_buffer[1].AudioBytes = sizeof(mixbuffer[1]);

  HRESULT res = CoInitialize(NULL);
  if (FAILED(res))
  {
      exit(-1);
  }

  res = XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(res))
  {
      pXAudio2 = NULL;
      exit(-1);
  }

  res = IXAudio2_CreateMasteringVoice(pXAudio2, &pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects);
    if (FAILED(res))
    {
        IXAudio2_Release(pXAudio2);
        pXAudio2 = NULL;
        exit(-1);
    }

    WAVEFORMATEX fmt = { 0 };
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2; // stereo
    fmt.nSamplesPerSec = SAMPLERATE;
    fmt.wBitsPerSample = 16;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nBlockAlign = (fmt.wBitsPerSample * fmt.nChannels) / 8;
    fmt.cbSize = 0;

    res = IXAudio2_CreateSourceVoice(pXAudio2, &pSourceVoice, &fmt,
        0,
        XAUDIO2_DEFAULT_FREQ_RATIO,
        NULL,
        NULL,
        NULL
    );
    if (FAILED(res))
    {
        IXAudio2Voice_DestroyVoice(pMasterVoice);
        pMasterVoice = NULL;
        IXAudio2_Release(pXAudio2);
        pXAudio2 = NULL;
        exit(-1);
    }

    res = IXAudio2SourceVoice_Start(pSourceVoice, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(res))
    {
        exit(-1);
    }
#else
  
  audio_fd = open("/dev/dsp", O_WRONLY);
  if (audio_fd<0)
    fprintf(stderr, "Could not open /dev/dsp\n");
  
                     
  i = 11 | (2<<16);                                           
  myioctl(audio_fd, SNDCTL_DSP_SETFRAGMENT, &i);
  myioctl(audio_fd, SNDCTL_DSP_RESET, 0);
  
  i=SAMPLERATE;
  
  myioctl(audio_fd, SNDCTL_DSP_SPEED, &i);
  
  i=1;
  myioctl(audio_fd, SNDCTL_DSP_STEREO, &i);
  
  myioctl(audio_fd, SNDCTL_DSP_GETFMTS, &i);
  
  if (i&=AFMT_S16_LE)    
    myioctl(audio_fd, SNDCTL_DSP_SETFMT, &i);
  else
    fprintf(stderr, "Could not play signed 16 data\n");
#endif

  fprintf(stderr, " configured audio device\n" );

    
  // Initialize external data (all sounds) at start, keep static.
  fprintf( stderr, "I_InitSound: ");
  
  for (i=1 ; i<NUMSFX ; i++)
  { 
    // Alias? Example is the chaingun sound linked to pistol.
    if (!S_sfx[i].link)
    {
      // Load data from WAD file.
      S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
    }	
    else
    {
      // Previously loaded already?
      S_sfx[i].data = S_sfx[i].link->data;
      lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
    }
  }

  fprintf( stderr, " pre-cached all sound data\n");
  
  // Now initialize mixbuffer with zero.
  for (int j = 0; j < 2; ++j)
  for ( i = 0; i< MIXBUFFERSIZE; i++ )
    mixbuffer[j][i] = 0;
  
  // Finished initialization.
  fprintf(stderr, "I_InitSound: sound module ready\n");
    
#endif
}

typedef struct
{
    char tag[4];
    unsigned short lenSong;
    unsigned short offSong;
    unsigned short primaryChannels;
    unsigned short secondaryChannels;
    unsigned short numInstruments;
    unsigned short reserved;
    unsigned short inst[13];
} mus_t;

typedef struct
{
    bool used;
    mus_t* data;
} music_t;

#define MAX_MUSIC 32
music_t musics[MAX_MUSIC];

HMIDISTRM     outHandle;
UINT deviceID;
PATCHARRAY patches;
PATCHARRAY drum_patches;
MIDIHDR header;
//
// MUSIC API.
// Still no music done.
// Remains. Dummies.
//
void I_InitMusic(void)
{
    MMRESULT res;

    MIDIOUTCAPS caps;
    for (deviceID = 0;; ++deviceID)
    {
        res = midiOutGetDevCaps(deviceID, &caps, sizeof(caps));
        if (res)
            exit(-1);
        if (caps.wVoices > 10)
            break;
    }
    
    res = midiStreamOpen(&outHandle, &deviceID, 1, NULL, NULL, CALLBACK_NULL);
    if (FAILED(res))
        exit(-1);
#if 1
    MIDIPROPTIMEDIV timediv;
    timediv.cbStruct = sizeof(MIDIPROPTIMEDIV);
    timediv.dwTimeDiv = 70;
    res = midiStreamProperty(outHandle, (LPBYTE)&timediv, MIDIPROP_SET | MIDIPROP_TIMEDIV);
    if (FAILED(res))
        exit(-1);
#else
    MIDIPROPTEMPO tempo;
    tempo.cbStruct = sizeof(MIDIPROPTEMPO);
    tempo.dwTempo = 60000000 / 360;
    res = midiStreamProperty(outHandle, (LPBYTE)&tempo, MIDIPROP_SET | MIDIPROP_TEMPO);
    if (FAILED(res))
        exit(-1);
#endif
}

void I_ShutdownMusic(void)
{
    midiStreamClose(outHandle);
}

static unsigned int minofuint(unsigned int a, unsigned int b)
{
    return min(a, b);
}

static int	looping=0;
static int	musicdies=-1;

static void convert_mus(byte* mus)
{
    typedef enum
    {
        mus_releasekey = 0x00,
        mus_presskey = 0x10,
        mus_pitchwheel = 0x20,
        mus_systemevent = 0x30,
        mus_changecontroller = 0x40,
        mus_endmesure = 0x50,
        mus_scoreend = 0x60
    } musevent;

    static DWORD buffer[0xffff / sizeof(DWORD)];
    byte velocities[16] = { 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 
                            0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};

    header.lpData = buffer;
    header.dwBytesRecorded = 0;
    header.dwBufferLength = sizeof(buffer);
    header.dwFlags = 0;

    byte* p = mus;
    DWORD timer = 0;

    while (true)
    {
        byte channel = *p & 0x0F;
        bool have_delay = !!(*p & 0x80);

        // swap percussion channel of MIDI vs MUS
        if (channel == 15)
            channel = 9;
        else if (channel == 9)
            channel = 15;

        switch (*p++ & 0x70)
        {
        case mus_releasekey:
            buffer[header.dwBytesRecorded++] = timer; timer = 0;
            buffer[header.dwBytesRecorded++] = 0;
            buffer[header.dwBytesRecorded++] = (velocities[channel] << 16) | (((*p++) &0x7f) << 8) | (0x80) | channel;
            break;

        case mus_presskey:
            buffer[header.dwBytesRecorded++] = timer; timer = 0;
            buffer[header.dwBytesRecorded++] = 0;
            if (!!(*p & 0x80))
            {
                byte note = (*p++) & 0x7f;
                velocities[channel] = (*p++) & 0x7f;
                buffer[header.dwBytesRecorded++] = (velocities[channel] << 16) | (note << 8) | (0x90) | channel;
            }
            else
                buffer[header.dwBytesRecorded++] = (velocities[channel] << 16) | (((*p++) & 0x7f) << 8) | (0x90) | channel;
            break;


        case mus_pitchwheel:
        {
            DWORD pitch = (*p++) * 64;
            pitch = (pitch & 0x7f) | (pitch & 0x3f80) << 1;
            buffer[header.dwBytesRecorded++] = timer; timer = 0;
            buffer[header.dwBytesRecorded++] = 0;
            buffer[header.dwBytesRecorded++] = (pitch << 8) | (0xE0) | channel;
        }
            break;

        case mus_systemevent:
            switch (*p++)
            {
            case 10:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = 0x78;
                break;

            case 11:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = 0x7B;
                break;

            case 12:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = 0x7E;
                break;

            case 13:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = 0x7F;
                break;

            case 14:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = 0x79;
                break;

            case 15:
                break;

            default:
                header.dwBytesRecorded = 0;
                return;
            }
            break;

        case mus_changecontroller:
            switch (*p++)
            {
            case 0:
                // change instrument
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (((*p++) & 0x7f) << 8) | (0xC0) | channel;
                break;

            case 1:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x20 << 8) | (0xB0) | channel;
            break;

            case 2:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x01 << 8) | (0xB0) | channel;
            break;

            case 3:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x07 << 8) | (0xB0) | channel;
            break;

            case 4:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x0A << 8) | (0xB0) | channel;
            break;

            case 5:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x0B << 8) | (0xB0) | channel;
            break;

            case 6:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x5B << 8) | (0xB0) | channel;
            break;

            case 7:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x5D << 8) | (0xB0) | channel;
            break;

            case 8:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x40 << 8) | (0xB0) | channel;
            break;

            case 9:
                buffer[header.dwBytesRecorded++] = timer; timer = 0;
                buffer[header.dwBytesRecorded++] = 0;
                buffer[header.dwBytesRecorded++] = (minofuint(*p++, 0x7f) << 16) | (0x43 << 8) | (0xB0) | channel;
                break;

            default:
                header.dwBytesRecorded = 0;
                return;
                break;
            }
            break;

        case mus_endmesure:
            break;

        case mus_scoreend:
            buffer[header.dwBytesRecorded++] = timer; timer = 0;
            buffer[header.dwBytesRecorded++] = 0;
            buffer[header.dwBytesRecorded++] = 0x78;
            header.dwBytesRecorded *= sizeof(DWORD);
            return;
            break;

        default:
            header.dwBytesRecorded = 0;
            return;
            break;
        }
        if (have_delay)
        {
            int add_delay = 0;
            bool continue_read = true;
            while (continue_read)
            {
                continue_read = !!(*p & 0x80);
                add_delay <<= 7;
                add_delay += *p++ & 0x7f;
            }
            timer += add_delay;
        }
    }
}

void I_PlaySong(int handle, int looping)
{
  // UNUSED.
  looping = 0;
  handle--;

  musicdies = gametic + TICRATE*30;
  int cur_patch = 0;
  int cur_drum_patch = 0;

  memset(&patches, 0, sizeof(patches));
  memset(&drum_patches, 0, sizeof(drum_patches));
  for (int i = 0; i < musics[handle].data->numInstruments; i++)
  {
      if (musics[handle].data->inst[i] < 128)
          patches[cur_patch++] = musics[handle].data->inst[i];
      else
          drum_patches[cur_drum_patch] = musics[handle].data->inst[i] - 100;
  }
  
  MMRESULT res = midiOutCachePatches(
      (HMIDIOUT)outHandle,
      0,
      patches,
      MIDI_CACHE_ALL
    );
  if (FAILED(res))
      exit(-1);

   res = midiOutCacheDrumPatches(
       (HMIDIOUT)outHandle,
       0,
       drum_patches,
       MIDI_CACHE_ALL
    );
   if (FAILED(res))
       exit(-1);

   convert_mus(((byte*)musics[handle].data) + musics[handle].data->offSong);

   res = midiOutPrepareHeader((HMIDIOUT)outHandle, &header, sizeof(MIDIHDR));
   if (FAILED(res))
       exit(-1);
   res = midiStreamOut(
       outHandle,
       &header,
       sizeof(MIDIHDR)
      );

   if (FAILED(res))
       exit(-1);
   res = midiStreamRestart(outHandle);
   if (FAILED(res))
       exit(-1);
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;
  midiStreamPause(outHandle);
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;
  midiStreamRestart(outHandle);
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;
  
  looping = 0;
  musicdies = 0;
  midiStreamStop(outHandle);
  midiOutUnprepareHeader((HMIDIOUT)outHandle, &header, sizeof(MIDIHDR));
}

void I_UnRegisterSong(int handle)
{
    musics[handle].used = false;
    musics[handle].data = NULL;
}

int I_RegisterSong(void* data)
{
    for (int i = 0; i < MAX_MUSIC; ++i)
    {
        if (musics[i].used)
            continue;

        musics[i].used = true;
        musics[i].data = data;
        return i + 1;
    }
  return 0;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  // UNUSED.
  handle = 0;
  return looping || musicdies > gametic;
}


#ifdef WIN32

void I_HandleSoundTimer(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
{
    if (flag)
    {
        I_SubmitSound();
        flag = 0;
    }
}

int I_SoundSetTimer(int duration_of_tick)
{
    MMRESULT res = timeSetEvent(
        duration_of_tick, // delay
        0,                     // resolution (global variable)
        (LPTIMECALLBACK)&I_HandleSoundTimer,   // callback function
        0,                     // user data
        TIME_PERIODIC);
    if (FAILED(res))
        exit(-1);
}

#else

//
// Experimental stuff.
// A Linux timer interrupt, for asynchronous
//  sound output.
// I ripped this out of the Timer class in
//  our Difference Engine, including a few
//  SUN remains...
//  
#ifdef sun
    typedef     sigset_t        tSigSet;
#else    
    typedef     int             tSigSet;
#endif


// We might use SIGVTALRM and ITIMER_VIRTUAL, if the process
//  time independend timer happens to get lost due to heavy load.
// SIGALRM and ITIMER_REAL doesn't really work well.
// There are issues with profiling as well.
static int /*__itimer_which*/  itimer = ITIMER_REAL;

static int sig = SIGALRM;

// Interrupt handler.
void I_HandleSoundTimer( int ignore )
{
  // Debug.
  //fprintf( stderr, "%c", '+' ); fflush( stderr );
  
  // Feed sound device if necesary.
  if ( flag )
  {
    // See I_SubmitSound().
    // Write it to DSP device.
    write(audio_fd, mixbuffer, SAMPLECOUNT*BUFMUL);

    // Reset flag counter.
    flag = 0;
  }
  else
    return;
  
  // UNUSED, but required.
  ignore = 0;
  return;
}

// Get the interrupt. Set duration in millisecs.
int I_SoundSetTimer( int duration_of_tick )
{
  // Needed for gametick clockwork.
  struct itimerval    value;
  struct itimerval    ovalue;
  struct sigaction    act;
  struct sigaction    oact;

  int res;
  
  // This sets to SA_ONESHOT and SA_NOMASK, thus we can not use it.
  //     signal( _sig, handle_SIG_TICK );
  
  // Now we have to change this attribute for repeated calls.
  act.sa_handler = I_HandleSoundTimer;
#ifndef sun    
  //ac	t.sa_mask = _sig;
#endif
  act.sa_flags = SA_RESTART;
  
  sigaction( sig, &act, &oact );

  value.it_interval.tv_sec    = 0;
  value.it_interval.tv_usec   = duration_of_tick;
  value.it_value.tv_sec       = 0;
  value.it_value.tv_usec      = duration_of_tick;

  // Error is -1.
  res = setitimer( itimer, &value, &ovalue );

  // Debug.
  if ( res == -1 )
    fprintf( stderr, "I_SoundSetTimer: interrupt n.a.\n");
  
  return res;
}


#endif

// Remove the interrupt. Set duration to zero.
void I_SoundDelTimer()
{
  // Debug.
  if ( I_SoundSetTimer( 0 ) == -1)
    fprintf( stderr, "I_SoundDelTimer: failed to remove interrupt. Doh!\n");
}
