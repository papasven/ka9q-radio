// Interactive program to send commands and display internal state of 'radiod'
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Written as one big polling loop because ncurses is **not** thread safe

// Copyright 2017-2023 Phil Karn, KA9Q
// Major revisions fall 2020, 2023 (really continuous revisions!)

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h> // for arc4random()
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <poll.h>
#include <sys/time.h>
#include <sys/select.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <iniparser/iniparser.h>
#include <sysexits.h>
#include <errno.h>

#include "misc.h"
#include "filter.h"
#include "multicast.h"
#include "bandplan.h"
#include "status.h"
#include "radio.h"
#include "config.h"

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1; // LAN only, no routers
static float Refresh_rate = 0.25f;
static char Locale[256] = "en_US.UTF-8";
static char const *Presets_file = "presets.conf"; // make configurable!
static dictionary *Pdict;
struct frontend Frontend;
static struct sockaddr_storage Metadata_source_address;      // Source of metadata
static struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)
static uint64_t Block_drops; // Stored in output filter on sender, not in channel structure

int Mcast_ttl = DEFAULT_MCAST_TTL;
int IP_tos = DEFAULT_IP_TOS;
float Blocktime;
int Ctl_fd,Status_fd;
uint64_t Metadata_packets;
const char *App_path;
int Verbose;

static struct control {
  int item;
  bool lock;
  int step;
} Control;


static int pprintw(WINDOW *w,int y, int x, char const *prefix, char const *fmt, ...);

static WINDOW *Tuning_win,*Sig_win,*Filtering_win,*Demodulator_win,
  *Options_win,*Presets_win,*Debug_win,
  *Output_win;

static void display_tuning(WINDOW *tuning,struct channel const *channel);
static void display_info(WINDOW *w,int row,int col,struct channel const *channel);
static void display_filtering(WINDOW *filtering,struct channel const *channel);
static void display_sig(WINDOW *sig,struct channel const *channel);
static void display_demodulator(WINDOW *demodulator,struct channel const *channel);
static void display_options(WINDOW *options,struct channel const *channel);
static void display_presets(WINDOW *modes,struct channel const *channel);
static void display_output(WINDOW *output,struct channel const *channel);
static int process_keyboard(struct channel *,uint8_t **bpp,int c);
static void process_mouse(struct channel *channel,uint8_t **bpp);
static int decode_radio_status(struct channel *channel,uint8_t const *buffer,int length);
static int for_us(struct channel *channel,uint8_t const *buffer,int length,uint32_t ssrc);
static int init_demod(struct channel *channel);

// Pop up a temporary window with the contents of a file in the
// library directory (usually /usr/local/share/ka9q-radio/)
// then wait for a single keyboard character to clear it
static void popup(char const *filename){
  static int const maxcols = 256;
  char fname[PATH_MAX];
  if (dist_path(fname,sizeof(fname),filename) == -1)
    return;
  FILE * const fp = fopen(fname,"r");
  if(fp == NULL)
    return;
  // Determine size of box
  int rows=0, cols=0;
  char line[maxcols];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    rows++;
    if(strlen(line) > cols)
      cols = strlen(line); // Longest line
  }
  rewind(fp);
  
  // Allow room for box
  WINDOW * const pop = newwin(rows+2,cols+2,0,0);
  box(pop,0,0);
  int row = 1; // Start inside box
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    mvwaddstr(pop,row++,1,line);
  }
  fclose(fp);
  wnoutrefresh(pop);
  doupdate();
  wtimeout(pop,-1); // blocking read - wait indefinitely
  (void)wgetch(pop); // Read and discard one character
  wtimeout(pop,0);
  werase(pop);
  wrefresh(pop);
  delwin(pop);
}

// Pop up a dialog box, issue a prompt and get a response
static void getentry(char const *prompt,char *response,int len){
  int boxwidth = strlen(prompt) + len;
  WINDOW * const pwin = newwin(5,boxwidth+2,0,0);
  box(pwin,0,0);
  mvwaddstr(pwin,1,1,prompt);
  wrefresh(pwin);
  echo();
  timeout(-1);
  // Manpage for wgetnstr doesn't say whether a terminating
  // null is stashed. Hard to believe it isn't, but this is to be sure
  memset(response,0,len);
  int r = wgetnstr(pwin,response,len);
  if(r != OK)
    memset(response,0,len); // Zero out the read buffer
  chomp(response);
  timeout(0);
  noecho();
  werase(pwin);
  wrefresh(pwin);
  delwin(pwin);
}

static FILE *Tty;
static SCREEN *Term;

static void display_cleanup(void){
  echo();
  nocbreak();
  if(!isendwin()){
    endwin();
    refresh();
  }
  if(Term)
    delscreen(Term);
  Term = NULL;
  if(Tty)
    fclose(Tty);
  Tty = NULL;
}

static bool Frequency_lock;

// Adjust the selected item up or down one step
static void adjust_item(struct channel *channel,uint8_t **bpp,int direction){
  double tunestep = pow(10., (double)Control.step);

  if(!direction)
    tunestep = - tunestep;

  switch(Control.item){
  case 0: // Carrier frequency
    if(!Frequency_lock){ // Ignore if locked
      channel->tune.freq += tunestep;
      encode_double(bpp,RADIO_FREQUENCY,channel->tune.freq);
    }
    break;
  case 1: // First LO
    if(Control.lock) // Tuner is locked, don't change it
      break;
    // Send via radiod
    encode_float(bpp,FIRST_LO_FREQUENCY,Frontend.frequency+tunestep);
    break;
  case 2: // IF (not implemented)
    break;
  case 3: // Filter low edge (hertz rather than millihertz)
    {
      float const x = min(channel->filter.max_IF,channel->filter.min_IF + (float)tunestep * 1000);
      channel->filter.min_IF = x;
      encode_float(bpp,LOW_EDGE,x);
    }
    break;
  case 4: // Filter high edge
    {
      float const x = max(channel->filter.min_IF,channel->filter.max_IF + (float)tunestep * 1000);
      channel->filter.max_IF = x;
      encode_float(bpp,HIGH_EDGE,x);
    }
    break;
  case 5: // Post-detection audio frequency shift
    channel->tune.shift += tunestep;
    encode_double(bpp,SHIFT_FREQUENCY,channel->tune.shift);
    break;

  }
}

