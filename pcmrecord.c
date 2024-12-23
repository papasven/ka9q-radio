// Read and record PCM/WAV and Ogg Opus audio streams
// Now with --stdout option to send (one) stream to standard output, eventually to replace pcmcat
// Also with --exec option to pipe stream into command, to replace pcmspawn
// Copyright 2021-2024 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <byteswap.h>
#else // bsd
#define bswap_16(value) ((((value) & 0xff) << 8) | ((value) >> 8)) // hopefully gets optimized
#endif
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <poll.h>
#include <sysexits.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <ogg/ogg.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"
#include "radio.h"

// size of stdio buffer for disk I/O. 8K is probably the default, but we have this for possible tuning
#define BUFFERSIZE (8192) // probably the same as default
#define RESEQ 64 // size of resequence queue. Probably excessive; WiFi reordering is rarely more than 4-5 packets

// Simplified .wav file header
// http://soundfile.sapp.org/doc/WaveFormat/
struct wav {
  char ChunkID[4];
  int32_t ChunkSize;
  char Format[4];

  char Subchunk1ID[4];
  int32_t Subchunk1Size;
  int16_t AudioFormat;
  int16_t NumChannels;
  int32_t SampleRate;
  int32_t ByteRate;
  int16_t BlockAlign;
  int16_t BitsPerSample;

  // adding additional chunk fields to the wav file header to support 32 bit FP
  // see https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
  int16_t ExtensionChunkSize;
  int16_t ValidBitsPerSample;
  int32_t ChannelMask;
  char Subformat[16];
  char FactID[4];
  uint32_t FactSize;
  uint32_t SamplesLength;

   // 'auxi' chunk to pass center frequency to SDR Console
   // http://www.moetronix.com/files/spectravue.pdf had some details on this chunk
   // and https://sdrplay.com/resources/IQ/ft4.zip
   // has some .wav files with a center frequency that SDR Console can use
   char AuxID[4];
   int32_t AuxSize;
   int16_t StartYear;
   int16_t StartMon;
   int16_t StartDOW;
   int16_t StartDay;
   int16_t StartHour;
   int16_t StartMinute;
   int16_t StartSecond;
   int16_t StartMillis;
   int16_t StopYear;
   int16_t StopMon;
   int16_t StopDOW;
   int16_t StopDay;
   int16_t StopHour;
   int16_t StopMinute;
   int16_t StopSecond;
   int16_t StopMillis;
   int32_t CenterFrequency;
   char AuxUknown[128];

  char SubChunk2ID[4];
  int32_t Subchunk2Size;
};

// One for each session being recorded
struct session {
  struct session *prev;
  struct session *next;
  struct sockaddr sender;      // Sender's IP address and source port

  char filename[PATH_MAX];
  bool can_seek;               // file is regular; can seek on it

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;

  // information obtained from status stream
  struct channel chan;
  struct frontend frontend;

  double last_frequency;       // Detect changes to trigger Ogg Opus stream restarts
  char last_preset[32];

  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;
  enum encoding encoding;

  // Ogg container state
  ogg_stream_state oggState;   // For ogg Opus
  int64_t granulePosition;
  int packetCount;
  struct reseq {
    struct rtp_header rtp;
    uint8_t *data;
    int size;
    bool inuse;
  } reseq[RESEQ];              // Reseqencing queue


  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate
  int64_t last_active;         // gps time of last activity
  int64_t starting_offset;     // First actual sample in file past wav header (for time alignment)

  bool substantial_file;       // At least one substantial segment has been seen
  int64_t current_segment_samples; // total samples in this segment without skips in timestamp
  int64_t samples_written;
  int64_t total_file_samples;
  int64_t samples_remaining;   // Samples remaining before file is closed; 0 means indefinite
};


static float SubstantialFileTime = 0.2;  // Don't record bursts < 250 ms unless they're between two substantial segments
static double FileLengthLimit = 0; // Length of file in seconds; 0 = unlimited
static const double Tolerance = 1.0; // tolerance for starting time in sec when FileLengthLimit is active
int Verbose;
static char PCM_mcast_address_text[256];
static int64_t Timeout = 20; // 20 seconds max idle time before file close
static char const *Recordings = ".";
static bool Subdirs; // Place recordings in subdirectories by SSID
static char const *Locale;
static uint32_t Ssrc; // SSRC, when manually specified
static bool Catmode = false; // sending one channel to standard output
static bool Flushmode = false; // Flush after each packet when writing to standard output
static const char *Command = NULL;
static bool Jtmode = false;
static bool Raw = false;

const char *App_path;
static int Input_fd,Status_fd;
static struct session *Sessions;
int Mcast_ttl;
struct sockaddr_storage Metadata_dest_socket;

static void closedown(int a);
static void input_loop(void);
static void cleanup(void);
int session_file_init(struct session *sp,struct sockaddr const *sender);
static int close_file(struct session **spp);
static uint8_t *encodeTagString(uint8_t *out,int size,const char *string);
static int start_ogg_opus_stream(struct session *sp);
static int emit_ogg_opus_tags(struct session *sp);
static int end_ogg_opus_stream(struct session *sp);
static int start_wav_stream(struct session *sp);
static int end_wav_stream(struct session *sp);

