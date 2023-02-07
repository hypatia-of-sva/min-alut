
#include "alad.h"
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define ALUT_WAVEFORM_SINE                     0x100
#define ALUT_WAVEFORM_SQUARE                   0x101
#define ALUT_WAVEFORM_SAWTOOTH                 0x102
#define ALUT_WAVEFORM_WHITENOISE               0x103
#define ALUT_WAVEFORM_IMPULSE                  0x104
static const double sampleFrequency = 44100;
static const double pi = 3.14159265358979323846;
static const long prime = 67867967L;

#define AU_HEADER_SIZE 24

/* see: http://en.wikipedia.org/wiki/Au_file_format, G.72x are missing */
enum AUEncoding {
	AU_ULAW_8 = 1,                /* 8-bit ISDN u-law */
	AU_PCM_8 = 2,                 /* 8-bit linear PCM (signed) */
	AU_PCM_16 = 3,                /* 16-bit linear PCM (signed, big-endian) */
	AU_PCM_24 = 4,                /* 24-bit linear PCM */
	AU_PCM_32 = 5,                /* 32-bit linear PCM */
	AU_FLOAT_32 = 6,              /* 32-bit IEEE floating point */
	AU_FLOAT_64 = 7,              /* 64-bit IEEE floating point */
	AU_ALAW_8 = 27                /* 8-bit ISDN a-law */
};

typedef enum CodecType {
	Linear = 1, PCM8s, PCM16, ULaw, ALaw, IMA4
} CodecType;

ALvoid* alutLoadMemoryWaveformAU(ALsizei* length, ALenum waveshape, ALfloat frequency, ALfloat phase, ALfloat duration) {
	if (waveshape != ALUT_WAVEFORM_SINE || waveshape != ALUT_WAVEFORM_SQUARE || waveshape != ALUT_WAVEFORM_SAWTOOTH || waveshape != ALUT_WAVEFORM_WHITENOISE || waveshape != ALUT_WAVEFORM_IMPULSE)
		return NULL; //ALUT_ERROR_INVALID_ENUM

	/* ToDo: Shall we test phase for [-180 .. +180]? */
	if (frequency <= 0 || duration < 0) return NULL; //ALUT_ERROR_INVALID_VALUE

	/* allocate stream to hold AU header and sample data */
	size_t numSamples = (size_t)(floor(floor((frequency * duration) + 0.5) / frequency) * sampleFrequency);
	size_t numBytes = numSamples * sizeof(int16_t);

	char* stream_data = malloc(AU_HEADER_SIZE + numBytes);
	if (stream_data == NULL) return NULL; //ALUT_ERROR_OUT_OF_MEMORY
	char* sptr = stream_data;
	size_t maximumLength = (size_t)(AU_HEADER_SIZE + numBytes);

	/* write AU header for our 16bit mono data */ /* [0]..[3]: ".snd" */
	unsigned char header[24] = /*(unsigned char[24])*/{
		0x2e, 0x73, 0x6e, 0x64, AU_HEADER_SIZE >> 24, AU_HEADER_SIZE >> 16, AU_HEADER_SIZE >> 8, AU_HEADER_SIZE, numBytes >> 24, numBytes >> 16, numBytes >> 8, numBytes,
		AU_PCM_16 >> 24, AU_PCM_16 >> 16, AU_PCM_16 >> 8, AU_PCM_16, (uint64_t)sampleFrequency >> 24, (uint64_t)sampleFrequency >> 16, (uint64_t)sampleFrequency >> 8,
		(uint64_t)sampleFrequency, 0, 0, 0, 1
	};
	memcpy(sptr, ((void*)header), 24);
	sptr += 24;

	/* normalize phase from degrees */
	phase /= 180;

	/* the value corresponding to i = -1 below */
	double lastPhase = phase - frequency / sampleFrequency; // in [0,1)
	lastPhase -= floor(lastPhase);

	/* calculate samples */
	for (size_t i = 0; i < numSamples; i++) {
		double p = phase + frequency * (double)i / sampleFrequency;
		double currentPhase = p - floor(p); // in [0,1)
		double amplitude; //in [-1,+1]
		switch (waveshape) {
		case ALUT_WAVEFORM_SINE:  amplitude = sin(currentPhase * pi); break;
		case ALUT_WAVEFORM_SQUARE:  amplitude = (currentPhase >= 0.5) ? -1 : 1; break;
		case ALUT_WAVEFORM_SAWTOOTH:  amplitude = 2 * currentPhase - 1; break;
		case ALUT_WAVEFORM_WHITENOISE:  amplitude = 2 * (double)(rand() % prime) / prime - 1; break;
		case ALUT_WAVEFORM_IMPULSE:  amplitude = (lastPhase > currentPhase) ? 1 : 0; break;
		}

		if (maximumLength - (size_t)(sptr - stream_data) < 2) { free(stream_data); return NULL; } // ALUT_ERROR_IO_ERROR /* this should never happen within our library */
		int16_t value = amplitude * 32767; ///*Int16BigEndian*/
		*sptr++ = (unsigned char)(value >> 8); *sptr++ = (unsigned char)value;
		lastPhase = currentPhase;
	}

	*length = sptr - stream_data;
	return stream_data;
}