// It seems better to just use the Griffin application to turn knob events into keystrokes or mouse events
static void adjust_up(struct channel *channel,uint8_t **bpp){
  adjust_item(channel,bpp,1);
}
static void adjust_down(struct channel *channel,uint8_t **bpp){
  adjust_item(channel,bpp,0);
}
static void toggle_lock(void){
  switch(Control.item){
  case 0:
    Frequency_lock = !Frequency_lock; // Toggle frequency tuning lock
    break;
  case 1:
    Control.lock = !Control.lock;
    break;
  }
}
// List of status windows, in order they'll be created, with sizes
static struct windef {
  WINDOW **w;
  int rows;
  int cols;
} Windefs[] = {
  {&Tuning_win, 15, 30},
  {&Options_win, 15, 12},  
  //  {&Presets_win,Npresets+2,9}, // Npresets is not a static initializer
  {&Presets_win,15,9},
  {&Sig_win,15,25},
  {&Demodulator_win,15,26},
  {&Filtering_win,15,22},
  {&Output_win,15,45},
};
#define NWINS (sizeof(Windefs) / sizeof(Windefs[0]))

static void setup_windows(void){
  // First row
  int row = 0;
  int col = 0;
  int maxrows = 0;

  endwin();
  refresh();
  clear();

  struct winsize w;
  ioctl(fileno(Tty),TIOCGWINSZ,&w);
  COLS = w.ws_col;
  LINES = w.ws_row;
  
  // Delete all previous windows
  for(int i=0; i < NWINS; i++){
    if(*Windefs[i].w)
      delwin(*Windefs[i].w);
    *Windefs[i].w = NULL;
  }
  // Create as many as will fit
  for(int i=0; i < NWINS; i++){  
    if(COLS < col + Windefs[i].cols){
      // No more room on this line, go to next
      col = 0;
      row += maxrows;
      maxrows = 0;
    }      
    if(LINES < row + Windefs[i].rows){
      // No more room for anything
      return;
    }
    // Room on this line
    * Windefs[i].w = newwin(Windefs[i].rows,Windefs[i].cols,row,col);
    col += Windefs[i].cols;
    maxrows = max(maxrows,Windefs[i].rows);
  }
  // Specially set up debug window
  // Minimum of 45 cols for debug window, otherwise go to next row
  if(col + 45 > COLS){
    row += maxrows;
    col = 0;
  }
  if(row < LINES && col < COLS)
    Debug_win = newwin(LINES - row,COLS-col,row,col); // Only if room is left
  // A message from our sponsor...
  scrollok(Debug_win,TRUE); // This one scrolls so it can be written to with wprintw(...\n)
  wprintw(Debug_win,"KA9Q-radio %s last modified %s\n",__FILE__,__TIMESTAMP__);
  wprintw(Debug_win,"Copyright 2023, Phil Karn, KA9Q. May be used under the terms of the GNU Public License\n");
}

#if 0
static int dcompare(void const *a,void const *b){
  struct channel const *da = a;
  struct channel const *db = b;  
  if(da->output.rtp.ssrc < db->output.rtp.ssrc)
    return -1;
  if(da->output.rtp.ssrc > db->output.rtp.ssrc)
    return +1;
  return 0;
}
#endif


static uint32_t Ssrc = 0;

static struct channel Channel;

// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
int main(int argc,char *argv[]){
  App_path = argv[0];
  {
    int c;
    while((c = getopt(argc,argv,"vVs:r:")) != -1){
      switch(c){
      case 'V':
	VERSION();
	exit(EX_OK);
      case 'v':
	Verbose++;
	break;
      case 's':
	Ssrc = strtol(optarg,NULL,0); // Send to specific SSRC
	break;
      case 'r':
	Refresh_rate = strtod(optarg,NULL);
	break;
      default:
	fprintf(stdout,"Unknown option %c\n",c);
	break;
      }
    }
  }
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  // Dummy filter
  if(argc <= optind){
    fprintf(stderr,"Usage: %s -s <ssrc> mcast_group\n",App_path);
    fprintf(stderr,"<ssrc> is positive decimal number, mcast_group is DNS name or IP address of control multicast group\n");
    exit(EX_USAGE);
  }
  while(Ssrc == 0){
    Ssrc = arc4random();
    fprintf(stderr,"-s missing, generating random ssrc: %'u\n",Ssrc);
  }
  char iface[1024]; // Multicast interface
  resolve_mcast(argv[optind],&Metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
  Status_fd = listen_mcast(&Metadata_dest_address,iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't listen to mcast status %s\n",argv[optind]);
    exit(EX_IOERR);
  }
  Ctl_fd = connect_mcast(&Metadata_dest_address,iface,Mcast_ttl,IP_tos);
  if(Ctl_fd < 0){
    fprintf(stderr,"connect to mcast control failed\n");
    exit(EX_IOERR);
  }
  char presetsfile_path[PATH_MAX];
  if (dist_path(presetsfile_path,sizeof(presetsfile_path),Presets_file) == -1) {
    fprintf(stderr,"Could not find mode file %s\n", Presets_file);
    exit(EX_NOINPUT);
  }
  Pdict = iniparser_load(presetsfile_path);
  if(Pdict == NULL){
    fprintf(stdout,"Can't load mode file %s\n",presetsfile_path);
    exit(EX_NOINPUT);
  }
  atexit(display_cleanup);

  // Set up display subwindows
  Tty = fopen("/dev/tty","r+");
  Term = newterm(NULL,Tty,Tty);
  set_term(Term);

  //  meta(stdscr,TRUE);
  keypad(stdscr,TRUE);
  timeout(0); // Don't block in getch()
  cbreak();
  noecho();
  mmask_t const mask = ALL_MOUSE_EVENTS;
  mousemask(mask,NULL);
  setup_windows();

  struct channel *const channel = &Channel;
  init_demod(channel);

  Frontend.frequency = Frontend.min_IF = Frontend.max_IF = NAN;

  /* Main loop:
     Send poll if we haven't received one in our refresh interval
     See if anything has arrived (use short timeout)
     If there's a response, update local status & repaint display windows
     Poll keyboard and process user commands

     Randomize polls over +/- 32 ms in case someone else is also polling
     This avoids possible synchronized back-to-back polls
     This is a common technique in multicast protocols (e.g., IGMP queries)
  */
  int const random_interval = 64 << 20; // power of 2 makes it easier for arc4random_uniform()

  int64_t now = gps_time_ns();
  int64_t next_radio_poll = now; // Immediate first poll
  bool screen_update_needed = false;
  for(;;){
    int64_t const radio_poll_interval = Refresh_rate * BILLION; // Can change from the keyboard
    if(now >= next_radio_poll){
      // Time to poll radio
      send_poll(Ctl_fd,Ssrc);
#ifdef DEBUG_POLL
      wprintw(Debug_win,"poll sent %lld\n",now);
#endif
      // Retransmit after 1/10 sec if no response
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
    }
    // Poll the input socket
    // This paces keyboard polling so wait no more than 100 ms, even for long refresh intervals
    int const recv_timeout = BILLION/10;    
    int64_t start_of_recv_poll = now;
    uint8_t buffer[PKTSIZE];
    int length = 0;
    do {
      int const npoll = 1;
      struct pollfd pollfd[npoll];
      pollfd[0].fd = Status_fd;
      pollfd[0].events = POLLIN;

      int n = poll(pollfd,npoll,100);
      now = gps_time_ns(); // poll() blocks, so update the time of day (only place we block)
      if(n < 0){
	wprintw(Debug_win,"poll() failure, %s\n",strerror(errno));
	screen_update_needed = true;
      }
      if(pollfd[0].revents & POLLIN){
	// Message from the radio program (or some transcoders)
	struct sockaddr_storage source_address;
	socklen_t ssize = sizeof(source_address);
	length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&source_address,&ssize); // should not block
	// Ignore our own command packets and responses to other SSIDs
	if(length >= 2 && (enum pkt_type)buffer[0] == STATUS && for_us(channel,buffer+1,length-1,Ssrc) >= 0 ){
	  // Save source only if it's a response
	  memcpy(&Metadata_source_address,&source_address,sizeof(Metadata_source_address));
	  break; // Got a response
	}
      }
    } while(now < start_of_recv_poll + recv_timeout);
    if(length > 0){
      screen_update_needed = true;
#ifdef DEBUG_POLL 
      wprintw(Debug_win,"got response length %d\n",length);
#endif
      decode_radio_status(channel,buffer+1,length-1);
      // Postpone next poll to specified interval
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
      if(Blocktime == 0 && Frontend.samprate != 0)
	Blocktime = 1000.0f * Frontend.L / Frontend.samprate; // Set the firat time
    }      
    // Set up command buffer in case we want to change something
    uint8_t cmdbuffer[PKTSIZE];
    uint8_t *bp = cmdbuffer;
    *bp++ = CMD; // Command

    // Poll keyboard and mouse
    int const c = getch();
    if(c == KEY_MOUSE){
      process_mouse(channel,&bp);
      screen_update_needed = true;
    } else if(c != ERR) {
      screen_update_needed = true;
      if(process_keyboard(channel,&bp,c) == -1)
	goto quit;
    }
    // Any commands to send?
    if(bp > cmdbuffer+1){
      // Yes
      assert(Ssrc != 0);
      encode_int(&bp,OUTPUT_SSRC,Ssrc); // Specific SSRC
      encode_int(&bp,COMMAND_TAG,arc4random()); // Append a command tag
      encode_eol(&bp);
      int const command_len = bp - cmdbuffer;
#ifdef DEBUG_POLL
      wprintw(Debug_win,"sent command len %d\n",command_len);
      screen_update_needed = true; // show local change right away
#endif
      if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len){
	wprintw(Debug_win,"command send error: %s\n",strerror(errno));
	screen_update_needed = true; // show local change right away
      }	
      // This will elicit an answer, defer the next poll
      next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
    }
    if(screen_update_needed){
      // update display windows
      display_tuning(Tuning_win,channel);
      display_filtering(Filtering_win,channel);
      display_sig(Sig_win,channel);
      display_demodulator(Demodulator_win,channel);
      display_options(Options_win,channel);
      display_presets(Presets_win,channel);
      display_output(Output_win,channel);
      
      if(Debug_win != NULL){
	touchwin(Debug_win); // since we're not redrawing it every cycle
	wnoutrefresh(Debug_win);
      }    
      doupdate();      // Update the screen right before we pause
      screen_update_needed = false;
    }    
  }
 quit:;
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
  if(Tty != NULL)
    fclose(Tty);
  exit(EX_OK);
}

int const Entry_width = 15;

