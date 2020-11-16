#include "sound.h"

#include "graphics/themefilenames.h"
#include "common/dsimenusettings.h"
// #include "soundbank_bin.h"
#include "streamingaudio.h"
#include "string.h"
#include "common/tonccpy.h"
#include <algorithm>

#define SFX_STARTUP		0
#define SFX_WRONG		1
#define SFX_LAUNCH		2
#define SFX_STOP		3
#define SFX_SWITCH		4
#define SFX_SELECT		5
#define SFX_BACK		6

#define MOD_MENU        0

#define MSL_NSONGS		0
#define MSL_NSAMPS		7
#define MSL_BANKSIZE	7


extern volatile s16 fade_counter;
extern volatile bool fade_out;

extern volatile s16* play_stream_buf;
extern volatile s16* fill_stream_buf;

// Number of samples filled into the fill buffer so far.
extern volatile s32 filled_samples;

extern volatile bool fill_requested;
extern volatile s32 samples_left_until_next_fill;
extern volatile s32 streaming_buf_ptr;

#define SAMPLES_USED (STREAMING_BUF_LENGTH - samples_left)
#define REFILL_THRESHOLD STREAMING_BUF_LENGTH >> 2

#ifdef SOUND_DEBUG
extern char debug_buf[256];
#endif

extern volatile u32 sample_delay_count;

volatile char SFX_DATA[0x8D000] = {0};