static struct option Options[] = {
  {"catmode", no_argument, NULL, 'c'}, // Send single stream to stdout
  {"stdout", no_argument, NULL, 'c'},
  {"directory", required_argument, NULL, 'd'},
  {"exec", required_argument, NULL, 'e'},
  {"flush", no_argument, NULL, 'f'},   // Quickly fflush in --stdout mode to minimize delay
  {"jt", no_argument, NULL, 'j'},      // Use K1JT format file names
  {"locale", required_argument, NULL, 'l'},
  {"minfiletime", required_argument, NULL, 'm'},
  {"mintime", required_argument, NULL, 'm'},
  {"raw", no_argument, NULL, 'r' },
  {"subdirectories", no_argument, NULL, 's'},
  {"subdirs", no_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 't'},
  {"verbose", no_argument, NULL, 'v'},
  {"lengthlimit", required_argument, NULL, 'L'},
  {"limit", required_argument, NULL, 'L'},
  {"ssrc", required_argument, NULL, 'S'},
  {"version", no_argument, NULL, 'V'},
  {NULL, no_argument, NULL, 0},
};
static char Optstring[] = "cd:e:fjl:m:rsS:t:vL:V";

int main(int argc,char *argv[]){
  App_path = argv[0];

  // Defaults
  Locale = getenv("LANG");

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'c':
      Catmode = true;
      break;
    case 'e':
      Command = optarg;
      break;
    case 'f':
      Flushmode = true;
      break;
    case 'j':
      Jtmode = true;
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'm':
      SubstantialFileTime = fabsf(strtof(optarg,NULL));
      break;
    case 'r':
      Raw = true;
      break;
    case 'S':
      {
	char *ptr;
	uint32_t x = strtol(optarg,&ptr,0);
	if(ptr != optarg)
	  Ssrc = x;
      }
      break;
    case 's':
      Subdirs = true;
      break;
    case 't':
      {
	char *ptr;
	int64_t x = strtoll(optarg,&ptr,0);
	if(ptr != optarg)
	  Timeout = x;
      }
      break;
    case 'v':
      Verbose++;
      break;
    case 'L':
      FileLengthLimit = fabsf(strtof(optarg,NULL));
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-c|--catmode|--stdout] [-r|--raw] [-e|--exec command] [-f|--flush] [-s] [-d directory] [-l locale] [-L maxtime] [-t timeout] [-j|--jt] [-v] [-m sec] PCM_multicast_address\n",argv[0]);
      exit(EX_USAGE);
      break;
    }
  }
  setlocale(LC_ALL,Locale);
  if(optind >= argc){
    fprintf(stderr,"Specify PCM_mcast_address_text_address\n");
    exit(EX_USAGE);
  }
  strlcpy(PCM_mcast_address_text,argv[optind],sizeof(PCM_mcast_address_text));
  setlocale(LC_ALL,Locale);
  setlinebuf(stderr); // In case we're redirected to a file

  if(Catmode && Command != NULL){
    fprintf(stderr,"--exec supersedes --stdout\n");
    Catmode = false;
  }
  if((Catmode || Command != NULL) && (Subdirs || Jtmode)){
    fprintf(stderr,"--stdout and --exec supersede --subdirs and --jtmode\n");
    Subdirs = false;
    Jtmode = false;
  }

  if(Subdirs && Jtmode){
    fprintf(stderr,"--jtmode supersedes --subdirs\n");
    Subdirs = false;
  }
  // Set up input socket for multicast data stream from front end
  {
    struct sockaddr_storage sock;
    char iface[1024];
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    Input_fd = listen_mcast(&sock,iface);
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    Status_fd = listen_mcast(&sock,iface);
  }
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input, exiting\n");
    exit(EX_IOERR);
  }
  int n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
  signal(SIGPIPE,closedown); // Should catch the --exec or --stdout receiving process terminating
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);

  atexit(cleanup);
  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stderr,"Can't change to directory %s: %s\n",Recordings,strerror(errno));
    exit(EX_CANTCREAT);
  }
  input_loop(); // Doesn't return

  exit(EX_OK);
}

static void closedown(int a){
  if(Verbose)
    fprintf(stderr,"%s: caught signal %d: %s\n",App_path,a,strsignal(a));

  cleanup();
  exit(EX_OK);  // Will call cleanup()
}

// This comes from a conversation with ChatGPT that was somewhat contradictory about the silence frame
static uint8_t OpusSilence[] = {0xf8,0xff,0xfe}; // Silence
//static uint8_t OpusSilence[] = {0xff}; // Comfort noise
//static uint8_t OpusSilence[] = {0xf8}; // Lost packet, packet loss concealment. creates buzz when squelch is closed


static int emit_opus_silence(struct session * const sp,int samples){
  ogg_packet oggPacket;
  oggPacket.b_o_s = 0;
  oggPacket.e_o_s = 0;    // End of stream flag

  while(samples > 0){
    int chunk = min(samples,960); // 20 ms is 960 samples @ 48 kHz
    oggPacket.packetno = sp->packetCount++; // Increment packet number
    sp->granulePosition += chunk; // points to end of this packet
    oggPacket.granulepos = sp->granulePosition; // Granule position
    oggPacket.packet = OpusSilence;
    oggPacket.bytes = sizeof(OpusSilence);
    int ret = ogg_stream_packetin(&sp->oggState, &oggPacket);	  // Add the packet to the Ogg stream
    (void)ret;
    assert(ret == 0);

    sp->rtp_state.timestamp += chunk; // also ready for next
    sp->total_file_samples += chunk;
    sp->current_segment_samples += chunk;
    sp->samples_written += chunk;
    if(FileLengthLimit != 0)
      sp->samples_remaining -= chunk;
    if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
      sp->substantial_file = true;
    samples -= chunk;
  }
  return 0;
}


