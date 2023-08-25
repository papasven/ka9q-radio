// $Id: metadump.c,v 1.19 2022/12/29 05:58:17 karn Exp $
// Utility to trace multicast SDR metadata
// Copyright 2018 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>
#include <sysexits.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"

int Status_sock;

const char *App_path;
int Verbose,Dump;
bool Newline;

char Locale[256] = "en_US.UTF-8";
int Mcast_ttl = 5;

int main(int argc,char *argv[]){
  App_path = argv[0];
  int c;

  while((c = getopt(argc,argv,"vn")) != -1){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'n':
      Newline = true;
      break;
    }
  }

  {
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  fprintf(stdout,"Listening to %s\n",argv[optind]);
  struct sockaddr_storage sock;
  char iface[1024];
  iface[0] = '\0';

  resolve_mcast(argv[optind],&sock,DEFAULT_STAT_PORT,iface,sizeof(iface));
  fprintf(stdout,"Interface: %s\n",iface);
  Status_sock = listen_mcast(&sock,iface);
  if(Status_sock < 0){
    fprintf(stdout,"Can't set up multicast input\n");
    exit(EX_IOERR);
  }

  for(;;){
    uint8_t buffer[8192];

    memset(buffer,0,sizeof(buffer));
    struct sockaddr_storage source;
    socklen_t len = sizeof(source);
    int length = recvfrom(Status_sock,buffer,sizeof(buffer),0,(struct sockaddr *)&source,&len);
    if(length <= 0){
      sleep(1);
      continue;
    }
    int64_t now = gps_time_ns();
    
    enum pkt_type const cr = buffer[0]; // Command/response byte
    char temp[1024];
    fprintf(stdout,"%s %s", format_gpstime(temp,sizeof(temp),now), formatsock(&source));
    fprintf(stdout," %s", cr == STATUS ? "STAT" : cr == CMD ? "CMD" : cr == SSRC_LIST ? "SSRC_LIST" : "unknown");
    dump_metadata(buffer+1,length-1,Newline);
    fflush(stdout);
  }
  exit(EX_OK); // can't reach
}


