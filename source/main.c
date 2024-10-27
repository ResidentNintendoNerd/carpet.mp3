/*
 * Fast, threaded Ogg Vorbis audio streaming example using libvorbisidec
 * (also known as libtremor) for libctru on Nintendo 3DS
 * 
 * Adapted to Vorbis by Théo B. (LiquidFenrir), originally written for Opus
 * by Lauren Kelly (thejsa) with help from mtheall.
 * See the opus-decoding example for more details
 * 
 * Last update: 2024-03-31
 */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#include <tremor/ivorbisfile.h>
#include <tremor/ivorbiscodec.h>
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// ---- DEFINITIONS ----

static const char *PATH = "romfs:/carpet.ogg";  // Path to Ogg Vorbis file to play

static const int THREAD_AFFINITY = -1;           // Execute thread on any core
static const int THREAD_STACK_SZ = 32 * 1024;    // 32kB stack for audio thread

// ---- END DEFINITIONS ----

ndspWaveBuf s_waveBufs[3];
int16_t *s_audioBuffer = NULL;

LightEvent s_event;
volatile bool s_quit = false;  // Quit flag

// ---- HELPER FUNCTIONS ----

// Retrieve strings for libvorbisidec errors
const char *vorbisStrError(int error)
{
    switch(error) {
        case OV_FALSE:
            return "OV_FALSE: A request did not succeed.";
        case OV_HOLE:
            return "OV_HOLE: There was a hole in the page sequence numbers.";
        case OV_EREAD:
            return "OV_EREAD: An underlying read, seek or tell operation "
                   "failed.";
        case OV_EFAULT:
            return "OV_EFAULT: A NULL pointer was passed where none was "
                   "expected, or an internal library error was encountered.";
        case OV_EIMPL:
            return "OV_EIMPL: The stream used a feature which is not "
                   "implemented.";
        case OV_EINVAL:
            return "OV_EINVAL: One or more parameters to a function were "
                   "invalid.";
        case OV_ENOTVORBIS:
            return "OV_ENOTVORBIS: This is not a valid Ogg Vorbis stream.";
        case OV_EBADHEADER:
            return "OV_EBADHEADER: A required header packet was not properly "
                   "formatted.";
        case OV_EVERSION:
            return "OV_EVERSION: The ID header contained an unrecognised "
                   "version number.";
        case OV_EBADPACKET:
            return "OV_EBADPACKET: An audio packet failed to decode properly.";
        case OV_EBADLINK:
            return "OV_EBADLINK: We failed to find data we had seen before or "
                   "the stream was sufficiently corrupt that seeking is "
                   "impossible.";
        case OV_ENOSEEK:
            return "OV_ENOSEEK: An operation that requires seeking was "
                   "requested on an unseekable stream.";
        default:
            return "Unknown error.";
    }
}

// Pause until user presses a button
void waitForInput(void) {
    printf("Press any button to exit...\n");
    while(aptMainLoop())
    {
        gspWaitForVBlank();
        gfxSwapBuffers();
        hidScanInput();

        if(hidKeysDown())
            break;
    }
}

// ---- END HELPER FUNCTIONS ----

// Audio initialisation code
// This sets up NDSP and our primary audio buffer
bool audioInit(OggVorbis_File *vorbisFile_) {
    vorbis_info *vi = ov_info(vorbisFile_, -1);

    // Setup NDSP
    ndspChnReset(0);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, vi->rate);
    ndspChnSetFormat(0, vi->channels == 1
        ? NDSP_FORMAT_MONO_PCM16
        : NDSP_FORMAT_STEREO_PCM16);

    // Allocate audio buffer
    // 120ms buffer
    const size_t SAMPLES_PER_BUF = vi->rate * 120 / 1000;
    // mono (1) or stereo (2)
    const size_t CHANNELS_PER_SAMPLE = vi->channels;
    // s16 buffer
    const size_t WAVEBUF_SIZE = SAMPLES_PER_BUF * CHANNELS_PER_SAMPLE * sizeof(s16);
    const size_t bufferSize = WAVEBUF_SIZE * ARRAY_SIZE(s_waveBufs);
    s_audioBuffer = (int16_t *)linearAlloc(bufferSize);
    if(!s_audioBuffer) {
        printf("Failed to allocate audio buffer\n");
        return false;
    }

    // Setup waveBufs for NDSP
    memset(&s_waveBufs, 0, sizeof(s_waveBufs));
    int16_t *buffer = s_audioBuffer;

    for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
        s_waveBufs[i].data_vaddr = buffer;
        s_waveBufs[i].nsamples   = WAVEBUF_SIZE / sizeof(buffer[0]);
        s_waveBufs[i].status     = NDSP_WBUF_DONE;

        buffer += WAVEBUF_SIZE / sizeof(buffer[0]);
    }

    return true;
}