// if !flush, send whatever's on the queue, up to the first missing segment
// if flush, empty the entire queue, skipping empty entries
static int send_opus_queue(struct session * const sp,bool flush){
  // Anything on the resequencing queue we can now process?
  int count = 0;
  for(int i=0; i < RESEQ; i++,sp->rtp_state.seq++){
    struct reseq *qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse && !flush)
      break; // Stop on first empty entry if we're not resynchronizing

    if(qp->inuse){
      ogg_packet oggPacket;
      oggPacket.b_o_s = 0;
      oggPacket.e_o_s = 0;    // End of stream flag
      int samples = opus_packet_get_nb_samples(qp->data,qp->size,48000); // Number of 48 kHz samples

      int32_t jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
      if(jump > 0){
	// Timestamp jumped since last frame
	// Catch up by emitting silence padding
	if(Verbose > 2 || (Verbose > 1  && flush))
	  fprintf(stderr,"timestamp jump %d samples\n",jump);

	emit_opus_silence(sp,jump);
      }
      // end of timestamp jump catch-up, send actual packets on queue
      oggPacket.packetno = sp->packetCount++; // Increment packet number
      // Adjust the granule position
      sp->granulePosition += samples; // points to end of this packet
      oggPacket.granulepos = sp->granulePosition; // Granule position
      oggPacket.packet = qp->data;
      oggPacket.bytes = qp->size;
      sp->rtp_state.timestamp += samples; // also ready for next
      sp->total_file_samples += samples;
      sp->current_segment_samples += samples;
      sp->samples_written += samples;
      if(FileLengthLimit != 0)
	sp->samples_remaining -= samples;
      if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	sp->substantial_file = true;

      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"writing from rtp sequence %u, timestamp %u: bytes %ld samples %d granule %lld\n",
		sp->rtp_state.seq,sp->rtp_state.timestamp,oggPacket.bytes,samples,
		(long long)oggPacket.granulepos);

      int ret = ogg_stream_packetin(&sp->oggState, &oggPacket);
      (void)ret;
      assert(ret == 0);
    } else
      sp->rtp_state.drops++;

    // Flush the stream to ensure packets are written
    ogg_page oggPage;
    while (ogg_stream_pageout(&sp->oggState, &oggPage)) {
      fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
      fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    }
    FREE(qp->data); // OK if NULL
    qp->size = 0;
    qp->inuse = false;
    count++;
  }
  if(Flushmode){
    ogg_page oggPage;
    while (ogg_stream_flush(&sp->oggState, &oggPage)) {
      fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
      fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    }
  }
  return count;
}
// if !flush, send whatever's on the queue, up to the first missing segment
// if flush, empty the entire queue, skipping empty entries
static int send_wav_queue(struct session * const sp,bool flush){
  // Anything on the resequencing queue we can now process?
  int count = 0;
  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  for(int i=0; i < RESEQ; i++,sp->rtp_state.seq++){
    struct reseq *qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse && !flush)
      break; // Stop on first empty entry if we're not resynchronizing

    int frames = qp->size / framesize;  // One frame per sample time
    if(qp->inuse){
      int jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
      if(jump > 0){
	// Timestamp jumped since last frame
	// Catch up by emitting silence padding
	if(Verbose > 2 || (Verbose > 1  && flush))
	  fprintf(stderr,"timestamp jump %d frames\n",jump);
	if(sp->can_seek)
	  fseeko(sp->fp,framesize * jump,SEEK_CUR);
	else {
	  unsigned char *zeroes = calloc(jump,framesize); // Don't use too much stack space
	  fwrite(zeroes,framesize,jump,sp->fp);
	  FREE(zeroes);
	}
	sp->rtp_state.timestamp += jump; // also ready for next
	sp->total_file_samples += jump;
	sp->current_segment_samples += jump;
	sp->samples_written += jump;
	if(FileLengthLimit != 0)
	  sp->samples_remaining -= jump;
      }
      // end of timestamp jump catch-up, send actual packets on queue
      fwrite(qp->data,framesize,frames,sp->fp);
      sp->rtp_state.timestamp += frames; // also ready for next
      sp->total_file_samples += frames;
      sp->current_segment_samples += frames;
      sp->samples_written += frames;
      if(FileLengthLimit != 0)
	sp->samples_remaining -= frames;
      if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	sp->substantial_file = true;

      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"writing from rtp sequence %u, timestamp %u: bytes %d frames %d\n",
		sp->rtp_state.seq,sp->rtp_state.timestamp,framesize * frames,frames);

      FREE(qp->data); // OK if NULL
      qp->size = 0;
      qp->inuse = false;
      count++;
    }
  }
  return count;
}


