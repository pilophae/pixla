#include <SDL2/SDL.h>
#include <stdio.h>

#include "synth.h"
#include "frequency_table.h"

typedef enum {
    ATTACK,
    DECAY,
    SUSTAIN,
    RELEASE,
    OFF
} Adsr;


typedef struct _Channel Channel;

/*
 * Function to create a byte in the waveform
 */
typedef Sint8 (*GenerateWaveformFunc)(Uint8 offset);

/*
 * Function to generate a sample for the current time period and channel
 */
typedef Sint8 (*GetSampleFunc)(Channel *channel);


/*
 * Definition of an oscillator channel
 */
typedef struct _Channel {
    Uint16 voiceFreq;
    Uint16 wavePos;
    Adsr adsr;
    Sint16 amplitude;
    Sint8 *wave;
    GetSampleFunc sampleFunc;
    Sint8 param1;
    Sint8 pwm;
    Uint16 dutyCycle;
    Uint16 adsrTimer;
    Sint8 attack;
    Sint8 decay;
    Sint8 sustain;
    Sint8 release;
} Channel;

/*
 * Definition of the synth
 */
typedef struct _Synth {
    Uint16 sampleFreq;
    Sint8 lowpassSaw[256];
    Sint8 lowpassPulse[256];
    Uint8 attackTable[512];
    Uint8 decayReleaseTable[512];
    SDL_AudioDeviceID audio;
    Channel* channelData;
    Uint16 additiveFilter;
    Uint8 channels;
} Synth;

#define SAMPLE_RATE 48000
#define ADSR_PWM_PRESCALER 16
#define ADSR_MAX_TIME_IN_SECS 5

//Uint16 adsrTimescaler = (255 * 127 * ADSR_PWM_PRESCALER)/(ADSR_MAX_TIME_IN_SECS * SAMPLE_RATE) = 2.67
/** Scaled up two times */
#define ADSR_TIMESCALER_2 (511 * 127 * ADSR_PWM_PRESCALER)/(ADSR_MAX_TIME_IN_SECS * SAMPLE_RATE)


void _synth_updateAdsr(Synth *synth, Channel *ch) {

    switch(ch->adsr) {
    case ATTACK:
        if (ch->attack == 0) {
            ch->amplitude = 32767;
            ch->adsr = DECAY;
        } else {
            Uint16 tableIndex = ADSR_TIMESCALER_2 * ch->adsrTimer / ch->attack;
            if (tableIndex < 511) { // 511 = scaled up twice
                ch->amplitude = synth->attackTable[tableIndex] << 7;
                ch->adsrTimer++;
            } else {
                ch->amplitude = 32767;
                ch->adsr = DECAY;
                ch->adsrTimer = 0;
            }
        }
        break;
    case DECAY:
        if (ch->decay == 0) {
            ch->amplitude = ch->sustain << 8;
            ch->adsr = SUSTAIN;
        } else {
            Uint16 tableIndex = ADSR_TIMESCALER_2 * ch->adsrTimer / ch->decay;
            if (tableIndex < 511) { // 511 = scaled up twice
                ch->amplitude = (ch->sustain << 8) + ((127-ch->sustain) * synth->decayReleaseTable[tableIndex]);
                ch->adsrTimer++;
            } else {
                ch->amplitude = ch->sustain << 8;
                ch->adsr = SUSTAIN;
            }
        }
        break;
    case RELEASE:
        if (ch->release == 0) {
            ch->amplitude = 0;
            ch->adsr = OFF;
        } else if (ch->adsrTimer < 0xffff) {
            // Normalized time in seconds:
            // t in s = adsrPos * ADSR_PWM_PRESCALER / SAMPLE_RATE, 16/48000 ger t = 3000  = 1 s
            // t in micros = 16000/48 = 300

            // max_length_in_s =
            // index: i = f(t)/release
            // index: 255 = f(4)/127
            // 255 = C * 4 / 127
            // C = 255*127/4
            // C= 8096,   f(t) = 8096 * t

            // release = 127, t = 0:  8096 * 0 / 127 = 0
            // release = 127, t = 2:  8096 * 2 / 127 = 127
            // release = 63, t = 2: 8096 * 2 / 63 = 257
            // release = 1, t = 2: 8096 * 2 / 1 = 16384

            // 255= C * t / r =>  t = 255 * r / 8096
            //
            // r = 127 => t = 255 * 127 / 8096 = 4 s
            // r = 63 => t = 255 * 63 / 8096 = 1.98s
            // r = 1 => t = 255 / 8096 =

            // i = 8096 * t_s / 127
            // i = (8096 * t_micros / 127) / 1000000
            // i = 80

            // i = 8096 * (pos * prescaler / samplerate) * (1/r)
            //
            //  i

            Uint16 tableIndex = ADSR_TIMESCALER_2 * ch->adsrTimer / ch->release;
            if (tableIndex < 511) { // 511 = scaled up twice
                ch->amplitude = ch->sustain * synth->decayReleaseTable[tableIndex];
                ch->adsrTimer++;
            } else {
                ch->amplitude = 0;
                ch->adsr = OFF;
            }
        } else {
            ch->amplitude = 0;
            ch->adsr = OFF;
        }
        break;
    case OFF:
    case SUSTAIN:
        break;
    }
}