// Audio de-initialisation code
// Stops playback and frees the primary audio buffer
void audioExit(void) {
    ndspChnReset(0);
    linearFree(s_audioBuffer);
}

// Main audio decoding logic
// This function pulls and decodes audio samples from vorbisFile_ to fill waveBuf_
bool fillBuffer(OggVorbis_File *vorbisFile_, ndspWaveBuf *waveBuf_) {
    #ifdef DEBUG
    // Setup timer for performance stats
    TickCounter timer;
    osTickCounterStart(&timer);
    #endif  // DEBUG

    // Decode (2-byte) samples until our waveBuf is full
    int totalBytes = 0;
    while(totalBytes < waveBuf_->nsamples * sizeof(s16)) {
        int16_t *buffer = waveBuf_->data_pcm16 + (totalBytes / sizeof(s16));
        const size_t bufferSize = (waveBuf_->nsamples * sizeof(s16) - totalBytes);

        // Decode bufferSize bytes from vorbisFile_ into buffer,
        // storing the number of bytes that were read (or error)
        const int bytesRead = ov_read(vorbisFile_, (char *)buffer, bufferSize, NULL);
        if(bytesRead <= 0) {
            if(bytesRead == 0) break;  // No error here

            printf("ov_read: error %d (%s)", bytesRead,
                   vorbisStrError(bytesRead));
            break;
        }
        
        totalBytes += bytesRead;
    }

    // If no samples were read in the last decode cycle, we're done
    if(totalBytes == 0) {
        printf("\nPlayback complete, press Start to exit\n");
		return false;
    }

    // Pass samples to NDSP
    // this calculation will make a number <= the previous nsamples
    // = for most cases
    // < for the last possible chunk of the file, which may have less samples before EOF
    // after which we don't care to recover the length
    waveBuf_->nsamples = totalBytes / sizeof(s16);
    ndspChnWaveBufAdd(0, waveBuf_);
    DSP_FlushDataCache(waveBuf_->data_pcm16, totalBytes);

    #ifdef DEBUG
    // Print timing info
    #endif  // DEBUG

    return true;
}

// NDSP audio frame callback
// This signals the audioThread to decode more things
// once NDSP has played a sound frame, meaning that there should be
// one or more available waveBufs to fill with more data.
void audioCallback(void *const nul_) {
    (void)nul_;  // Unused

    if(s_quit) { // Quit flag
        return;
    }
    
    LightEvent_Signal(&s_event);
}

// Audio thread
// This handles calling the decoder function to fill NDSP buffers as necessary
void audioThread(void *const vorbisFile_) {
    OggVorbis_File *const vorbisFile = (OggVorbis_File *)vorbisFile_;

    while(!s_quit) {  // Whilst the quit flag is unset,
                      // search our waveBufs and fill any that aren't currently
                      // queued for playback (i.e, those that are 'done')
        for(size_t i = 0; i < ARRAY_SIZE(s_waveBufs); ++i) {
            if(s_waveBufs[i].status != NDSP_WBUF_DONE) {
                continue;
            }
            
            if(!fillBuffer(vorbisFile, &s_waveBufs[i])) {   // Playback complete
                return;
            }
        }

        // Wait for a signal that we're needed again before continuing,
        // so that we can yield to other things that want to run
        // (Note that the 3DS uses cooperative threading)
        LightEvent_Wait(&s_event);
    }
}