// Read both data and status from RTP network socket, assemble blocks of samples
// Doing both in one thread avoids a lot of synchronization problems with the session structure, since both write it
static void input_loop(){
  struct sockaddr sender;
  int64_t last_scan_time = 0;
  while(true){
    // Receive status or data
    struct pollfd pfd[2];
    pfd[0].fd = Input_fd;
    pfd[1].fd = Status_fd;
    pfd[1].events = pfd[0].events = POLLIN;
    pfd[1].revents = pfd[0].revents = 0;

    int const n = poll(pfd,sizeof(pfd)/sizeof(pfd[0]),1000); // Wait 1 sec max so we can scan active session list
    if(n < 0)
      break; // error of some kind - should we exit or retry?
    int64_t current_time = gps_time_ns();
    if(pfd[1].revents & (POLLIN|POLLPRI)){
      // Process status packet
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int length = recvfrom(Status_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(length <= 0){    // ??
	perror("recvfrom");
	goto statdone; // Some sort of error
      }
      if(buffer[0] != STATUS)
	goto statdone;
      // Extract just the SSRC to see if the session exists
      // NB! Assumes same IP source address *and UDP source port* for status and data
      // This is only true for recent versions of radiod, after the switch to unconnected output sockets
      // But older versions don't send status on the output channel anyway, so no problem
      struct channel chan;
      memset(&chan,0,sizeof(chan));
      struct frontend frontend;
      memset(&frontend,0,sizeof(frontend));
      decode_radio_status(&frontend,&chan,buffer+1,length-1);

      if(Ssrc != 0 && chan.output.rtp.ssrc != Ssrc)
	goto statdone; // Unwanted session, but still clear any data packets

      // Look for existing session
      // Everything must match, or we create a different session & file
      struct session *sp;
      for(sp = Sessions;sp != NULL;sp=sp->next){
	if(sp->ssrc == chan.output.rtp.ssrc
	   && sp->type == chan.output.rtp.type
	   && address_match(&sp->sender,&sender)
	   && getportnumber(&sp->sender) == getportnumber(&sender))
	  break;
      }
      if(sp == NULL){
	// Create session and initialize
	sp = calloc(1,sizeof(*sp));
	if(sp == NULL)
	  goto statdone; // unlikely

	sp->prev = NULL;
	sp->next = Sessions;
	if(sp->next)
	  sp->next->prev = sp;
	Sessions = sp;
	if(Catmode && Ssrc == 0){
	  Ssrc = chan.output.rtp.ssrc; // Latch onto the first ssrc we see, ignore others
	}
      }
      // Wav can't change channels or samprate mid-stream, so if they're going to change we
      // should probably add an option to force stereo and/or some higher sample rate.
      // OggOpus can restart the stream with the new parameters, so it's not a problem
      sp->ssrc = chan.output.rtp.ssrc;
      sp->type = chan.output.rtp.type;
      sp->channels = chan.output.channels;
      sp->encoding = chan.output.encoding;
      sp->samprate = chan.output.samprate;
      memcpy(&sp->sender,&sender,sizeof(sp->sender));
      memcpy(&sp->chan,&chan,sizeof(sp->chan));
      memcpy(&sp->frontend,&frontend,sizeof(sp->frontend));
      sp->last_active = current_time;
      if(sp->fp == NULL){
	session_file_init(sp,&sender);
	if(sp->encoding == OPUS){
	  if(Raw)
	    fprintf(stderr,"--raw ignored on Ogg Opus streams\n");
	  start_ogg_opus_stream(sp);
	  emit_ogg_opus_tags(sp);
	  if(sp->starting_offset != 0)
	    emit_opus_silence(sp,sp->starting_offset);
	} else {
	  if(!Raw)
	    start_wav_stream(sp); // Don't emit wav header in --raw
	  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2);
	  if(sp->can_seek){
	    fseeko(sp->fp,framesize * sp->starting_offset,SEEK_CUR);
	  } else {
	    // Emit zero padding
	    unsigned char *zeroes = calloc(sp->starting_offset,framesize); // Don't use too much stack space
	    fwrite(zeroes,framesize,sp->starting_offset,sp->fp);
	    FREE(zeroes);
	  }
	}
	fflush(sp->fp); // Get the header out on disk so the file won't be empty too long
      }
      // Ogg (containing opus) can concatenate streams with new metadata, so restart when it changes
      // WAV files don't even have this metadata, so ignore changes
      if(sp->encoding == OPUS){
	if(sp->last_frequency != sp->chan.tune.freq
	   || strncmp(sp->last_preset,sp->chan.preset,sizeof(sp->last_preset))){
	  end_ogg_opus_stream(sp);
	  start_ogg_opus_stream(sp);
	  emit_ogg_opus_tags(sp);
	}
      }
    }
  statdone:;
    if(pfd[0].revents & (POLLIN|POLLPRI)){
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(size <= 0){    // ??
	perror("recvfrom");
	continue; // Some sort of error, quit
      }
      if(size < RTP_MIN_SIZE)
	continue; // Too small for RTP, ignore

      struct rtp_header rtp;
      uint8_t *dp = (uint8_t *)ntoh_rtp(&rtp,buffer);
      if(rtp.pad){
	// Remove padding
	size -= dp[size-1];
	rtp.pad = 0;
      }
      if(size <= 0)
	continue; // Bogus RTP header

      size -= (dp - buffer);

      if(Ssrc != 0 && rtp.ssrc != Ssrc)
	continue;

      // Sessions are defined by the tuple {ssrc, payload type, sending IP address, sending UDP port}
      struct session *sp;
      for(sp = Sessions;sp != NULL;sp=sp->next){
	if(sp->ssrc == rtp.ssrc
	   && sp->type == rtp.type
	   && address_match(&sp->sender,&sender)
	   && getportnumber(&sp->sender) == getportnumber(&sender))
	  break;
      }
      // If a matching session is not found, drop packet and wait for first status packet to create it
      // This is a change from previous behavior without status when the first RTP packet would create it
      // This is the only way to work with dynamic payload types since we need the status info
      // We can't even process RTP timestamps without knowing how big a frame is
      if(sp == NULL)
	continue;

      if(!sp->rtp_state.init){
	sp->rtp_state.seq = rtp.seq;
	sp->rtp_state.timestamp = rtp.timestamp;
	sp->rtp_state.init = true;
	if(Verbose > 1)
	  fprintf(stderr,"init seq %u timestamp %u\n",rtp.seq,rtp.timestamp);
      }

      // Place packet into proper place in resequence ring buffer
      int16_t const seqdiff = rtp.seq - sp->rtp_state.seq;
      if(seqdiff < 0){
	// old, drop
	if(Verbose > 1)
	  fprintf(stderr,"drop old sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);
	sp->rtp_state.dupes++;
	// But could be a resynch, should test for this ****
	continue;
      }
      if(seqdiff >= RESEQ){
	// too far ahead to resequence, flush what we have
	// But could be a possible resync or long outage, test for this ****
	if(Verbose > 1)
	  fprintf(stderr,"flushing with drops\n");
	if(sp->encoding == OPUS)
	  send_opus_queue(sp,true);
	else
	  send_wav_queue(sp,true);
	if(Verbose > 1)
	  fprintf(stderr,"reset & queue sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);
      }
      if(Verbose > 2)
	fprintf(stderr,"queue sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);
      int qi = rtp.seq % RESEQ;
      struct reseq * const qp = &sp->reseq[qi];
      qp->inuse = true;
      qp->rtp = rtp;
      qp->data = malloc(size);
      qp->size = size;

      if(sp->encoding == S16BE){
	// Flip endianness from big-endian on network to little endian wanted by .wav
	// byteswap.h is linux-specific; need to find a portable way to get the machine instructions

	int16_t const * const samples = (int16_t *)dp;
	int16_t *wp = (int16_t *)qp->data;
	int samp_count = size / sizeof(int16_t);
	for(int n = 0; n < samp_count; n++)
	  wp[n] = bswap_16((uint16_t)samples[n]);
      } else {
	memcpy(qp->data,dp,size);
      }
      if(sp->encoding == OPUS)
	send_opus_queue(sp,false); // Transmit any packets we can
      else
	send_wav_queue(sp,false);

      if(FileLengthLimit != 0 && sp->samples_remaining <= 0)
	close_file(&sp);
      sp->last_active = gps_time_ns();
      if(Flushmode)
	fflush(sp->fp);
    } // end of packet processing

    if(current_time > last_scan_time + BILLION){
      last_scan_time = current_time;

      // Walk through list, close idle sessions
      struct session *next;
      for(struct session *sp = Sessions;sp != NULL; sp = next){
	next = sp->next; // save in case sp is closed
	int64_t idle = current_time - sp->last_active;
	if(idle > Timeout * BILLION){
	  close_file(&sp); // sp will be NULL
	  // Close idle session
	}
      }
    }
  }
}


static void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session *next_s = Sessions->next;
    close_file(&Sessions); // Sessions will be NULL
    Sessions = next_s;
  }
}
int session_file_init(struct session *sp,struct sockaddr const *sender){
  if(sp->fp != NULL)
    return 0;

  char const *file_encoding = encoding_string(sp->encoding == S16BE ? S16LE : sp->encoding);
  if(Catmode){
    sp->fp = stdout;
    if(Verbose)
    fprintf(stderr,"receiving ssrc %u samprate %d channels %d encoding %s freq %.3lf preset %s offset %lld\n",
	    sp->ssrc,sp->samprate,sp->channels,file_encoding,sp->chan.tune.freq,
	    sp->chan.preset,(long long)sp->starting_offset);
    return 0;
  } else if(Command != NULL){
    // Substitute parameters as specified
    char expanded_command[2048];
    expanded_command[0] = '\0';
    char command_copy[2048]; // Don't overwrite program args
    strlcpy(command_copy,Command,sizeof(command_copy));
    char *cp = command_copy;
    char *a;
    while((a = strsep(&cp,"$")) != NULL){
      strlcat(expanded_command,a,sizeof(expanded_command));
      if(cp != NULL && strlen(cp) > 0){
	char temp[256];
	switch(*cp++){
	case '$':
	  snprintf(temp,sizeof(temp),"$");
	  break;
	case 'd':
	  snprintf(temp,sizeof(temp),"%s",sp->frontend.description);
	  break;
	case 'h':
	  snprintf(temp,sizeof(temp),"%.1lf",sp->chan.tune.freq);
	  break;
	case 'k':
	  snprintf(temp,sizeof(temp),"%.4lf",sp->chan.tune.freq/1000.);
	  break;
	case 'm':
	  snprintf(temp,sizeof(temp),"%.7lf",sp->chan.tune.freq/1000000.);
	  break;
	case 'c':
	  snprintf(temp,sizeof(temp),"%d",sp->channels);
	  break;
	case 'r':
	  snprintf(temp,sizeof(temp),"%d",sp->samprate);
	  break;
	case 's':
	  snprintf(temp,sizeof(temp),"%u",sp->ssrc);
	  break;
	default:
	  break;
	}
	strlcat(expanded_command,temp,sizeof(expanded_command));
      }
    }
    if(Verbose)
      fprintf(stderr,"Executing %s\n",expanded_command);
    sp->fp = popen(expanded_command,"w");
    if(sp->fp == NULL){
      fprintf(stderr,"Cannot start %s, exiting",expanded_command);
      exit(1); // Will probably fail for all others too, just give up
    }
    return 0;
  }
  char const *suffix = ".raw";
  if(!Raw){
    switch(sp->encoding){
    case S16BE:
    case S16LE:
    case F32LE:
      suffix = ".wav";
      break;
    case F16LE:
      suffix = ".f16"; // Non standard! But gotta do something with it for now
      break;
    case OPUS:
      suffix = ".opus";
      break;
    default:
      suffix = ".raw";
      break;
    }
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct timespec file_time = now; // Default to actual time when length limit is not set

  sp->starting_offset = 0;
  sp->samples_remaining = 0;
  if(FileLengthLimit > 0){ // Not really supported on opus yet
    // Pad start of first file with zeroes
#if 0
    struct tm const * const tm_now = gmtime(&now.tv_sec);
    fprintf(stderr,"time now = %4d-%02d-%02dT%02d:%02d:%02d.%dZ\n",
	     tm_now->tm_year+1900,
	     tm_now->tm_mon+1,
	     tm_now->tm_mday,
	     tm_now->tm_hour,
	     tm_now->tm_min,
	     tm_now->tm_sec,
	    (int)(now.tv_nsec / 100000000)); // 100 million, i.e., convert to tenths of a sec
#endif
    // Do time calculations with a modified epoch to avoid overflow problems
    int const epoch = 1704067200; // seconds between Jan 1 1970 00:00:00 UTC (unix epoch) and Jan 1 2024 00:00:00
    intmax_t now_ns = BILLION * (now.tv_sec - epoch) + now.tv_nsec; // ns since Jan 2024
    intmax_t limit = FileLengthLimit * BILLION;
    imaxdiv_t r = imaxdiv(now_ns,limit);
    intmax_t start_ns = r.quot * limit; // ns since epoch
    intmax_t skip_ns = now_ns - start_ns;

    if(skip_ns > (int64_t)(Tolerance * BILLION) && (int64_t)(limit - skip_ns) > Tolerance * BILLION){
      // Adjust file time to previous multiple of specified limit size and pad start to first sample
      imaxdiv_t f = imaxdiv(start_ns,BILLION);
      file_time.tv_sec = f.quot + epoch; // restore original epoch
      file_time.tv_nsec = f.rem;

      sp->starting_offset = (sp->samprate * skip_ns) / BILLION;
      sp->total_file_samples += sp->starting_offset;
#if 0
      fprintf(stderr,"padding %lf sec %lld samples\n",
	      (float)skip_ns / BILLION,
	      sp->starting_offset);
#endif
    }
    sp->samples_remaining = FileLengthLimit * sp->samprate - sp->starting_offset;
  }
  struct tm const * const tm = gmtime(&file_time.tv_sec);
  // yyyy-mm-dd-hh:mm:ss so it will sort properly
#if 0
  fprintf(stderr,"file time = %4d-%02d-%02dT%02d:%02d:%02d.%dZ\n",
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	  (int)(file_time.tv_nsec / 100000000)); // 100 million, i.e., convert to tenths of a sec
#endif

  if(Jtmode){
    //  K1JT-format file names in flat directory
    snprintf(sp->filename,sizeof(sp->filename),"%4d%02d%02dT%02d%02d%02dZ_%.0lf_%s%s",
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     sp->chan.tune.freq,
	     sp->chan.preset,
	     suffix);
  } else if(Subdirs){
    // Create directory path
    char dir[PATH_MAX];
    snprintf(dir,sizeof(dir),"%u",sp->ssrc);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d",sp->ssrc,tm->tm_year+1900);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d/%d",sp->ssrc,tm->tm_year+1900,tm->tm_mon+1);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d/%d/%d",sp->ssrc,tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(sp->filename,sizeof(sp->filename),"%u/%d/%d/%d/%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ%s",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(file_time.tv_nsec / 100000000), // 100 million, i.e., convert to tenths of a sec
	     suffix);
  } else {
    // create file in current directory
    snprintf(sp->filename,sizeof(sp->filename),"%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ%s",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(file_time.tv_nsec / 100000000),
	     suffix);
  }
  sp->fp = fopen(sp->filename,"w++");

  if(sp->fp == NULL){
    fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
    return -1;
  }
  {
    struct stat statbuf;
    if(fstat(fileno(sp->fp),&statbuf) != 0){
      fprintf(stderr,"stat(%s) failed: %s\n",sp->filename,strerror(errno));
    } else {
      switch(statbuf.st_mode & S_IFMT){
      case S_IFREG:
	sp->can_seek = true;
	break;
      default:
	sp->can_seek = false;
	break;
      }
    }
  }
  // We byte swap S16BE to S16LE, so change the tag
  if(Verbose)
    fprintf(stderr,"creating %s ssrc %u samprate %d channels %d encoding %s freq %.3lf preset %s offset %lld\n",
	    sp->filename,sp->ssrc,sp->samprate,sp->channels,file_encoding,sp->chan.tune.freq,
	    sp->chan.preset,(long long)sp->starting_offset);
  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

  int const fd = fileno(sp->fp);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data

  attrprintf(fd,"encoding","%s",file_encoding);
  attrprintf(fd,"samprate","%u",sp->samprate);
  attrprintf(fd,"channels","%d",sp->channels);
  attrprintf(fd,"ssrc","%u",sp->ssrc);
  attrprintf(fd,"frequency","%.3lf",sp->chan.tune.freq);
  attrprintf(fd,"preset","%s",sp->chan.preset);
  char sender_text[NI_MAXHOST];
  // Don't wait for an inverse resolve that might cause us to lose data
  getnameinfo((struct sockaddr *)sender,sizeof(*sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
  attrprintf(fd,"source","%s",sender_text);
  attrprintf(fd,"multicast","%s",PCM_mcast_address_text);
  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);

  if(sp->frontend.description)
    attrprintf(fd,"description","%s",sp->frontend.description);

  if(sp->starting_offset != 0)
    attrprintf(fd,"starting offset","%lld",sp->starting_offset);

  if(sp->chan.demod_type == LINEAR_DEMOD && !sp->chan.linear.agc)
    attrprintf(fd,"gain","%.3f",voltage2dB(sp->chan.output.gain));
  return 0;
}

// Close a session, update .wav header, remove from session table
// If the file is not "substantial", just delete it
static int close_file(struct session **spp){
  if(spp == NULL)
    return -1;
  struct session *sp = *spp;

  if(sp == NULL)
    return -1;

  if(sp->encoding == OPUS)
    end_ogg_opus_stream(sp);
  else
    end_wav_stream(sp);

  if(Verbose){
    fprintf(stderr,"closing %s %'.1f/%'.1f sec\n",sp->filename, // might be blank
            (float)sp->samples_written / (sp->samprate * sp->channels),
            (float)sp->total_file_samples / (sp->samprate * sp->channels));
  }
  if(Verbose > 1 && (sp->rtp_state.dupes != 0 || sp->rtp_state.drops != 0))
    fprintf(stderr," dupes %llu drops %llu\n",(long long unsigned)sp->rtp_state.dupes,(long long unsigned)sp->rtp_state.drops);

  if(sp->substantial_file){ // Don't bother for non-substantial files
    if(sp->can_seek){
      int fd = fileno(sp->fp);
      attrprintf(fd,"samples written","%lld",sp->samples_written);
      attrprintf(fd,"total samples","%lld",sp->total_file_samples);
    }
  } else if(sp->can_seek && strlen(sp->filename) > 0){
    unlink(sp->filename);
    if(Verbose)
      fprintf(stderr,"deleting %s %'.1f/%'.1f sec\n",sp->filename,
            (float)sp->samples_written / (sp->samprate * sp->channels),
            (float)sp->total_file_samples / (sp->samprate * sp->channels));
  }
  if(Command != NULL)
    pclose(sp->fp);
  else
    fclose(sp->fp);
  sp->fp = NULL;
  FREE(sp->iobuffer);

  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  if(sp->next)
    sp->next->prev = sp->prev;
  FREE(sp);
  return 0;
}

static int start_ogg_opus_stream(struct session *sp){
  if(sp == NULL)
    return -1;
  // Create Ogg container with Opus codec
  int serial = rand(); // Unique stream serial number
  if (ogg_stream_init(&sp->oggState, serial) != 0) {
    fprintf(stderr, "Failed to initialize Ogg stream.\n");
    return -1;
  }
  sp->granulePosition = 0;
  sp->packetCount = 0;

  struct {
    char head[8];
    uint8_t version;
    uint8_t channels;
    int16_t preskip;
    uint32_t samprate;
    int16_t gain;
    uint8_t map_family;
  } opusHeader;

  memset(&opusHeader,0,sizeof(opusHeader));
  memcpy(opusHeader.head, "OpusHead", 8);  // Signature
  opusHeader.version = 1;                  // Version
  // Some decoders get confused when the channel count or sample rate changes in a stream, so always say we're emitting 48kHz stereo.
  // Opus won't use more bits when the input is actually mono and/or at a lower rate
  opusHeader.channels = 2;
  opusHeader.preskip = 312;
  opusHeader.samprate = 48000;
  opusHeader.gain = 0;
  opusHeader.map_family = 0;

  ogg_packet idPacket;
  idPacket.packet = (unsigned char *)&opusHeader;
  idPacket.bytes = 19; // sizeof(opusHeader) pads up, can't use!
  idPacket.b_o_s = 1;  // Beginning of stream
  idPacket.e_o_s = 0;
  idPacket.granulepos = 0; // always zero
  idPacket.packetno = sp->packetCount++;
  // Add the identification header to the Ogg stream
  ogg_stream_packetin(&sp->oggState, &idPacket);
  ogg_page oggPage;
  while (ogg_stream_flush(&sp->oggState, &oggPage)) {
    fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
    fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
  }
  return 0;
}
static int emit_ogg_opus_tags(struct session *sp){
  if(sp == NULL)
    return -1;

  if(ogg_stream_check(&sp->oggState))
    return -1;

  // fill this in with sender ID (ka9q-radio, etc, frequency, mode, etc etc)
  uint8_t opusTags[2048]; // Variable length, not easily represented as a structure
  memset(opusTags,0,sizeof(opusTags));
  memcpy(opusTags,"OpusTags",8);
  uint8_t *wp = opusTags + 8;
  wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),"KA9Q-radio"); // Vendor - don't bother computing actual remaining sp
  int32_t *np = (int32_t *)wp;
  *np++ = 8; // Number of tags follows
  wp = (uint8_t *)np;
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"ENCODER=KA9Q radiod - %s",opus_get_version_string());
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);

  char datestring[128];
  char timestring[128];
  snprintf(datestring,sizeof(datestring),"%4d-%02d-%02d",
	   tm->tm_year+1900,
	   tm->tm_mon+1,
	   tm->tm_mday);

  snprintf(timestring,sizeof(timestring),"%02d:%02d:%02d.%03d UTC",
	   tm->tm_hour,
	   tm->tm_min,
	   tm->tm_sec,
	   (int)(now.tv_nsec / 1000000));
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"TITLE=ssrc %u: %'.3lf Hz %s, %s %s",sp->ssrc,sp->chan.tune.freq,sp->chan.preset,
	     datestring,timestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"TIME=%s",timestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"DATE=%s",datestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  if(sp->frontend.description != NULL){
    char temp[256];
    snprintf(temp,sizeof(temp),"DESCRIPTION=%s",sp->frontend.description);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"SSRC=%u",sp->ssrc);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"FREQUENCY=%.3lf",sp->chan.tune.freq);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"PRESET=%s",sp->chan.preset);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  ogg_packet tagsPacket;
  tagsPacket.packet = opusTags;
  tagsPacket.bytes = wp - opusTags;
  tagsPacket.b_o_s = 0;             // Not the beginning of the stream
  tagsPacket.e_o_s = 0;             // Not the end of the stream
  tagsPacket.granulepos = sp->granulePosition;        // No granule position for metadata
  tagsPacket.packetno = sp->packetCount++;          // Second packet in the stream

  ogg_stream_packetin(&sp->oggState, &tagsPacket);
  assert(sp->fp != NULL);
  ogg_page oggPage;
  while (ogg_stream_flush(&sp->oggState, &oggPage)) {
    fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
    fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
  }
  // Remember so we'll detect changes
  sp->last_frequency = sp->chan.tune.freq;
  strlcpy(sp->last_preset,sp->chan.preset,sizeof(sp->last_preset));
  return 0;
}
static int end_ogg_opus_stream(struct session *sp){
  if(sp == NULL)
    return -1;

  if(ogg_stream_check(&sp->oggState))
    return -1;

  // Terminate ogg Opus file
  // Write an empty packet with the end bit set
  ogg_packet endPacket;
  endPacket.packet = OpusSilence;
  endPacket.bytes = sizeof(OpusSilence);
  endPacket.b_o_s = 0;
  endPacket.e_o_s = 1;    // End of stream flag
  endPacket.granulepos = sp->granulePosition; // Granule position
  sp->granulePosition += 0; // Doesn't change
  endPacket.packetno = sp->packetCount++; // Increment packet number (not actually used again)

  // Add the packet to the Ogg stream
  int ret = ogg_stream_packetin(&sp->oggState, &endPacket);
  (void)ret;
  assert(ret == 0);
  // Flush the stream to ensure packets are written
  ogg_page finalPage;
  while (ogg_stream_flush(&sp->oggState, &finalPage)) {
    fwrite(finalPage.header, 1, finalPage.header_len, sp->fp);
    fwrite(finalPage.body, 1, finalPage.body_len, sp->fp);
  }
  ogg_stream_clear(&sp->oggState);
  return 0;
}