Sint8 _synth_getSampleFromArray(Channel *ch) {
    Sint8 sample = ch->wave[ch->wavePos >> 8];
    return sample;
}

Sint8 _synth_getPulseAtPos(Uint16 dutyCycle, Uint16 wavePos) {
    return wavePos > dutyCycle ? 127 : -128;
}

Sint8 _synth_getPulse(Channel *ch) {
    Uint16 dutyCycle = ch->dutyCycle;
    Uint16 wavePos = ch->wavePos;
    return _synth_getPulseAtPos(dutyCycle, wavePos);
}

Sint8 _synth_getNoise(Channel *ch) {
    return rand() >> 24;
}

void _synth_processBuffer(void* userdata, Uint8* stream, int len) {
    Synth *synth = (Synth*)userdata;
    Sint8 *buffer = (Sint8*)stream;

    synth->additiveFilter++;

    Sint8 scaler = 96/synth->channels;

    for (int i = 0; i < len; i++) {
        Sint16 output = 0;

        for (int j = 0; j < synth->channels; j++) {
            Channel *ch = &synth->channelData[j];

            if (0 == i % ADSR_PWM_PRESCALER) {
                _synth_updateAdsr(synth, ch);
                if (ch->pwm > 0) {
                    ch->dutyCycle+=ch->pwm;
                }
            }


            if (ch->adsr != OFF) {
                Sint16 sample = ch->sampleFunc(ch) * ch->amplitude/32768;
                output += sample;
            }

            ch->wavePos += (65536 * ch->voiceFreq) / synth->sampleFreq;
        }
        buffer[i] = output * scaler / 128;

    }
}

Sint8 getSquareAmplitude(Uint8 offset) {
    return (offset > 50) ? 127 : -128;
}

Sint8 getSawAmplitude(Uint8 offset) {
    return offset-128;
}

void createFilteredBuffer(GenerateWaveformFunc sampleFunc, Sint8* output, int filter) {
    for (int i = 0; i < 256; i++) {
        Sint16 value = 0;
        if (filter == 0) {
            value = sampleFunc(i);
        } else {
            for (int j = 0; j < filter; j++) {
                value += sampleFunc((i+j)%256);
            }
            value /= filter;
        }
        output[i] = value;
    }
}

void _synth_initAudioTables(Synth *synth) {
    createFilteredBuffer(getSquareAmplitude, synth->lowpassPulse, 16);
    createFilteredBuffer(getSawAmplitude, synth->lowpassSaw, 4);


    for (int i = 0; i < 511; i++) {
        synth->attackTable[i] = 11.2*sqrt(i);
    }
    for (int i = 0; i < 511; i++) {
        synth->decayReleaseTable[i] = 13950/(i+50)-24;
    }
    for (int i = 0; i < 511; i++) {
        printf("%d = %d\n", i, synth->attackTable[i]);
    }
}