static int process_keyboard(struct channel *channel,uint8_t **bpp,int c){
  // Look for keyboard and mouse events

  switch(c){
  case ERR:
    break;
  case KEY_RESIZE:
    setup_windows();
    break;
  case 0x3: // ^C
  case 'q':   // Exit entire radio program. Should this be removed? ^C also works.
    return -1;
  case 'h':
  case '?':
    popup("help.txt");
    break;
  case 'l': // Toggle RF or first LO lock; affects how adjustments to LO and IF behave
    toggle_lock();
    break;
  case KEY_NPAGE: // Page Down/tab key
  case '\t':      // go to next tuning item
    Control.item = (Control.item + 1) % 6;
    break;
  case KEY_BTAB:  // Page Up/Backtab, i.e., shifted tab:
  case KEY_PPAGE: // go to previous tuning item
    Control.item = (6 + Control.item - 1) % 6;
    break;
  case KEY_HOME: // Go back to item 0
    Control.item = 0;
    Control.step = 0;
    break;
  case KEY_BACKSPACE: // Cursor left: increase tuning step 10x
  case KEY_LEFT:
    if(Control.step >= 9){
      beep();
      break;
    }
    Control.step++;
    break;
  case KEY_RIGHT:     // Cursor right: decrease tuning step /10
    if(Control.step <= -3){
      beep();
      break;
    }
    Control.step--;
    break;
  case KEY_UP:        // Increase whatever digit we're tuning
    adjust_up(channel,bpp);
    break;
  case KEY_DOWN:      // Decrease whatever we're tuning
    adjust_down(channel,bpp);
    break;
  case '\f':  // Screen repaint (formfeed, aka control-L)
    clearok(curscr,TRUE);
    break;
  case 's': // Squelch threshold for current mode
    {
      char str[Entry_width],*ptr;
      getentry("Squelch SNR: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str){
	encode_float(bpp,SQUELCH_OPEN,x);
	encode_float(bpp,SQUELCH_CLOSE,x - 1); // Make this a separate command
      }
    }
    break;
  case 'T': // Hang time, s (always taken as positive)
    {
      char str[Entry_width],*ptr;
      getentry("Hang time, s: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,AGC_HANGTIME,x);
    }
    break;
  case 'P': // PLL loop bandwidth, always positive
    {
      char str[Entry_width],*ptr;
      getentry("PLL loop bandwidth, Hz: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,PLL_BW,x);
    }
    break;
  case 'L': // AGC threshold, dB relative to headroom
    {
      char str[Entry_width],*ptr;
      getentry("AGC threshold, dB: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str)
	encode_float(bpp,AGC_THRESHOLD,x);
    }
    break;
  case 'R': // Recovery rate, dB/s (always taken as positive)
    {
      char str[Entry_width],*ptr;
      getentry("Recovery rate, dB/s: ",str,sizeof(str));
      float const x = fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,AGC_RECOVERY_RATE,x);
    }
    break;
  case 'H': // Target AGC output level (headroom), dB, taken as negative
    {
      char str[Entry_width],*ptr;
      getentry("Headroom, dB: ",str,sizeof(str));
      float const x = -fabsf(strtof(str,&ptr));
      if(ptr != str)
	encode_float(bpp,HEADROOM,x);
    }
    break;
  case 'g': // Manually set linear channel gain, dB (positive or negative)
    {
      char str[Entry_width],*ptr;
      getentry("Gain, dB: ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      if(ptr != str){
	encode_float(bpp,GAIN,x);
	encode_byte(bpp,AGC_ENABLE,0); // Also done implicitly in radiod
      }
    }
    break;
  case 'r': // Poll/refresh rate
    {
      char str[Entry_width],*ptr;
      getentry("Refresh rate (s): ",str,sizeof(str));
      float const x = strtof(str,&ptr);
      Refresh_rate = x;
    }
    break;
  case 'p':
  case 'm': // Manual mode preset
    {
      char str[Entry_width];
      char prompt[1024];
      snprintf(prompt,sizeof(prompt),"Mode/Preset [ ");
      int const nsec = iniparser_getnsec(Pdict);

      for(int i=0;i < nsec;i++){
	strlcat(prompt,iniparser_getsecname(Pdict,i),sizeof(prompt));
	strlcat(prompt," ",sizeof(prompt));
      }
      strlcat(prompt,"]: ",sizeof(prompt));
      getentry(prompt,str,sizeof(str));
      if(strlen(str) > 0)
	encode_string(bpp,PRESET,str,strlen(str));
    }
    break;
  case 'f':   // Tune to new radio frequency
    {
      char str[Entry_width];
      getentry("Carrier frequency: ",str,sizeof(str));
      if(strlen(str) > 0){
	channel->tune.freq = fabs(parse_frequency(str,true)); // Handles funky forms like 147m435
	encode_double(bpp,RADIO_FREQUENCY,channel->tune.freq);
      }
    }
    break;
  case 'k': // Kaiser window parameter
    {
      char str[Entry_width],*ptr;
      getentry("Kaiser window beta: ",str,sizeof(str));
      double const b = strtod(str,&ptr);
      if(ptr == str)
	break; // nothing entered
      if(b < 0 || b >= 100){
	beep();
	break; // beyond limits
      }
      encode_float(bpp,KAISER_BETA,b);
    }
    break;
  case 'o': // Set/clear option flags, most apply only to linear detector
    {
      char str[Entry_width];
      getentry("[isb pll square stereo mono agc], '!' prefix disables: ",str,sizeof(str));
      if(strcasecmp(str,"mono") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,1);
      } else if(strcasecmp(str,"!mono") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,2);
      } else if(strcasecmp(str,"stereo") == 0){
	encode_int(bpp,OUTPUT_CHANNELS,2);
      } else if(strcasecmp(str,"isb") == 0){
	encode_byte(bpp,INDEPENDENT_SIDEBAND,1);
      } else if(strcasecmp(str,"!isb") == 0){
	encode_byte(bpp,INDEPENDENT_SIDEBAND,0);
      } else if(strcasecmp(str,"pll") == 0){
	encode_byte(bpp,PLL_ENABLE,1);
      } else if(strcasecmp(str,"!pll") == 0){
	encode_byte(bpp,PLL_ENABLE,0);
	encode_byte(bpp,PLL_SQUARE,0);	
      } else if(strcasecmp(str,"square") == 0){
	encode_byte(bpp,PLL_ENABLE,1);
	encode_byte(bpp,PLL_SQUARE,1);	
      } else if(strcasecmp(str,"!square") == 0){	  
	encode_byte(bpp,PLL_SQUARE,0);	
      } else if(strcasecmp(str,"agc") == 0){
	encode_byte(bpp,AGC_ENABLE,1);
      } else if(strcasecmp(str,"!agc") == 0){
	encode_byte(bpp,AGC_ENABLE,0);
      }
    }
    break;
  default:
    beep();
    break;
  } // switch
  return 0;
}

static void process_mouse(struct channel *channel,uint8_t **bpp){
  // Process mouse events
  // Need to handle the wheel as equivalent to up/down arrows
  MEVENT mouse_event;

  getmouse(&mouse_event);
  int mx,my;
  mx = mouse_event.x;
  my = mouse_event.y;
  mouse_event.y = mouse_event.x = mouse_event.z = 0;
  if(mx != 0 && my != 0){
#if 0
    wprintw(debug," (%d %d)",mx,my);
#endif
    if(Tuning_win && wmouse_trafo(Tuning_win,&my,&mx,false)){
      // Tuning window
      Control.item = my-1;
      Control.step = 24-mx;
      if(Control.step < 0)
	Control.step++;
      if(Control.step > 3)
	Control.step--;
      if(Control.step > 6)
	Control.step--;
      if(Control.step > 9)	
	Control.step--;
      // Clamp to range
      if(Control.step < -3)
	Control.step = -3;
      if(Control.step > 9)
	Control.step = 9;
	  
    } else if(Presets_win && wmouse_trafo(Presets_win,&my,&mx,false)){
      // In the presets window?
      my--;
      if(my >= 0 && my < iniparser_getnsec(Pdict)){
	char const *preset = iniparser_getsecname(Pdict,my);
	encode_string(bpp,PRESET,preset,strlen(preset));
      }
	  
    } else if(Options_win && wmouse_trafo(Options_win,&my,&mx,false)){
      // In the options window
      if(channel->demod_type == WFM_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 2:
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
	}
      } else if(channel->demod_type == FM_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,THRESH_EXTEND,0);
	  break;
	case 2:
	  encode_int(bpp,THRESH_EXTEND,1);	  
	  break;
	}
      } else if(channel->demod_type == LINEAR_DEMOD){
	switch(my){
	case 1:
	  encode_int(bpp,ENVELOPE,1);
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 2:
	  encode_int(bpp,ENVELOPE,1);
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
 	case 3:
	  encode_int(bpp,ENVELOPE,0);
	  encode_int(bpp,OUTPUT_CHANNELS,1);
	  break;
	case 4:
	  encode_int(bpp,ENVELOPE,0);
	  encode_int(bpp,OUTPUT_CHANNELS,2);
	  break;
	case 5:
	  encode_int(bpp,PLL_ENABLE,0);
	  break;
	case 6:
	  encode_int(bpp,PLL_ENABLE,1);
	  encode_int(bpp,PLL_SQUARE,0);
	  break;
	case 7:	  
	  encode_int(bpp,PLL_ENABLE,1);
	  encode_int(bpp,PLL_SQUARE,1);
	  break;
	case 8:
	  encode_int(bpp,AGC_ENABLE,0);
	  break;
	case 9:
	  encode_int(bpp,AGC_ENABLE,1);
	  break;
	}
      }
    }
  } // end of mouse processing
}