int main(int argc, char* argv[]) {
    // Initialise platform features
    romfsInit();
    ndspInit();
    gfxInitDefault();

    consoleInit(GFX_TOP, NULL);

	double milisecondsElapsed = 0;

    // Setup LightEvent for synchronisation of audioThread
    LightEvent_Init(&s_event, RESET_ONESHOT);


    // Open the Ogg Vorbis audio file
    OggVorbis_File vorbisFile;
    FILE *fh = fopen(PATH, "rb");
    int error = ov_open(fh, &vorbisFile, NULL, 0);
    if(error) {
        printf("Failed to open file: error %d (%s)\n", error,
               vorbisStrError(error));
        // Only fclose manually if ov_open failed.
        // If ov_open succeeds, fclose happens in ov_clear.
        fclose(fh);
        waitForInput();
    }

    // Attempt audioInit
    if(!audioInit(&vorbisFile)) {
        printf("Failed to initialise audio\n");
        ov_clear(&vorbisFile);
        waitForInput();

        gfxExit();
        ndspExit();
        romfsExit();
        return EXIT_FAILURE;
    }

    // Set the ndsp sound frame callback which signals our audioThread
    ndspSetCallback(audioCallback, NULL);

    // Spawn audio thread

    // Set the thread priority to the main thread's priority ...
    int32_t priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // ... then subtract 1, as lower number => higher actual priority ...
    priority -= 1;
    // ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
    priority = priority < 0x18 ? 0x18 : priority;
    priority = priority > 0x3F ? 0x3F : priority;

    // Start the thread, passing the address of our vorbisFile as an argument.
    const Thread threadId = threadCreate(audioThread, &vorbisFile,
                                         THREAD_STACK_SZ, priority,
                                         THREAD_AFFINITY, false);
    printf("Now Playing: \nBurning Man's Soul - Persona Trinity Soul\n\n");
	
	TickCounter timer;
    osTickCounterStart(&timer);
	int lyricCount = 0;
	
    // Standard main loop
    while(aptMainLoop())
    {
        gspWaitForVBlank();
        gfxSwapBuffers();
        hidScanInput();
		
        // Your code goes here
		osTickCounterUpdate(&timer);
		milisecondsElapsed += osTickCounterRead(&timer);
		
		// Lyrics
		if ((73640<(int)milisecondsElapsed && (int)milisecondsElapsed<73660) && lyricCount==0){ //73650 ms
			printf("Check it out, I'm in the house like carpet\n\n");
			lyricCount++;
		}
		if ((76490<(int)milisecondsElapsed && (int)milisecondsElapsed<76510) && lyricCount==1){ //76500 ms
			printf("And it there's too many heads on my blunt,\nI won't spark it\n\n");
			lyricCount++;
		}
		
		
		if ((79290<(int)milisecondsElapsed && (int)milisecondsElapsed<79310) && lyricCount==2){ //79300 ms
			printf("I'll put in my pocket and save it like rocket fuel\n");
			lyricCount++;
		}
		if ((82990<(int)milisecondsElapsed && (int)milisecondsElapsed<83010) && lyricCount==3){ //83000 ms
			printf("'Til everybody's gone and it's cool\n\n");
			lyricCount++;
		}
		if ((84790<(int)milisecondsElapsed && (int)milisecondsElapsed<84810) && lyricCount==4){ //84800 ms
			printf("Then I spark it up with my brother\n\n");
			lyricCount++;
		}
		
		
		if ((87490<(int)milisecondsElapsed && (int)milisecondsElapsed<87510) && lyricCount==5){ //87500 ms
			printf("His momma named him Moe, but I call him \n\"Moe-lover\"\n\n");
			lyricCount++;
		}
		if ((89990<(int)milisecondsElapsed && (int)milisecondsElapsed<90010) && lyricCount==6){ //90000 ms
			printf("And he's more than a cover, he's a quilt\n\n");
			lyricCount++;
		}
		
		
		if ((92990<(int)milisecondsElapsed && (int)milisecondsElapsed<93010) && lyricCount==7){ //93000 ms
			printf("We're putting shit together like that house that \nJon built\n\n");
			lyricCount++;
		}
		if ((95490<(int)milisecondsElapsed && (int)milisecondsElapsed<95510) && lyricCount==8){ //95500 ms
			printf("On the hill\n\n");
			lyricCount++;
		}
		if ((96490<(int)milisecondsElapsed && (int)milisecondsElapsed<96510) && lyricCount==9){ //96500 ms
			printf("'Cause this shit's gonna feel like Velvet Turtle\n\n");
			lyricCount++;
		}
		
		
		if ((98990<(int)milisecondsElapsed && (int)milisecondsElapsed<99010) && lyricCount==10){ //99000 ms
			printf("My style fits tighter than a girdle\n\n");
			lyricCount++;
		}
		if ((100790<(int)milisecondsElapsed && (int)milisecondsElapsed<100810) && lyricCount==11){ //100800 ms
			printf("If ya hate it, than you can just leave it\n\n");
			lyricCount++;
		}
		
		if ((102990<(int)milisecondsElapsed && (int)milisecondsElapsed<103010) && lyricCount==12){ //103000 ms
			printf("Like beaver\n\n");
			lyricCount++;
		}
		if ((103490<(int)milisecondsElapsed && (int)milisecondsElapsed<103510) && lyricCount==13){ //103500 ms
			printf("But in a day or two, I'll make you a true believerin me\n\n");
			lyricCount++;
		}
		
		
		if ((106790<(int)milisecondsElapsed && (int)milisecondsElapsed<106810) && lyricCount==14){ //106800 ms
			printf("'Cause like the alphabet, you'll \"C\"\n\n");
			lyricCount++;
		}
		if ((108990<(int)milisecondsElapsed && (int)milisecondsElapsed<109010) && lyricCount==15){ //109000 ms
			printf("That 'ism kicks a rhyme, not your everyday \nsoliloquy\n\n");
			lyricCount++;
		}
		if ((111990<(int)milisecondsElapsed && (int)milisecondsElapsed<112010) && lyricCount==16){ //112000 ms
			printf("Like Chef Boyardee\n\n");
			lyricCount++;
		}
		if ((112990<(int)milisecondsElapsed && (int)milisecondsElapsed<113010) && lyricCount==17){ //113000 ms
			printf("My rhyme is truly cookin'\n\n");
			lyricCount++;
		}
		
		
		if ((114490<(int)milisecondsElapsed && (int)milisecondsElapsed<114510) && lyricCount==18){ //114500 ms
			printf("Peace to Matty Rich 'cause he's Straight Out of \nBrooklyn, New York\n\n");
			lyricCount++;
		}
		if ((117490<(int)milisecondsElapsed && (int)milisecondsElapsed<117510) && lyricCount==19){ //117500 ms
			printf("I don't eat pork or swine when I dine\n\n");
			lyricCount++;
		}
		if ((119985<(int)milisecondsElapsed && (int)milisecondsElapsed<120015) && lyricCount==20){ //120000 ms
			printf("I drink a cup of Kool-Aid\n\n");
			lyricCount++;
		} // wider timeframe (+-15ms isntead of +-10ms) due to inconsistant triggering during testing on real hardware
		if ((120990<(int)milisecondsElapsed && (int)milisecondsElapsed<121010) && lyricCount==21){ //121000 ms
			printf("Not a big glass of wine\n\n");
			lyricCount++;
		}
		
		
		if ((122490<(int)milisecondsElapsed && (int)milisecondsElapsed<122510) && lyricCount==22){ //122500 ms
			printf("Or a Henn, Hein\n\n");
			lyricCount++;
		}
		if ((123490<(int)milisecondsElapsed && (int)milisecondsElapsed<123510) && lyricCount==23){ //123500 ms
			printf("If you have time\n\n");
			lyricCount++;
		}
		if ((124490<(int)milisecondsElapsed && (int)milisecondsElapsed<124510) && lyricCount==24){ //124500 ms
			printf("I'll drop rhyme again\n\n");
			lyricCount++;
		}
		
		
		
        u32 kDown = hidKeysDown();
        if(kDown & KEY_START) {
            printf("\nQuitting...\n");
            break;
        }
    }

    // Signal audio thread to quit
    s_quit = true;
    LightEvent_Signal(&s_event);

    // Free the audio thread
    threadJoin(threadId, UINT64_MAX);
    threadFree(threadId);

    // Cleanup audio things and de-init platform features
    audioExit();
    ndspExit();
    ov_clear(&vorbisFile);

    romfsExit();
    gfxExit();

    return EXIT_SUCCESS;
}