void _synth_initChannels(Synth *synth) {
    for (int i = 0; i < synth->channels; i++) {
        synth->channelData[i].amplitude = 0;
        synth->channelData[i].adsr = OFF;
        synth->channelData[i].wave = synth->lowpassPulse;
        synth->channelData[i].sampleFunc = _synth_getSampleFromArray;
    }
}

Synth* synth_init(Uint8 channels) {
    if (channels < 1) {
        fprintf(stderr, "Cannot set 0 channels\n");
        return NULL;
    }
    Synth *synth = calloc(1, sizeof(Synth));
    synth->sampleFreq = SAMPLE_RATE;
    synth->channels = channels;
    synth->channelData = calloc(channels, sizeof(Channel));

    SDL_AudioSpec want;
    SDL_AudioSpec have;

    srand(time(NULL));

    SDL_memset(&want, 0, sizeof(want));
    want.freq = synth->sampleFreq; // Playback frequency on Sound card. Each sample takes worth 1/24000 second
    want.format = AUDIO_S8; // 8-bit unsigned samples
    want.channels = 1; // Only play mono for simplicity = 1 byte = 1 sample
    want.samples = 256; // Buffer size.
    want.callback = _synth_processBuffer; // Called whenever the sound card needs more data
    want.userdata = synth;

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    synth->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (synth->audio == 0) {
        fprintf(stderr, "Failed to open audio due to %s\n", SDL_GetError());
        synth_close(synth);
        return NULL;
    }
    if (have.format != want.format) { /* we let this one thing change. */
        SDL_Log("We didn't get our audio format.");
    }
    _synth_initAudioTables(synth);
    _synth_initChannels(synth);

    SDL_PauseAudioDevice(synth->audio, 0); /* start audio playing. */

    return synth;
}

void synth_close(Synth *synth) {
    if (NULL != synth) {
        if (synth->audio != 0) {
            SDL_CloseAudioDevice(synth->audio);
        }
        if (NULL != synth->channelData) {
            free(synth->channelData);
            synth->channelData = NULL;
        }
        free(synth);
        synth = NULL;
    }
}

void synth_setPwm(Synth* synth, Uint8 channel, Sint8 dutyCycle, Sint8 pwm) {
    if (synth == NULL || channel >= synth->channels) {
        return;
    }
    Channel *ch = &synth->channelData[channel];
    if (dutyCycle >= 0) {
        ch->dutyCycle = dutyCycle << 9;
    }
    ch->pwm = pwm;
}

void synth_setChannel(Synth *synth, Uint8 channel, Sint8 attack, Sint8 decay, Sint8 sustain, Sint8 release, Waveform waveform) {
    if (synth == NULL || channel >= synth->channels) {
        return;
    }

    Channel *ch = &synth->channelData[channel];
    ch->attack = attack;
    ch->decay = decay;
    ch->sustain = sustain;
    ch->release = release;
    switch (waveform) {
    case LOWPASS_SAW:
        ch->wave = synth->lowpassSaw;
        ch->sampleFunc =  _synth_getSampleFromArray;
        break;
    case LOWPASS_PULSE:
        ch->wave = synth->lowpassPulse;
        ch->sampleFunc =  _synth_getSampleFromArray;
        break;
    case PWM:
        ch->dutyCycle = 64 << 9;
        ch->pwm = 2;
        ch->sampleFunc = _synth_getPulse;
        break;
    case NOISE:
        ch->sampleFunc = _synth_getNoise;
        break;

    }
}

void synth_notePitch(Synth *synth, Uint8 channel, Sint8 note) {
    synth->channelData[channel].voiceFreq = frequencyTable[(note) % (sizeof(frequencyTable)/sizeof(Uint16))];
}

void synth_noteTrigger(Synth *synth, Uint8 channel, Sint8 note) {
    synth->channelData[channel].adsr = ATTACK;
    synth->channelData[channel].amplitude = 0;
    synth->channelData[channel].adsrTimer = 0;
    synth_notePitch(synth, channel, note);
}

void synth_noteRelease(Synth *synth, Uint8 channel) {
    if (synth == NULL || channel >= synth->channels || synth->channelData[channel].adsr == OFF) {
        return;
    }
    synth->channelData[channel].adsr = RELEASE;
    synth->channelData[channel].adsrTimer = 0;
}