// Encode a string as {length,string}, with length in 4 bytes, little endian, string without terminating null
// Return pointer to first unused byte in output
// Used in writing Ogg tags
static uint8_t *encodeTagString(uint8_t *out,int size,const char *string){
  if(out == NULL || string == NULL || size <= sizeof(uint32_t))
    return out;

  int len = strlen(string);
  uint32_t *wp = (uint32_t *)out;
  *wp = len;
  uint8_t *sp = out + sizeof(uint32_t);
  size -= sizeof(uint32_t);
  memcpy(sp,string,min(len,size));
  sp += min(len,size);
  return sp;
}
// Write WAV header at start of file
// Leave file positioned after header
static int start_wav_stream(struct session *sp){
  if(sp == NULL)
    return -1;

  // Write .wav header, skipping size fields
  struct wav header;
  memset(&header,0,sizeof(header));

  memcpy(header.ChunkID,"RIFF", 4);
  header.ChunkSize = 0xffffffff; // Temporary
  memcpy(header.Format,"WAVE",4);
  memcpy(header.Subchunk1ID,"fmt ",4);
  header.Subchunk1Size = 40; // ??????
  switch(sp->encoding){
  default:
    return -1;
  case S16LE:
  case S16BE:
    header.AudioFormat = 1;
    header.BitsPerSample = 16;
    header.ByteRate = sp->samprate * sp->channels * 2;
    header.BlockAlign = sp->channels * 2;;
    break;
  case F32LE:
    header.AudioFormat = 3;
    header.BitsPerSample = 32;
    header.ByteRate = sp->samprate * sp->channels * 4;
    header.BlockAlign = sp->channels * 4;
    break;
  case F16LE:
    header.AudioFormat = 0; // What should go here for IEEE 16-bit float?
    header.BitsPerSample = 16;
    header.ByteRate = sp->samprate * sp->channels * 2;
    header.BlockAlign = sp->channels * 2;;
    break;
  }

  header.NumChannels = sp->channels;
  header.SampleRate = sp->samprate;
  header.ExtensionChunkSize = 22;
  memcpy(header.SubChunk2ID,"data",4);
  header.Subchunk2Size = 0xffffffff; // Temporary

  //  appears to be needed for FP
  memcpy(header.FactID,"fact",4);
  header.FactSize = 4;
  header.SamplesLength = 0xffffffff;

  // fill in the auxi chunk (start time, center frequency)
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);

  memcpy(header.AuxID, "auxi", 4);
  header.AuxSize=164;
  header.StartYear=tm->tm_year+1900;
  header.StartMon=tm->tm_mon+1;
  header.StartDOW=tm->tm_wday;
  header.StartDay=tm->tm_mday;
  header.StartHour=tm->tm_hour;
  header.StartMinute=tm->tm_min;
  header.StartSecond=tm->tm_sec;
  header.StartMillis=(int16_t)(now.tv_nsec / 1000000);
  header.CenterFrequency= sp->chan.tune.freq;
  memset(header.AuxUknown, 0, 128);
  if(sp->can_seek)
    rewind(sp->fp); // should be at BOF but make sure
  fwrite(&header,sizeof(header),1,sp->fp);
  return 0;
}
// Update wav header with now-known size and end auxi information
static int end_wav_stream(struct session *sp){
  if(sp == NULL)
    return -1;

  if(!sp->can_seek)
    return 0; // Can't seek back to the beginning on a pipe

  rewind(sp->fp);
  struct wav header;
  if(fread(&header,sizeof(header),1,sp->fp) != 1)
    return -1;

  struct stat statbuf;
  if(fstat(fileno(sp->fp),&statbuf) != 0){
    fprintf(stderr,"fstat(%d) [%s] failed! %s\n",fileno(sp->fp),sp->filename,strerror(errno));
    return -1;
  }
  header.ChunkSize = statbuf.st_size - 8;
  header.Subchunk2Size = statbuf.st_size - sizeof(header);

  // write number of samples (or is it frames?) into the fact chunk
  header.SamplesLength = sp->samples_written / sp->channels;

  // write end time into the auxi chunk
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);
  header.StopYear=tm->tm_year+1900;
  header.StopMon=tm->tm_mon+1;
  header.StopDOW=tm->tm_wday;
  header.StopDay=tm->tm_mday;
  header.StopHour=tm->tm_hour;
  header.StopMinute=tm->tm_min;
  header.StopSecond=tm->tm_sec;
  header.StopMillis=(int16_t)(now.tv_nsec / 1000000);

  rewind(sp->fp);
  if(fwrite(&header,sizeof(header),1,sp->fp) != 1)
    return -1;
  return 0;
}