SoundControl::SoundControl()
	: stream_is_playing(false), stream_source(NULL), startup_sample_length(0)
 {

	FILE* soundbank_file;

	if (ms().theme == 4) {
		soundbank_file = fopen(std::string(TFN_SATURN_SOUND_EFFECTBANK).c_str(), "rb");
	} else {
		switch(ms().dsiMusic) {
			case 2:
				soundbank_file = fopen(std::string(TFN_SHOP_SOUND_EFFECTBANK).c_str(), "rb");
				break;
			case 3:
				soundbank_file = fopen(std::string(TFN_SOUND_EFFECTBANK).c_str(), "rb");
				if (soundbank_file) break; // fallthrough if soundbank_file fails.
			case 1:
			default:
				soundbank_file = fopen(std::string(TFN_DEFAULT_SOUND_EFFECTBANK).c_str(), "rb");
				break;
		}
	}

	fread((void*)SFX_DATA, 1, sizeof(SFX_DATA), soundbank_file);

	fclose(soundbank_file);

	// Since SFX_STARTUP is the first sample, it begins at 0x10 after the
	// *maxmod* header. Subtract the size of the sample header,
	// and divide by two to get length in samples.
	// https://github.com/devkitPro/mmutil/blob/master/source/msl.c#L80
	
	startup_sample_length = (((*(u32*)(SFX_DATA + 0x10)) - 20) >> 1);

	// sprintf(debug_buf, "Read sample length %li for startup", startup_sample_length);
    // nocashMessage(debug_buf);

	if (ms().dsiMusic < 2) {
		mmInitDefaultMem((mm_addr)SFX_DATA);
	} else {
		sys.mod_count = MSL_NSONGS;
		sys.samp_count = MSL_NSAMPS;
		sys.mem_bank = new mm_word[MSL_BANKSIZE];
		sys.fifo_channel = FIFO_MAXMOD;

		mmInit(&sys);
		mmSoundBankInMemory((mm_addr)SFX_DATA);
	}

	mmLoadEffect(SFX_LAUNCH);
	mmLoadEffect(SFX_SELECT);
	mmLoadEffect(SFX_STOP);
	mmLoadEffect(SFX_WRONG);
	mmLoadEffect(SFX_BACK);
	mmLoadEffect(SFX_SWITCH);
	mmLoadEffect(SFX_STARTUP);
	// mmLoadEffect(SFX_MENU);

	snd_launch = {
	    {SFX_LAUNCH},	    // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	snd_select = {
	    {SFX_SELECT},	    // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	snd_stop = {
	    {SFX_STOP},		     // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	snd_wrong = {
	    {SFX_WRONG},	     // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	snd_back = {
	    {SFX_BACK},		     // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	snd_switch = {
	    {SFX_SWITCH},	    // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};
	mus_startup = {
	    {SFX_STARTUP},	   // id
	    (int)(1.0f * (1 << 10)), // rate
	    0,			     // handle
	    255,		     // volume
	    128,		     // panning
	};


	if (ms().dsiMusic == 0) {
		return;
	}

	if (ms().dsiMusic == 1) {
		mmLoad(MOD_MENU);
		mmSetModuleVolume(500);
		return;
	}

	bool isDSi = (isDSiMode() || REG_SCFG_EXT != 0);
	bool loopableMusic = false;

	stream.sampling_rate = 16000;	 		// 16000Hz

	if (ms().theme == 4) {
		stream_source = fopen(std::string(TFN_DEFAULT_SOUND_BG).c_str(), "rb");
	} else {
		switch(ms().dsiMusic) {
			case 5:
				if (isDSi) {
					stream.sampling_rate = 22050;	 		// 22050Hz
				}
				stream_start_source = fopen(std::string(isDSi ? TFN_HBL_START_SOUND_BG : TFN_HBL_START_DS_SOUND_BG).c_str(), "rb");
				stream_source = fopen(std::string(isDSi ? TFN_HBL_LOOP_SOUND_BG : TFN_HBL_LOOP_DS_SOUND_BG).c_str(), "rb");
				loopableMusic = true;
				break;
			case 4:
				stream_source = fopen(std::string(TFN_CLASSIC_SOUND_BG).c_str(), "rb");
				break;
			case 2:
			default:
				stream_source = fopen(std::string(TFN_SHOP_SOUND_BG).c_str(), "rb");
				break;
			case 3:
				stream_source = fopen(std::string(TFN_SOUND_BG).c_str(), "rb");
			case 1:
				break;
		}
	}
	

	fseek(stream_source, 0, SEEK_SET);

	stream.buffer_length = (isDSi ? 0x1000 : 800);	  			// should be adequate
	stream.callback = on_stream_request;    
	stream.format = MM_STREAM_16BIT_MONO;  // select format
	stream.timer = MM_TIMER0;	    	   // use timer0
	stream.manual = false;	      		   // auto filling
	
	if (loopableMusic) {
		fseek(stream_start_source, 0, SEEK_END);
		size_t fileSize = ftell(stream_start_source);
		fseek(stream_start_source, 0, SEEK_SET);

		// Prep the first section of the stream
		fread((void*)play_stream_buf, sizeof(s16), STREAMING_BUF_LENGTH, stream_start_source);
		if (fileSize < STREAMING_BUF_LENGTH*sizeof(s16)) {
			size_t fillerSize = 0;
			while (fileSize+fillerSize < STREAMING_BUF_LENGTH*sizeof(s16)) {
				fillerSize++;
			}
			fread((void*)play_stream_buf+fileSize, 1, fillerSize, stream_source);

			// Fill the next section premptively
			fread((void*)fill_stream_buf, sizeof(s16), STREAMING_BUF_LENGTH, stream_source);

			//loopingPoint = true;
		} else {
			// Fill the next section premptively
			fread((void*)fill_stream_buf, sizeof(s16), STREAMING_BUF_LENGTH, stream_start_source);
			fileSize -= STREAMING_BUF_LENGTH*sizeof(s16);
			if (fileSize < STREAMING_BUF_LENGTH*sizeof(s16)) {
				size_t fillerSize = 0;
				while (fileSize+fillerSize < STREAMING_BUF_LENGTH*sizeof(s16)) {
					fillerSize++;
				}
				fread((void*)fill_stream_buf+fileSize, 1, fillerSize, stream_source);

				//loopingPoint = true;
			}
		}
	} else {
		// Prep the first section of the stream
		fread((void*)play_stream_buf, sizeof(s16), STREAMING_BUF_LENGTH, stream_source);

		// Fill the next section premptively
		fread((void*)fill_stream_buf, sizeof(s16), STREAMING_BUF_LENGTH, stream_source);
	}
}

mm_sfxhand SoundControl::playLaunch() { return mmEffectEx(&snd_launch); }
mm_sfxhand SoundControl::playSelect() { return mmEffectEx(&snd_select); }
mm_sfxhand SoundControl::playBack() { return mmEffectEx(&snd_back); }
mm_sfxhand SoundControl::playSwitch() { return mmEffectEx(&snd_switch); }
mm_sfxhand SoundControl::playStartup() { return mmEffectEx(&mus_startup); }
mm_sfxhand SoundControl::playStop() { return mmEffectEx(&snd_stop); }
mm_sfxhand SoundControl::playWrong() { return mmEffectEx(&snd_wrong); }

void SoundControl::beginStream() {
	if (ms().dsiMusic == 1) {
		mmStart(MOD_MENU, MM_PLAY_LOOP);
		return;
	}

	// open the stream
	stream_is_playing = true;
	mmStreamOpen(&stream);
	SetYtrigger(0);
}

void SoundControl::stopStream() {
	if (ms().dsiMusic == 1 && mmActive()) {
		mmStop();
		return;
	}
	if (!stream_is_playing) return;
	stream_is_playing = false;
	mmStreamClose();
}

void SoundControl::fadeOutStream() {
	fade_out = true;
	if (ms().dsiMusic == 1) {
		fifoSendValue32(FIFO_USER_01, 1); // Sound fade-out workaround for module/sequenced music
	}
}

void SoundControl::cancelFadeOutStream() {
	fade_out = false;
	fade_counter = FADE_STEPS;
	if (ms().dsiMusic == 1) {
		fifoSendValue32(FIFO_USER_01, 0);
	}
}

void SoundControl::setStreamDelay(u32 delay) {
	sample_delay_count = delay;
}


// Samples remaining in the fill buffer.
#define SAMPLES_LEFT_TO_FILL (abs(STREAMING_BUF_LENGTH - filled_samples))

// Samples that were already streamed and need to be refilled into the buffer.
#define SAMPLES_TO_FILL (abs(streaming_buf_ptr - filled_samples))

// Updates the background music fill buffer
// Fill the amount of samples that were used up between the
// last fill request and this.

// Precondition Invariants:
// filled_samples <= STREAMING_BUF_LENGTH
// filled_samples <= streaming_buf_ptr

// Postcondition Invariants:
// filled_samples <= STREAMING_BUF_LENGTH
// filled_samples <= streaming_buf_ptr
// fill_requested == false
volatile void SoundControl::updateStream() {
	
	if (!stream_is_playing) return;
	if (fill_requested && filled_samples < STREAMING_BUF_LENGTH) {
			
		// Reset the fill request
		fill_requested = false;
		int instance_filled = 0;

		// Either fill the max amount, or fill up the buffer as much as possible.
		int instance_to_fill = std::min(SAMPLES_LEFT_TO_FILL, SAMPLES_TO_FILL);

		// If we don't read enough samples, loop from the beginning of the file.
		instance_filled = fread((s16*)fill_stream_buf + filled_samples, sizeof(s16), instance_to_fill, stream_source);		
		if (instance_filled < instance_to_fill) {
			fseek(stream_source, 0, SEEK_SET);
			instance_filled += fread((s16*)fill_stream_buf + filled_samples + instance_filled,
				 sizeof(s16), (instance_to_fill - instance_filled), stream_source);
		}

		#ifdef SOUND_DEBUG
		sprintf(debug_buf, "FC: SAMPLES_LEFT_TO_FILL: %li, SAMPLES_TO_FILL: %li, instance_filled: %i, filled_samples %li, to_fill: %i", SAMPLES_LEFT_TO_FILL, SAMPLES_TO_FILL, instance_filled, filled_samples, instance_to_fill);
    	nocashMessage(debug_buf);
		#endif

		// maintain invariant 0 < filled_samples <= STREAMING_BUF_LENGTH
		filled_samples = std::min<s32>(filled_samples + instance_filled, STREAMING_BUF_LENGTH);

	
	} else if (fill_requested && filled_samples >= STREAMING_BUF_LENGTH) {
		// filled_samples == STREAMING_BUF_LENGTH is the only possible case
		// but we'll keep it at gte to be safe.
		filled_samples = 0;
		// fill_count = 0;
	}

}