// Initialize a new, unused channel instance where fields start non-zero
static int init_demod(struct channel *channel){
  memset(channel,0,sizeof(*channel));
  channel->tune.second_LO = NAN;
  channel->tune.freq = channel->tune.shift = NAN;
  channel->filter.min_IF = channel->filter.max_IF = channel->filter.kaiser_beta = NAN;
  channel->output.headroom = channel->linear.hangtime = channel->linear.recovery_rate = NAN;
  channel->sig.bb_power = channel->sig.snr = channel->sig.foffset = NAN;
  channel->fm.pdeviation = channel->linear.cphase = channel->linear.lock_timer = NAN;
  channel->output.gain = NAN;
  channel->tp1 = channel->tp2 = NAN;

  channel->output.data_fd = channel->output.rtcp_fd = -1;
  return 0;
}

// Is response for us (1), or for somebody else (-1)?
static int for_us(struct channel *channel,uint8_t const *buffer,int length,uint32_t ssrc){
  uint8_t const *cp = buffer;
  
  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // end of list, no length
    
    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan
    
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case OUTPUT_SSRC: // If we've specified a SSRC, it must match it
      if(ssrc != 0){
	if(decode_int32(cp,optlen) == ssrc)
	  return 1; // For us
	return -1; // For someone else
      }
      break;
    default:
      break; // Ignore on this pass
    }
    cp += optlen;
  }
 done:;
  return 0; // not specified
}