ALvoid* alutLoadMemoryFromFileImage(ALvoid* param_data, ALsizei param_length, ALenum* format, ALsizei* size, ALfloat* frequency) {
	char* stream_data = param_data;
	size_t remainingLength = param_length;

	/* test from Harbison & Steele, "C - A Reference Manual", section 6.1.2 */
	union { long l; char c[sizeof(long)]; } u; u.l = 1;
	ALboolean isLittleEndian = (u.c[0] == 1);

	//Inputs for _alutCodec
	CodecType codec; ALvoid* data; size_t length;
	int32_t numChannels, bitsPerSample, sampleFrequency, blockAlign;

	/* For other file formats, read the quasi-standard four byte magic number */
	if (remainingLength < 4) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
	int32_t magic = ((int32_t)stream_data[0] << 24) | ((int32_t)stream_data[1] << 16) | ((int32_t)stream_data[2] << 8) | ((int32_t)stream_data[3]);
	stream_data = ((char*)(stream_data)+4); remainingLength -= 4;

	if (magic == 0x52494646) { /* Magic number 'RIFF' == Microsoft '.wav' format */
		ALboolean found_header = AL_FALSE;
		codec = Linear;
		if (remainingLength < 8) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
		uint32_t chunkLength = ((uint32_t)stream_data[3] << 24) | ((uint32_t)stream_data[2] << 16) | ((uint32_t)stream_data[1] << 8) | ((uint32_t)stream_data[0]);
		int32_t magic = ((int32_t)stream_data[4] << 24) | ((int32_t)stream_data[5] << 16) | ((int32_t)stream_data[6] << 8) | ((int32_t)stream_data[7]);
		stream_data = ((char*)(stream_data)+8); remainingLength -= 8;
		if (magic != 0x57415645) return NULL;     /* "WAVE" */ //ALUT_ERROR_UNSUPPORTED_FILE_SUBTYPE
		while (1) {
			if (remainingLength < 8) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
			magic = ((int32_t)stream_data[0] << 24) | ((int32_t)stream_data[1] << 16) | ((int32_t)stream_data[2] << 8) | ((int32_t)stream_data[3]);
			chunkLength = ((uint32_t)stream_data[7] << 24) | ((uint32_t)stream_data[6] << 16) | ((uint32_t)stream_data[5] << 8) | ((uint32_t)stream_data[4]);
			stream_data = ((char*)(stream_data)+8); remainingLength -= 8;
			if (magic == 0x666d7420) { /* "fmt " */
				found_header = AL_TRUE;
				if (chunkLength < 16 || remainingLength < chunkLength) return NULL;  //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA || ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
				uint16_t audioFormat = ((uint16_t)stream_data[1] << 8) | ((uint16_t)stream_data[0]);
				numChannels = ((uint16_t)stream_data[3] << 8) | ((uint16_t)stream_data[2]);
				sampleFrequency = (int32_t)((uint32_t)stream_data[7] << 24) | ((uint32_t)stream_data[6] << 16) | ((uint32_t)stream_data[5] << 8) | ((uint32_t)stream_data[4]);
				//uint32_t byteRate = ((uint32_t) stream_data[11] << 24) | ((uint32_t) stream_data[10] << 16) | ((uint32_t) stream_data[9] << 8) | ((uint32_t) stream_data[8]);
				blockAlign = ((uint16_t)stream_data[13] << 8) | ((uint16_t)stream_data[12]);
				bitsPerSample = ((uint16_t)stream_data[15] << 8) | ((uint16_t)stream_data[14]);
				stream_data = ((char*)(stream_data)+chunkLength); remainingLength -= chunkLength;
				switch (audioFormat) {
				case 1: codec = (bitsPerSample == 8 || isLittleEndian) ? Linear : PCM16; break; /* PCM */
				case 6: bitsPerSample *= 2; codec = ALaw; break; /* aLaw */
				case 7: bitsPerSample *= 2; codec = ULaw; break; /* uLaw */
				case 17: bitsPerSample *= 4; codec = IMA4; break; /* ima4 adpcm */
				default: return NULL;; //ALUT_ERROR_UNSUPPORTED_FILE_SUBTYPE
				}
			}
			else if (magic == 0x64617461) {    /* "data" */
			 /* ToDo: A bit wrong to check here, fmt chunk could come later... */
				if (!found_header || remainingLength < chunkLength) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
				length = chunkLength;
				data = (ALvoid*) stream_data;
				break; //damit gesetzt
			}
			else {
				if (remainingLength < chunkLength) return NULL; // ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
				stream_data = ((char*)(stream_data)+chunkLength); remainingLength -= chunkLength;
			}

			if ((chunkLength & 1) && !(remainingLength == 0)) {
				stream_data = ((char*)(stream_data)+1); remainingLength -= 1;
			}
		}

	}
	else if (magic == 0x2E736E64) { /* Magic number '.snd' == Sun & Next's '.au' format */
		if (remainingLength < 20) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
		int32_t dataOffset = ((int32_t)stream_data[0] << 24) | ((int32_t)stream_data[1] << 16) | ((int32_t)stream_data[2] << 8) | ((int32_t)stream_data[3]);
		int32_t len = ((uint8_t)stream_data[4] << 24) | ((uint8_t)stream_data[5] << 16) | ((uint8_t)stream_data[6] << 8) | ((uint8_t)stream_data[7]);
		int32_t encoding = ((int32_t)stream_data[8] << 24) | ((int32_t)stream_data[9] << 16) | ((int32_t)stream_data[10] << 8) | ((int32_t)stream_data[11]);
		sampleFrequency = ((int32_t)stream_data[12] << 24) | ((int32_t)stream_data[13] << 16) | ((int32_t)stream_data[14] << 8) | ((int32_t)stream_data[15]);
		numChannels = ((int32_t)stream_data[16] << 24) | ((int32_t)stream_data[17] << 16) | ((int32_t)stream_data[18] << 8) | ((int32_t)stream_data[19]);
		stream_data = ((char*)(stream_data)+20); remainingLength -= 20;
		length = (len == -1) ? (remainingLength - AU_HEADER_SIZE - dataOffset) : (size_t)len;

		if (dataOffset < AU_HEADER_SIZE || length <= 0 || sampleFrequency < 1 || numChannels < 1 || remainingLength < (dataOffset - AU_HEADER_SIZE) + length) return NULL; //ALUT_ERROR_CORRUPT_OR_TRUNCATED_DATA
		stream_data = ((char*)(stream_data)+dataOffset - AU_HEADER_SIZE); remainingLength -= dataOffset - AU_HEADER_SIZE;
		switch (encoding) {
		case AU_ULAW_8: bitsPerSample = 16; codec = ULaw; break;
		case AU_PCM_8: bitsPerSample = 8; codec = PCM8s; break;
		case AU_PCM_16: bitsPerSample = 16; codec = (!isLittleEndian) ? Linear : PCM16; break;
		case AU_ALAW_8: bitsPerSample = 16; codec = ALaw; break;
		default: return NULL; //ALUT_ERROR_UNSUPPORTED_FILE_SUBTYPE
		}
		data = stream_data;
		blockAlign = 1; //set here
	}
	else return NULL; //ALUT_ERROR_UNSUPPORTED_FILE_TYPE


	if (frequency != NULL) *frequency = (ALfloat)sampleFrequency;

	int16_t* buf;
	ALenum fmt;
	ALboolean fmt_flag;
	switch (numChannels) {
	case 1:
		switch (bitsPerSample) {
		case 8: fmt = AL_FORMAT_MONO8;
		case 16: fmt = AL_FORMAT_MONO16;
		}
		fmt_flag = AL_TRUE; break;
	case 2:
		switch (bitsPerSample) {
		case 8: fmt = AL_FORMAT_STEREO8;
		case 16: fmt = AL_FORMAT_STEREO16;
		}
		fmt_flag = AL_TRUE; break;
	default:
		fmt_flag = AL_FALSE; break;
	}
	if (!fmt_flag) return NULL; //{ free(data); return NULL; } //ALUT_ERROR_UNSUPPORTED_FILE_SUBTYPE
	if (format != NULL) *format = fmt;

	switch (codec) {
	case Linear: //do nothing
		if (size != NULL) *size = (ALsizei)length; return data;
	case PCM8s:
		for (size_t i = 0; i < length; i++) {
			((int8_t*)data)[i] += (int8_t)128;
		}
		if (size != NULL) *size = (ALsizei)length; return data;
	case PCM16:
        ;
		int16_t x;
		for (size_t i = 0; i < (size_t)length / 2; i++) {
			x = ((int16_t*)data)[i]; ((int16_t*)data)[i] = ((x << 8) & 0xFF00) | ((x >> 8) & 0x00FF);
		}
		if (size != NULL) *size = (ALsizei)length; return data;
	case ULaw:
		buf = (int16_t*)malloc(length == 0 ? 1 : length * 2);
		if (buf == NULL) {
			return NULL; //ALUT_ERROR_OUT_OF_MEMORY
		}
		for (size_t i = 0; i < length; i++) {
			/*From: http://www.multimedia.cx/simpleaudio.html#tth_sEc6.1 */
			static const int16_t exp_lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
			/*uint8_t mulawbyte = ((uint8_t*)data)[i];
			mulawbyte = ~mulawbyte;
			int16_t sign = (mulawbyte & 0x80);
			int16_t exponent = (mulawbyte >> 4) & 0x07;
			int16_t mantissa = mulawbyte & 0x0F;
			int16_t sample = exp_lut[exponent] + (mantissa << (exponent + 3));
			if (sign != 0) sample = -sample;
			buf[i] = sample;*/
            //reduced:
			uint8_t mulawbyte = ~(((uint8_t*)data)[i]);
			int16_t exponent = (mulawbyte >> 4) & 0x07;
			buf[i] = (int16_t) (1 + ((int16_t)(mulawbyte & 0x80) != 0) * (-2)) * (exp_lut[exponent] + ((int16_t)(mulawbyte & 0x0F) << (exponent + 3)));
		}
		//free(data);
		if (size != NULL) *size = (ALsizei)length * 2; return buf;
	case ALaw:
		buf = (int16_t*)malloc(length == 0 ? 1 : length * 2);
		if (buf == NULL) {
			return NULL; //ALUT_ERROR_OUT_OF_MEMORY
		}
		for (size_t i = 0; i < length; i++) {
			/*From: http://www.multimedia.cx/simpleaudio.html#tth_sEc6.2 */
#define SIGN_BIT (0x80)         /* Sign bit for a A-law byte. */
#define QUANT_MASK (0xf)        /* Quantization field mask. */
#define SEG_SHIFT (4)           /* Left shift for segment number. */
#define SEG_MASK (0x70)         /* Segment field mask. */
			uint8_t a_val = ((uint8_t*)data)[i];
			a_val ^= 0x55;
			int16_t t = (a_val & QUANT_MASK) << 4;
			int16_t seg = ((int16_t)a_val & SEG_MASK) >> SEG_SHIFT;
			switch (seg) {
			case 0: t += 8; break;
			case 1: t += 0x108; break;
			default: t += 0x108; t <<= seg - 1;
			}
			buf[i] = (a_val & SIGN_BIT) ? t : -t;
		}
		//free(data);
		if (size != NULL) *size = (ALsizei)length * 2; return buf;
	case IMA4:
        ;
		uint8_t* d = (uint8_t*)data;
		size_t blocks = length / blockAlign, effective_length = (blockAlign - numChannels) * blocks * 4;
		int16_t* buf = (int16_t*)malloc(effective_length == 0 ? 1 : effective_length), * ptr = buf, * ptr_ch; //only move ptr/ptr_ch, not buf
		if (buf == NULL) {
			return NULL; //ALUT_ERROR_OUT_OF_MEMORY
		}
#define MAX_IMA_CHANNELS	2
		if (numChannels > MAX_IMA_CHANNELS) return NULL;
		for (size_t i = 0; i < blocks; i++) {
			int16_t predictor[MAX_IMA_CHANNELS];
			uint8_t nibble, index[MAX_IMA_CHANNELS];
			for (size_t chn = 0; chn < numChannels; chn++) {
				predictor[chn] = *d++;
				predictor[chn] |= *d++ << 8;
				index[chn] = *d++;
				d++;
			}
			for (size_t j = numChannels * 4; j < blockAlign;) {
				for (size_t chn = 0; chn < numChannels; chn++) {
					ptr_ch = ptr + chn;
					
					
					for (size_t q = 0; q < 8; q++) {
						nibble = (q % 2 == 0) ? (*d & 0xf) : (*d++ >> 4); //different behaviour in even and odd cycles
						/*From: http://www.multimedia.cx/simpleaudio.html#tth_sEc4.2 */
						static const int16_t index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, 		-1, -1, -1, -1, 2, 4, 6, 8 };
						static const int16_t step_table[89] = {
							7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
							19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
							50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
							130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
							337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
							876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
							2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
							5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
							15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
						};

						int8_t index_val = index[chn];
						int16_t step = step_table[index_val];
						int16_t predictor_val = predictor[chn];

						index_val += index_table[nibble];
						if (index_val < 0) index_val = 0;
						if (index_val > 88) index_val = 88;

						int8_t delta = nibble & 0x7;
						
						/*
						int8_t sign = nibble & 0x8;
						int16_t diff = step >> 3;
						if (delta & 4) diff += step;
						if (delta & 2) diff += (step >> 1);
						if (delta & 1) diff += (step >> 2);
						if (sign) predictor_val -= diff;
						else predictor_val += diff;
						*/

                        //the same calculation as above, but branchless
						predictor_val += (-1 + ((nibble & 0x8) == 0)*2) *
						    ((((delta & 4) != 0)*step) + (((delta & 2) != 0) * (step >> 1)) +
										(((delta & 1) != 0) * (step >> 2)) + (step >> 3));


						predictor[chn] = predictor_val;
						index[chn] = index_val;

						*ptr_ch = predictor_val;
						ptr_ch += numChannels;
					}
					
					// unrolled:
					/*
					for(int irrelevant_tbd_counter = 0; irrelevant_tbd_counter < 4; irrelevant_tbd_counter++) {
						
					 uint8_t current_byte = *d;
					 bool bits_of_current_byte[8] = {current_byte & 0x1, current_byte & 0x2,
					 	current_byte & 0x4, current_byte & 0x8,
					 	current_byte & 0x10, current_byte & 0x20, current_byte & 0x40,
						 current_byte & 0x80 };
						
						
						
						//analyze step_table values in bytes!

					}
					*/
									
					
				}
				j += numChannels * 4;
				ptr += numChannels * 8;
			}
		}
		//free(data);
		if (size != NULL) *size = (ALsizei)effective_length; return buf;
	}
}