void synth_noteOff(Synth *synth, Uint8 channel) {
    if (synth == NULL || channel >= synth->channels || synth->channelData[channel].adsr == OFF) {
        return;
    }
    synth->channelData[channel].adsr = OFF;
    synth->channelData[channel].amplitude = 0;
    synth->channelData[channel].adsrTimer = 0;
}



Uint8 streamDebug[256];

void _synth_printChannel(Channel *channel) {
    char *adsrText;
    switch (channel->adsr) {
    case ATTACK:
        adsrText = "Attack";
        break;
    case DECAY:
        adsrText = "Decay";
        break;
    case SUSTAIN:
        adsrText = "Sustain";
        break;
    case RELEASE:
        adsrText = "Release";
        break;
    case OFF:
        adsrText = "Off";
        break;
    }
    printf("? Amplitude: %d, ADSR: %s, ADSR Pos: %d\n", channel->amplitude, adsrText, channel->amplitude);
}

Uint32 testSamples=0;
int testNumberOfChannels=1;

void _synth_testRunBuffer(Synth *testSynth) {
    int bufSize = 64;
    Uint8 buf[bufSize];
    _synth_processBuffer(testSynth, buf, bufSize);

    Sint8 *sbuf = (Sint8*)buf;

    Sint8 high = -128;
    Sint8 low = 127;

    for (int i = 0; i < bufSize; i++) {
        if (sbuf[i] < low) {
            low = sbuf[i];
        }
        if (sbuf[i] > high) {
            high = sbuf[i];
        }
    }

    double t = testSamples / (double)SAMPLE_RATE;
    printf("%.2fs ", t);
    testSamples += bufSize;

    for (int i = -64; i < 64; i++) {
        if (i < (low/2) || i > (high/2)) {
            if (i == 0) {
                printf("|");
            } else {
                printf(".");
            }
        } else {
            printf("X");
        }
    }
    printf("[% 3d..% 3d]\n", low,high);
}

void _synth_testInitChannel(Synth *testSynth,int channel,  Sint8 attack, Sint8 decay, Sint8 sustain, Sint8 release) {
    printf("+ Ch: %d A: %d D: %d S: %d R: %d\n", channel, attack, decay, sustain, release);
    synth_setChannel(testSynth, channel, attack, decay, sustain, release, PWM);
}

void _synth_testNoteOn(Synth *testSynth) {
    printf("+ NOTE TRIGGER (24)\n");
    for (int i = 0; i < testNumberOfChannels; i++) {
        synth_noteTrigger(testSynth, i, 24);
    }
}

void _synth_testNoteOff(Synth *testSynth) {
    printf("+ NOTE OFF \n");
    for (int i = 0; i < testNumberOfChannels; i++) {
        synth_noteOff(testSynth, i);
    }
}


void _synth_runTests(Synth *testSynth) {
    printf("\n");
    for (int i = 0; i < testNumberOfChannels; i++) {
        _synth_testInitChannel(testSynth, i, 10, 20, 100, 120);
        _synth_printChannel(&testSynth->channelData[i]);
    }
    _synth_testRunBuffer(testSynth);
    _synth_testNoteOn(testSynth);
    for (int i = 0; i < testNumberOfChannels; i++) {
        _synth_printChannel(&testSynth->channelData[i]);
    }
    testSamples = 0;
    while (testSamples < SAMPLE_RATE/2) {
        _synth_testRunBuffer(testSynth);
    }
    _synth_testNoteOff(testSynth);
    testSamples = 0;
    while (testSamples < SAMPLE_RATE) {
        _synth_testRunBuffer(testSynth);
    }

}

void synth_test() {
    Synth *testSynth = synth_init(testNumberOfChannels);
    if (testSynth == NULL) {
        fprintf(stderr, "Synth test failed to start\n");
        return;
    }
    printf("======================TEST OF %d CHANNELS!========================\n", testNumberOfChannels);
    SDL_PauseAudioDevice(testSynth->audio, 1); /* sop audio playing. */
    _synth_runTests(testSynth);
    printf("\n");
    SDL_PauseAudioDevice(testSynth->audio, 0); /* start audio playing. */
    synth_close(testSynth);
}