// Decode incoming status message from the radio program, convert and fill in fields in local channel structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
// Note that we use some fields in channel differently than in the radio (e.g., dB vs ratios)
static int decode_radio_status(struct channel *channel,uint8_t const *buffer,int length){
  uint8_t const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list
    
    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan
    switch(type){
    case EOL:
      break;
    case CMD_CNT:
      channel->commands = decode_int32(cp,optlen);
      break;
    case DESCRIPTION:
      FREE(Frontend.description);
      Frontend.description = decode_string(cp,optlen);
      break;
    case GPS_TIME:
      Frontend.timestamp = decode_int64(cp,optlen);
      break;
    case INPUT_SAMPRATE:
      Frontend.samprate = decode_int(cp,optlen);
      break;
    case INPUT_SAMPLES:
      Frontend.samples = decode_int64(cp,optlen);
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      decode_socket(&channel->output.data_source_address,cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&channel->output.data_dest_address,cp,optlen);
      break;
    case OUTPUT_SSRC:
      channel->output.rtp.ssrc = decode_int32(cp,optlen);
      break;
    case OUTPUT_TTL:
      Mcast_ttl = decode_int8(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      channel->output.samprate = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      channel->output.rtp.packets = decode_int64(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      Metadata_packets = decode_int64(cp,optlen);      
      break;
    case FILTER_BLOCKSIZE:
      Frontend.L = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      Frontend.M = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      channel->filter.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      channel->filter.max_IF = decode_float(cp,optlen);
      break;
    case FE_LOW_EDGE:
      Frontend.min_IF = decode_float(cp,optlen);
      break;
    case FE_HIGH_EDGE:
      Frontend.max_IF = decode_float(cp,optlen);
      break;
    case FE_ISREAL:
      Frontend.isreal = decode_int8(cp,optlen) ? true: false;
      break;
    case AD_BITS_PER_SAMPLE:
      Frontend.bitspersample = decode_int(cp,optlen);
      break;
    case IF_GAIN:
      Frontend.if_gain = decode_int8(cp,optlen);
      break;
    case LNA_GAIN:
      Frontend.lna_gain = decode_int8(cp,optlen);
      break;
    case MIXER_GAIN:
      Frontend.mixer_gain = decode_int8(cp,optlen);
      break;
    case KAISER_BETA:
      channel->filter.kaiser_beta = decode_float(cp,optlen);
      break;
    case FILTER_DROPS:
      Block_drops = decode_int(cp,optlen);
      break;
    case IF_POWER:
      Frontend.if_power = dB2power(decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      channel->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
      break;
    case NOISE_DENSITY:
      channel->sig.n0 = dB2power(decode_float(cp,optlen));
      break;
    case DEMOD_SNR:
      channel->sig.snr = dB2power(decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      channel->sig.foffset = decode_float(cp,optlen);
      break;
    case PEAK_DEVIATION:
      channel->fm.pdeviation = decode_float(cp,optlen);
      break;
    case PLL_LOCK:
      channel->linear.pll_lock = decode_int8(cp,optlen);
      break;
    case PLL_BW:
      channel->linear.loop_bw = decode_float(cp,optlen);
      break;
    case PLL_SQUARE:
      channel->linear.square = decode_int8(cp,optlen);
      break;
    case PLL_PHASE:
      channel->linear.cphase = decode_float(cp,optlen);
      break;
    case ENVELOPE:
      channel->linear.env = decode_int8(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      channel->output.energy = dB2power(decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      channel->output.samples = decode_int64(cp,optlen);
      break;
    case COMMAND_TAG:
      channel->command_tag = decode_int32(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      channel->tune.freq = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY:
      channel->tune.second_LO = decode_double(cp,optlen);
      break;
    case SHIFT_FREQUENCY:
      channel->tune.shift = decode_double(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      Frontend.frequency = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY:
      channel->tune.doppler = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY_RATE:
      channel->tune.doppler_rate = decode_double(cp,optlen);
      break;
    case DEMOD_TYPE:
      channel->demod_type = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      channel->output.channels = decode_int(cp,optlen);
      break;
    case INDEPENDENT_SIDEBAND:
      channel->filter.isb = decode_int8(cp,optlen);
      break;
    case THRESH_EXTEND:
      channel->fm.threshold = decode_int8(cp,optlen);
      break;
    case PLL_ENABLE:
      channel->linear.pll = decode_int8(cp,optlen);
      break;
    case GAIN:              // dB to voltage
      channel->output.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      channel->linear.agc = decode_int8(cp,optlen);
      break;
    case HEADROOM:          // db to voltage
      channel->output.headroom = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:      // s to samples
      channel->linear.hangtime = decode_float(cp,optlen);
      break;
    case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
      channel->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:   // dB to voltage
      channel->linear.threshold = dB2voltage(decode_float(cp,optlen));
      break;
    case TP1: // Test point
      channel->tp1 = decode_float(cp,optlen);
      break;
    case TP2:
      channel->tp2 = decode_float(cp,optlen);
      break;
    case SQUELCH_OPEN:
      channel->squelch_open = dB2power(decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      channel->squelch_close = dB2power(decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      channel->deemph.gain = decode_float(cp,optlen);
      break;
    case DEEMPH_TC:
      channel->deemph.rate = 1e6*decode_float(cp,optlen);
      break;
    case PL_TONE:
      channel->fm.tone_freq = decode_float(cp,optlen);
      break;
    case PL_DEVIATION:
      channel->fm.tone_deviation = decode_float(cp,optlen);
      break;
    case NONCOHERENT_BIN_BW:
      channel->spectrum.bin_bw = decode_float(cp,optlen);
      break;
    case BIN_COUNT:
      channel->spectrum.bin_count = decode_int(cp,optlen);
      break;
    case BIN_DATA:
      break;
    case RF_GAIN:
      Frontend.rf_gain = decode_float(cp,optlen);
      break;
    case RF_ATTEN:
      Frontend.rf_atten = decode_float(cp,optlen);
      break;
    case BLOCKS_SINCE_POLL:
      channel->blocks_since_poll = decode_int64(cp,optlen);
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	strlcpy(channel->preset,p,sizeof(channel->preset));
	FREE(p);
      }
    default: // ignore others
      break;
    }
    cp += optlen;
  }
  return 0;
}

static void display_tuning(WINDOW *w,struct channel const *channel){
  // Tuning control window - these can be adjusted by the user
  // using the keyboard or tuning knob, so be careful with formatting
  if(w == NULL)
    return;

  wmove(w,0,0);
  wclrtobot(w);
  int row = 1;
  int col = 1;
  if(Frequency_lock)
    wattron(w,A_UNDERLINE); // Underscore means the frequency is locked
  pprintw(w,row++,col,"Carrier","%'.3f",channel->tune.freq); // RF carrier frequency
  
  // second LO frequency is negative of IF, i.e., a signal at +48 kHz
  // needs a second LO frequency of -48 kHz to bring it to zero
  if(Frontend.lock)
    wattron(w,A_UNDERLINE);
  pprintw(w,row++,col,"First LO","%'.3f",Frontend.frequency);
  wattroff(w,A_UNDERLINE);

  // Wink IF display if out of front end's range
  wattroff(w,A_UNDERLINE);
  if(-channel->tune.second_LO + channel->filter.min_IF < Frontend.min_IF)
    wattron(w,A_BLINK);
  if(-channel->tune.second_LO + channel->filter.max_IF > Frontend.max_IF)
    wattron(w,A_BLINK);
  
  pprintw(w,row++,col,"IF","%'.3f",-channel->tune.second_LO);
  wattroff(w,A_BLINK);
  
  pprintw(w,row++,col,"Filter low","%'+.0f",channel->filter.min_IF);
  pprintw(w,row++,col,"Filter high","%'+.0f",channel->filter.max_IF);
  
  if(!isnan(channel->tune.shift))
    pprintw(w,row++,col,"Shift","%'+.3f",channel->tune.shift);
  
  pprintw(w,row++,col,"FE filter low","%'+.0f",Frontend.min_IF);
  pprintw(w,row++,col,"FE filter high","%'+.0f",Frontend.max_IF);

  // Doppler info displayed only if active
  double const dopp = channel->tune.doppler;
  if(dopp != 0){
    pprintw(w,row++,col,"Doppler","%'.3f",dopp);
    pprintw(w,row++,col,"Dop Rate, Hz/s","%'.3f",channel->tune.doppler_rate);
  }
  row++; // Blank line between frequency & band info
  display_info(w,row,col,channel); // moved to bottom of tuning window
  box(w,0,0);
  mvwaddstr(w,0,1,"Tuning Hz");
  // Highlight cursor for tuning step
  // A little messy because of the commas in the frequencies
  // They come from the ' option in the printf formats
  // tunestep is the log10 of the digit position (0 = units)
  int hcol;
  if(Control.step >= 0){
    hcol = Control.step + Control.step/3;
    hcol = -hcol;
  } else {
    hcol = -Control.step;
    hcol = 1 + hcol + (hcol-1)/3; // 1 for the decimal point, and extras if there were commas in more than 3 places
  }
  int mod_x,mod_y;
  mod_y = Control.item + 1;
  mod_x = 24 + hcol; // units in column 24
  mvwchgat(w,mod_y,mod_x,1,A_STANDOUT,0,NULL);
  wnoutrefresh(w);
}

// Imbed in tuning window
static void display_info(WINDOW *w,int row,int col,struct channel const *channel){
  if(w == NULL)
    return;

  struct bandplan const *bp_low,*bp_high;
  bp_low = lookup_frequency(channel->tune.freq + channel->filter.min_IF);
  bp_high = lookup_frequency(channel->tune.freq + channel->filter.max_IF);
  // Make sure entire receiver passband is in the band
  if(bp_low != NULL && bp_high != NULL){
    if(bp_low)
      mvwaddstr(w,row++,col,bp_low->description);
    if(bp_high && bp_high != bp_low)
    mvwaddstr(w,row++,col,bp_high->description);    
  }
}
static void display_filtering(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  // Filter window values
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  pprintw(w,row++,col,"Fs in","%'d Hz",Frontend.samprate); // Nominal
  pprintw(w,row++,col,"Fs out","%'d Hz",channel->output.samprate);
  
  pprintw(w,row++,col,"Block Time","%'.1f ms",Blocktime);
  
  int64_t const N = Frontend.L + Frontend.M - 1;
  
  pprintw(w,row++,col,"FFT in","%'lld %c ",N,Frontend.isreal ? 'r' : 'c');
  
  if(Frontend.samprate)
    pprintw(w,row++,col,"FFT out","%'lld c ",N * channel->output.samprate / Frontend.samprate);
  
  int overlap = 1 + Frontend.L / (Frontend.M - 1); // recreate original overlap parameter
  pprintw(w,row++,col,"Overlap","1/%d   ",overlap);
  pprintw(w,row++,col,"Bin step","%'.3f Hz",(float)Frontend.samprate / N); // frequency between FFT bin centers
  pprintw(w,row++,col,"Bin width","%'.3f Hz",1000.0/Blocktime); // Just the block rate
  
  float const beta = channel->filter.kaiser_beta;
  pprintw(w,row++,col,"Kaiser beta","%'.1f   ",beta);
  
  
#if 0 // Doesn't really give accurate results
  // Play with Kaiser window values
  // Formulas taken from On the Use of the I0-sinh Window for Spectrum Analysis
  // James F Kaiser & Ronald W Schafer
  // ieee transaction on accoustics feb 1980
  // Eq (7) attenuation of first sidelobe
  float const cos_theta_r = 0.217324; // cosine of the first solution of tan(x) = x [really]
  float atten = 20 * log10(sinh(beta) / (cos_theta_r * beta));
  pprintw(w,row++,col,"Sidelobes","%'.1f dB",-atten);
  
  float firstnull = (1/(2*M_PI)) * sqrtf(M_PI * M_PI + beta*beta); // Eqn (3) to first null
  float const transition = (2.0 / M_PI) * sqrtf(M_PI*M_PI + beta * beta);
  pprintw(w,row++,col,"first null","%'.1f Hz",0.5 * transition * Frontend.samprate / (Frontend.M-1)); // Not N, apparently
  //  pprintw(w,row++,col,"first null","%'.1f Hz",firstnull * 1000. / Blocktime);
#endif

  pprintw(w,row++,col,"Drops","%'llu   ",Block_drops);
  
  box(w,0,0);
  mvwaddstr(w,0,1,"Filtering");
  
  wnoutrefresh(w);
}
// Signal data window
static void display_sig(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  float const noise_bandwidth = fabsf(channel->filter.max_IF - channel->filter.min_IF);
  float sig_power = channel->sig.bb_power - noise_bandwidth * channel->sig.n0;
  if(sig_power < 0)
    sig_power = 0; // can happen due to smoothing

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);

  if(Frontend.lna_gain != 0 || Frontend.mixer_gain != 0 || Frontend.if_gain != 0)
    pprintw(w,row++,col,"A Gain","%02d+%02d+%02d dB   ",Frontend.lna_gain,
	    Frontend.mixer_gain,
	    Frontend.if_gain);

  pprintw(w,row++,col,"Input","%.1f dBFS ",power2dB(Frontend.if_power));
  pprintw(w,row++,col,"FE Gain","%.1f dB   ",Frontend.rf_atten + Frontend.rf_gain);
  pprintw(w,row++,col,"Baseband","%.1f dB   ",power2dB(channel->sig.bb_power));
  pprintw(w,row++,col,"N0","%.1f dB/Hz",power2dB(channel->sig.n0));
  
  // Derived numbers
  float sn0 = sig_power/channel->sig.n0;
  pprintw(w,row++,col,"S/N0","%.1f dBHz ",power2dB(sn0));
  pprintw(w,row++,col,"NBW","%.1f dBHz ",power2dB(noise_bandwidth));
  pprintw(w,row++,col,"SNR","%.1f dB   ",power2dB(sn0/noise_bandwidth));

  pprintw(w,row++,col,"Gain","%.1lf dB   ",voltage2dB(channel->output.gain));
  pprintw(w,row++,col,"Output","%.1lf dBFS ",power2dB(channel->output.energy)); // actually level; sender does averaging
  pprintw(w,row++,col,"Headroom","%.1f dBFS ",voltage2dB(channel->output.headroom));
  box(w,0,0);
  mvwaddstr(w,0,1,"Signal");
  wnoutrefresh(w);
}
static void display_demodulator(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  // Demodulator info
  wmove(w,0,0);
  wclrtobot(w);
  int row = 1;
  int col = 1;

  // Display only if used by current mode
  switch(channel->demod_type){
  case FM_DEMOD:
  case WFM_DEMOD:
    pprintw(w,row++,col,"Input SNR","%.1f dB",power2dB(channel->sig.snr));
    pprintw(w,row++,col,"Squelch open","%.1f dB",power2dB(channel->squelch_open));
    pprintw(w,row++,col,"Squelch close","%.1f dB",power2dB(channel->squelch_close));    
    pprintw(w,row++,col,"Offset","%'+.3f Hz",channel->sig.foffset);
    pprintw(w,row++,col,"Deviation","%.1f Hz",channel->fm.pdeviation);
    if(!isnan(channel->fm.tone_freq) && channel->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone squelch","%.1f Hz",channel->fm.tone_freq);
    if(!isnan(channel->fm.tone_deviation) && !isnan(channel->fm.tone_freq) && channel->fm.tone_freq != 0)
      pprintw(w,row++,col,"Tone dev","%.1f Hz",channel->fm.tone_deviation);
    if(channel->deemph.rate != 0){
      pprintw(w,row++,col,"Deemph tc","%.1f us",channel->deemph.rate);
      pprintw(w,row++,col,"Deemph gain","%.1f dB",channel->deemph.gain);
    }
    break;
  case LINEAR_DEMOD:
    if(!isnan(channel->linear.threshold) && channel->linear.threshold > 0)
      pprintw(w,row++,col,"Threshold","%.1f dB  ",voltage2dB(channel->linear.threshold));
    if(!isnan(channel->linear.recovery_rate) && channel->linear.recovery_rate > 0)
      pprintw(w,row++,col,"Recovery rate","%.1f dB/s",voltage2dB(channel->linear.recovery_rate));
    if(!isnan(channel->linear.hangtime))
      pprintw(w,row++,col,"Hang time","%.1f s   ",channel->linear.hangtime);
    
    if(channel->linear.pll){
      pprintw(w,row++,col,"PLL BW","%.1f Hz  ",channel->linear.loop_bw);
      pprintw(w,row++,col,"PLL SNR","%.1f dB  ",power2dB(channel->sig.snr));
      pprintw(w,row++,col,"Offset","%'+.3f Hz  ",channel->sig.foffset);
      pprintw(w,row++,col,"PLL Phase","%+.1f deg ",channel->linear.cphase*DEGPRA);
      pprintw(w,row++,col,"PLL Lock","%s     ",channel->linear.pll_lock ? "Yes" : "No");
      pprintw(w,row++,col,"Squelch open","%.1f dB  ",power2dB(channel->squelch_open));
      pprintw(w,row++,col,"Squelch close","%.1f dB  ",power2dB(channel->squelch_close));    
    }
    break;
  case SPECT_DEMOD:
    pprintw(w,row++,col,"Bin width","%.0f Hz",channel->spectrum.bin_bw);
    pprintw(w,row++,col,"Bins","%d   ",channel->spectrum.bin_count);
    if(channel->spectrum.bin_data != NULL)
      pprintw(w,row++,col,"Bin 0","%.1f   ",channel->spectrum.bin_data[0]);
    break;
  }

  if(!isnan(channel->tp1))
    pprintw(w,row++,col,"TP1","%+g",channel->tp1);

  if(!isnan(channel->tp2))
    pprintw(w,row++,col,"TP2","%+g",channel->tp2);

  box(w,0,0);
  mvwprintw(w,0,1,"%s demodulator",demod_name_from_type(channel->demod_type));
  wnoutrefresh(w);
}

static void display_output(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  {
    char tbuf[100];
    pprintw(w,row++,col,"","%s",format_gpstime(tbuf,sizeof(tbuf),Frontend.timestamp));
  }
  pprintw(w,row++,col,"Samples","%'llu",Frontend.samples);
  pprintw(w,row++,col,"Update interval","%'.2f sec",Refresh_rate);
  mvwhline(w,row,0,0,1000);
  mvwaddstr(w,row++,1,"Status");
  pprintw(w,row++,col,"","%s->%s",formatsock(&Metadata_source_address),
	   formatsock(&Metadata_dest_address));
  pprintw(w,row++,col,"Status pkts","%'llu",Metadata_packets);
  pprintw(w,row++,col,"Control pkts","%'llu",channel->commands);
  pprintw(w,row++,col,"Blocks since last poll","%'llu",channel->blocks_since_poll);

  mvwhline(w,row,0,0,1000);
  mvwaddstr(w,row++,1,"Data");  

  pprintw(w,row++,col,"","%s->%s",formatsock(&channel->output.data_source_address),
	  formatsock(&channel->output.data_dest_address));
  
  pprintw(w,row++,col,"SSRC","%'u",channel->output.rtp.ssrc);
  pprintw(w,row++,col,"Packets","%'llu",(long long unsigned)channel->output.rtp.packets);
  
  
  box(w,0,0);
  mvwaddstr(w,0,1,Frontend.description);
  wnoutrefresh(w);
}

static void display_options(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  // Demodulator options, can be set with mouse
  int row = 1;
  int col = 1;
  wmove(w,row,col);
  wclrtobot(w);
  if(channel->demod_type == FM_DEMOD){ // FM from status.h
    if(!channel->fm.threshold)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Th Ext off");
    wattroff(w,A_UNDERLINE);

    if(channel->fm.threshold)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Th Ext on");
    wattroff(w,A_UNDERLINE);

  } else if(channel->demod_type == WFM_DEMOD){
    // Mono/stereo are only options
    if(channel->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Mono");    
    wattroff(w,A_UNDERLINE);
    
    if(channel->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Stereo");    
    wattroff(w,A_UNDERLINE);
  } else if(channel->demod_type == LINEAR_DEMOD){
    if(channel->linear.env && channel->output.channels == 1)
      wattron(w,A_UNDERLINE);	
    mvwaddstr(w,row++,col,"Envelope");
    wattroff(w,A_UNDERLINE);
    
    if(channel->linear.env && channel->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear+Envelope");
    wattroff(w,A_UNDERLINE);
    
    if(!channel->linear.env && channel->output.channels == 1)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"Linear");
    wattroff(w,A_UNDERLINE);
    
    if(!channel->linear.env && channel->output.channels == 2)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"I/Q");    
    wattroff(w,A_UNDERLINE);
    
    if(!channel->linear.pll)
      wattron(w,A_UNDERLINE);      
    mvwaddstr(w,row++,col,"PLL Off");
    wattroff(w,A_UNDERLINE);
    
    if(channel->linear.pll && !channel->linear.square)
      wattron(w,A_UNDERLINE);      
    mvwaddstr(w,row++,col,"PLL On");
    wattroff(w,A_UNDERLINE);
    
    if(channel->linear.pll && channel->linear.square)
      wattron(w,A_UNDERLINE);            
    mvwaddstr(w,row++,col,"PLL Square");
    wattroff(w,A_UNDERLINE);
    
    if(!channel->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC Off");
    wattroff(w,A_UNDERLINE);     
    if(channel->linear.agc)
      wattron(w,A_UNDERLINE);
    mvwaddstr(w,row++,col,"AGC On");
    wattroff(w,A_UNDERLINE);
    
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Options");
  wnoutrefresh(w);
}

static void display_presets(WINDOW *w,struct channel const *channel){
  if(w == NULL)
    return;

  // Display list of presets defined in presets.conf
  // Can be selected with mouse
  int row = 1;
  int col = 1;
  int npresets = iniparser_getnsec(Pdict);

  for(int i=0;i<npresets;i++){
    char const * const cp = iniparser_getsecname(Pdict,i);
    if(strncasecmp(cp,channel->preset,sizeof(channel->preset)) == 0)
      wattron(w,A_UNDERLINE);
    else
      wattroff(w,A_UNDERLINE);      
    mvwaddstr(w,row++,col,cp);
  }
  box(w,0,0);
  mvwaddstr(w,0,1,"Presets");
  wnoutrefresh(w);
}

// Like mvwprintw, but right justify the formatted output on the line and overlay with
// a left-justified label
static int pprintw(WINDOW *w,int y, int x, char const *label, char const *fmt,...){
  int maxy __attribute__((unused)); // needed for getmaxyx
  int maxx;
  getmaxyx(w,maxy,maxx);

  // Format the variables
  va_list ap;
  va_start(ap,fmt);
  char result[maxx+1];
  int const r = vsnprintf(result,sizeof(result)-1,fmt,ap);
  va_end(ap);
  
  if(r == -1)
    return -1;

  result[sizeof(result)-1] = '\0'; // Ensure it's terminated

  // vsnprintf returns character count that *would* be written without limits
  // We want the actual length
  int const len = strlen(result);

  int vstart = maxx - 2 - len; // Allow an extra space for right side of box
  if(vstart < 0)
    vstart = 0; // Too long, truncate on right

  wmove(w,y,x);
  wclrtoeol(w);
  mvwaddstr(w,y,x+vstart,result);
  mvwaddstr(w,y,x,label);
  return 0;
}
