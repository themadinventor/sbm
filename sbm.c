/*

    System Boot Manager v0.2
    (c) Fredrik Ahlberg, 2008 <fredrik@z80.se>

    This is a utility to control the iROM boot monitor
    in the i.MX21 series ARM CPUs.
    It is using the protocol defined by the Freescale
    Reference Manual, and by using portmon to analyze
    the Freescale HAB Toolkit.
    This code should be quite bug-free (at least GCC
    is as silent as a -Wall :-), but the iROM protocol is
    not very well defined, and some response values (those
    ugly magic numbers around in the code) may differ on
    your device. Feel free to edit those.

    Commands:
    sync                  Ping iROM.
    set ADDR SIZE VALUE   Write to a register.
    download FILE ADDR    Write a binary image to RAM at ADDR.
    run [ADDR]            Let the i.MX run the code at ADDR.
                          If ADDR is not specified, the
			  address of the first byte of the last
			  downloaded image is used.
    baud [BAUD]           Change bootloader baud rate, to speed
                          up image downloading. If BAUD is not
			  specified, it'll be set to sbm's default
			  (which is 921.6 kbaud, eight times the
			  start-up value, maximum of the CP2101)
    setup                 Initialize the SDRAM-controller using
                          a default setup routine.
    terminal [BAUD]       Starts an interactive terminal on the
                          still open port. Nifty thing to get a
			  shell after booting linux on your i.MX.
			  It'll redirect all kinds of escape
			  sequences transparently to your terminal.
			  However, sending ^C does not currently work.


    Typical usage:
    sbm setup download test.bin 0xc0000000 run

    Install:
    Just compile with "gcc -o sbm sbm.c" and copy to
    /usr/local/bin, or run in the current directory using
    "./sbm".
    
    NOTE: Please uncomment the "#define IS_BIG_ENDIAN"
    when compiling for a big endian machine (duh)
    such as a SPARC.
    My SPARC machines does not have any USB hosts, (and my
    i.MX board currently has a USB-to-serial chip tied to UART1)
    so I have not tested this code on them, but, "it should work(tm)".


    080927: First version.

    081102: Added baud rate setting and interactive terminal.

*/

#define DEFAULT_PORT "/dev/ttyUSB0"
#define DEFAULT_HIGHSPEED 921600
#define DEFAULT_TERMBAUD  230400
#define CHUNK_SIZE 4096
//#define IS_BIG_ENDIAN

/* === Only h4xx0rz may edit below this line ======================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>

#define IMX_SYNC 0x0505
#define IMX_SREG 0x0202
#define IMX_DWNL 0x0404

#ifdef IS_BIG_ENDIAN
#define SWAPDWORD(d) (d)
#else
#define SWAPDWORD(d) ((((d)&0xff)<<24)|(((d)&0xff00)<<8)|(((d)&0xff0000)>>8)|(((d)&0xff000000)>>24))
#endif

struct imx_packet {
  unsigned short header;
  unsigned int   address;
  unsigned char  type;
  unsigned int   length;
  unsigned int   data;
  unsigned char  end;
} __attribute__((__packed__));

// This one really makes me wanna cry. But, to receive asynchronously
// using the SIGIO, we've got to define it globally. Ugly as hell.
static int port;

int set_baud(int fd, unsigned int baud)
{
  struct termios tty;
  tcgetattr(fd,&tty);
  cfsetspeed(&tty,baud);
  tcflush(fd,TCIFLUSH);
  tcsetattr(fd,TCSANOW,&tty);
}

int open_port(char *port)
{
  int fd;
  
  fd = open(port,O_RDWR|O_NOCTTY);
  if(fd == -1)
    {
      fprintf(stderr,"open_port: Unable to open port %s: ",port);
      perror(NULL);
      return -1;
    }
  else
    fcntl(fd, F_SETFL, 0);
  
  struct termios tty;
  bzero(&tty, sizeof(tty)); 
  tty.c_cflag |= B115200|CS8|CLOCAL|CREAD;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1;
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSANOW, &tty);

  return fd;
}

int imx_sync(int fd)
{
  printf("sbm: Synchronizing...");
  fflush(stdout);

  struct imx_packet pckt;  
  memset(&pckt,0,sizeof(pckt));
  pckt.header = IMX_SYNC;
  write(fd,&pckt,sizeof(pckt));

  unsigned int response;
  if(read(fd,&response,4) == 4)
    {
      if(response == 0xf0f0f0f0)
	{
	  printf("ok\n");
	  return 0;
	}
      else
	{
	  printf("failed (illegal response 0x%08x)\n",response);
	  return -1;
	}
    }
  else
    {
      printf("failed (timeout)\n");
      return -1;
    }
}

int imx_set_register(int fd, unsigned int addr, unsigned int len, unsigned int value, int ignore)
{
  printf("sbm: Write 0x%08x (%d) to 0x%08x...",value,len,addr);
  fflush(stdout);

  struct imx_packet pckt;
  
  pckt.header = IMX_SREG;
  pckt.address = SWAPDWORD(addr);
  pckt.type = len;

  if(len != 8 && len != 16 && len != 32)
    {
      printf("failed\nsbm: Illegal register size!\n");
      return -1;
    }

  pckt.length = 0;

  pckt.data = SWAPDWORD(value);
  pckt.end = 0;

  write(fd,&pckt,sizeof(pckt));

  fflush(stdout);

  unsigned int response_b, response_c;
  if(read(fd,&response_b,4) != 4 || read(fd,&response_c,4) != 4)
    {
      if(ignore)
	{
	  printf("<ignored>\n");  // Yet another ugly hack.
	  return 0;
	}
      printf("failed (timeout)\n");
      return -1;
    }

  if(response_b == 0x56787856 && response_c == 0x128a8a12)
    {
      printf("ok\n");
      return 0;
    }
  else
    {
      if(ignore)
	{
	  printf("<ignored>\n");
	  return 0;
	}
      printf("failed (illegal response 0x%08x:0x%08x)\n",response_b,response_c);
      return -1;
    }
}

int imx_set_baud(int fd, unsigned int baud)
{
  printf("sbm: Changing baud rate to %d...\n",baud);

  imx_set_register(fd,0x1000A0A4,32,(baud/100)-1,0);
  imx_set_register(fd,0x1000A0A8,32,(10000)-1,1);
  set_baud(fd,baud);

  printf("sbm: Baud rate change done\n");
  return 0;
}

int imx_setup_system(int fd)
{
  printf("sbm: Initializing SDRAM\n");

  int i;

  // Those are the same as in imx21_meminit.txt

  imx_set_register(fd, 0x10000000, 32, 0x00040304, 0);
  imx_set_register(fd, 0x10020000, 32, 0x00000000, 0);
  imx_set_register(fd, 0x10000004, 32, 0xfffbfcfb, 0);
  imx_set_register(fd, 0x10020004, 32, 0xffffffff, 0);
  imx_set_register(fd, 0xdf001008, 32, 0x00002000, 0);
  imx_set_register(fd, 0xdf00100c, 32, 0x11118501, 0);
  imx_set_register(fd, 0x10015520, 32, 0x00000000, 0);
  imx_set_register(fd, 0x10015538, 32, 0x00000000, 0);
  imx_set_register(fd, 0x1003f300, 32, 0x00123456, 0);
  imx_set_register(fd, 0xdf000000, 32, 0x92129399, 0);
  imx_set_register(fd, 0xc0200000, 32, 0x00000000, 0);
  imx_set_register(fd, 0xdf000000, 32, 0xa2120300, 0);

  for(i=0; i<8; i++)
    imx_set_register(fd, 0xc0000000, 32, 0x00000000, 0);

  imx_set_register(fd, 0xdf000000, 32, 0xb2120300, 0);
  imx_set_register(fd, 0xc0119800, 32, 0x00000000, 0);
  imx_set_register(fd, 0xdf000000, 32, 0x8212f339, 0);

  printf("sbm: SDRAM Initialized\n");

  return 0;
}

int imx_download(int fd, char *file, unsigned int addr)
{
  FILE *bin;
  struct imx_packet pckt;
  unsigned int i,j,size,to_write;

  bin = fopen(file,"rb");
  if(!bin)
    {
      perror("imx_download: Unable to open binary image");
      printf("sbm: Failed downloading binary image %s\n",file);
      return -1;
    }

  fseek(bin,0,SEEK_END);
  size = ftell(bin);
  fseek(bin,0,SEEK_SET);

  pckt.header = IMX_DWNL;
  pckt.type = 0;
  pckt.data = 0;
  pckt.end = 0;

  printf("sbm: Downloading %s to 0x%08x (%d bytes):\n",file,addr,size);

  for(i=0; i<size;)
    {
      to_write = size - i;
      if(to_write > CHUNK_SIZE)
	to_write = CHUNK_SIZE;

      pckt.address = SWAPDWORD(addr+i);
      pckt.length = SWAPDWORD(to_write);

      write(fd,&pckt,sizeof(pckt));

      unsigned int response_b;
      if(read(fd,&response_b,4) != 4)
	{
	  printf("\nsbm: Failed writing block offset %d size %d at 0x%08x: operation timed out\n",i,to_write,addr+i);
	  fclose(bin);
	  return -1;
	}
      else
	{
	  if(response_b != 0x56787856)
	    {
	      printf("\nsbm: Failed writing block offset %d size %d at 0x%08x: illegal response (0x%08x)\n",i,to_write,addr+i,response_b);
	      fclose(bin);
	      return -1;
	    }
	}

      void *data = malloc(to_write);
      fread(data,1,to_write,bin);
      write(fd,data,to_write);
      free(data);

      i += to_write;

      printf("\rsmb: [");
      
      for(j=0; j<30; j++)
	if(j<i*30/size)
	  printf("=");
	else
	  printf(" ");

      printf("] %d/%d (%d%%)",i,size,i*100/size);
      fflush(stdout);
    }
  
  printf("\nsbm: Download complete\n");
  return 0;
}

int imx_run(int fd, unsigned int addr)
{
  printf("sbm: Calling code at 0x%08x...",addr);

  struct imx_packet pckt;

  pckt.header = IMX_DWNL;
  pckt.address = SWAPDWORD(addr);
  pckt.type = 0;
  pckt.length = 0;
  pckt.data = 0;
  pckt.end = 0xaa;

  write(fd,&pckt,sizeof(pckt));
  
  unsigned int response_b, response_d;
  if(read(fd,&response_b,4) != 4)
    {
      printf("failed (timeout)\n");
      return -1;
    }
  else
    {
      if(response_b == 0x56787856)
	{
	  pckt.header = IMX_SYNC;
	  pckt.address = 0;
	  pckt.type = 0;
	  pckt.length = 0;
	  pckt.data = 0;
	  pckt.end = 0;
	  
	  write(fd,&pckt,sizeof(pckt));

	  if(read(fd,&response_d,4) != 4)
	    {
	      printf("failed (timeout)\n");
	      return -1;
	    }
	  else
	    {
	      if(response_d == 0x08888888 || response_d == 0x88888888)
		{
		  printf("ok\n");
		  return 0;
		}
	      else
		{
		  printf("failed (illegal response 0x%08x)\n",response_d);
		  return -1;
		}
	    }
	}
      else
	{
	  printf("failed (illegal response 0x%08x)\n",response_b);
	  return -1;
	}
    }
}

/*int listen(int fd)
{
  struct termios tty;
  bzero(&tty, sizeof(tty)); 
  tty.c_cflag |= B230400|CS8|CLOCAL|CREAD;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSANOW, &tty);

  char c;

  while(1)
    {
      if(read(fd,&c,1))
	{
	  printf("%c",c);
	  fflush(stdout);
	}
    }
}*/

void term_recv(int status)
{
  unsigned char buf;
  int len;

  while(len = read(port,&buf,1))
    {
      if(len != 1)
        {
          printf("sbm: Read error\n");
          return;
        }

      write(1,&buf,1);
    }
}

int terminal(int fd)
{
  struct termios tio;
  struct sigaction saio;

  printf("sbm: Interactive terminal:\n\n");

  fcntl(fd,F_SETOWN,getpid());
  fcntl(fd,F_SETFL,FASYNC);

  saio.sa_handler = term_recv;
  sigemptyset(&saio.sa_mask);
  saio.sa_flags = 0;
  saio.sa_restorer = NULL;
  sigaction(SIGIO,&saio,NULL);

  tcgetattr(0,&tio);
  tio.c_lflag &= ~(ICANON|ECHO);
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  tcsetattr(0,TCSANOW,&tio);

  char c;
  while(1)
  {
    if(read(0,&c,1)<1)
      continue;

    write(fd,&c,1);
  }

  return 0;
}

int main(int argc, char **argv)
{
  if(argc == 1)
    {
      printf("\nSystem Boot Manager for i.MX21\n"
	     "(c) Fredrik Ahlberg, 2008\n\n"
	     "Usage: %s [-p PORT] COMMAND [PARAMETERS] [COMMAND [PARAMETERS] ...]\n"
	     "   set ADDRESS {8|16|32} VALUE\n"
	     "   download FILE ADDRESS\n"
	     "   run [ADDRESS]\n"
	     "   setup\n"
	     "   baud [BAUD]\n"
	     "   terminal [BAUD]\n"
	     "   sync\n\n"
	     "   Default port is %s, default high speed baudrate is %d, default terminal baudrate is %d\n",
	     argv[0],DEFAULT_PORT,DEFAULT_HIGHSPEED,DEFAULT_TERMBAUD);
      return 0;
    }

  int i = 1, synced = 0;
  char *cmd;
  unsigned int entry = 0;
  char *port_str = DEFAULT_PORT;

  if(!strcmp(argv[1],"-p"))
    {
      if(argc < 3)
	{
	  printf("sbm: Port expected, quitting\n");
	  return -1;
	}
      port_str = argv[2];
      i += 2;
    }

  port = open_port(port_str);
  if(port == -1)
    {
      printf("sbm: Unable to open port, quitting\n");
      return -1;
    }

  while(i<argc)
    {
      cmd = argv[i];

      if(!strcmp(argv[i],"set"))
	{
	  if(argc < i+4)
	    {
	      printf("sbm: Not enough parameters\n");
	      break;
	    }

	  if(!synced)
	    {
	      if(imx_sync(port))
		{
		  printf("sbm: Unable to sync, quitting\n");
		  break;
		}
	      synced = 1;
	    }
	  
	  unsigned int addr, size, value;
	  addr = strtoul(argv[i+1],NULL,0);
	  size = strtoul(argv[i+2],NULL,0);
	  value = strtoul(argv[i+3],NULL,0);

	  if(imx_set_register(port,addr,size,value,0))
	    {
	      printf("sbm: Unable to set register 0x%08x (%d) to 0x%08x, aborting\n",addr,size,value);
	      break;
	    }

	  i += 4;
	  continue;
	}

      if(!strcmp(argv[i],"download"))
	{
	  if(argc < i+3)
	    {
	      printf("sbm: Not enough parameters\n");
	      break;
	    }
	  
	  if(!synced)
	    {
	      if(imx_sync(port))
		{
		  printf("sbm: Unable to sync, quitting\n");
		  break;
		}
	      synced = 1;
	    }

	    unsigned int addr;
	    addr = strtoul(argv[i+2],NULL,0);

	    if(imx_download(port,argv[i+1],addr))
	      {
		printf("sbm: Unable to download %s to 0x%08x, quitting\n",argv[i+1],addr);
		break;
	      }
	    
	    entry = addr;

	    i += 3;
	    continue;
	  }

      if(!strcmp(argv[i],"sync"))
	{
	  if(imx_sync(port))
	    {
	      printf("sbm: Unable to sync, quitting\n");
	      break;
	    }
	  synced = 1;

	  i++;
	  continue;
	}

      if(!strcmp(argv[i],"setup"))
	{
	  if(!synced)
	    {
	      if(imx_sync(port))
		{
		  printf("sbm: Unable to sync, quitting\n");
		  break;
		}
	      synced = 1;
	    }

	  if(imx_setup_system(port))
	    {
	      printf("sbm: Unable to setup system, quitting\n");
	      break;
	    }

	  i++;
	  continue;
	}

      if(!strcmp(argv[i],"baud"))
	{
	  if(!synced)
	    {
	      if(imx_sync(port))
		{
		  printf("sbm: Unable to sync, quitting\n");
		  break;
		}
	      synced = 1;
	    }

	  unsigned int baud;

	  if(argc < i+2)
	    baud = DEFAULT_HIGHSPEED;
	  else
	    {
	      baud = strtoul(argv[i+1],NULL,0);
	      if(!baud)
		baud = DEFAULT_HIGHSPEED;
	      else
		i++;
	    }

	  if(imx_set_baud(port,baud))
	    {
	      printf("sbm: Unable to change baud rate.\n");
	      break;
	    }

	  i++;
	  continue;
	}

      if(!strcmp(argv[i],"run"))
	{
	  /*if(!synced)
	    {
	      if(imx_sync(port))
		{
		  printf("sbm: Unable to sync, quitting\n");
		  break;
		}
	      synced = 1;
	      }*/

	  unsigned int addr;

	  if(argc < i+2)
	    addr = entry;
	  else
	    {
	      addr = strtoul(argv[i+1],NULL,0);
	      if(!addr)
		addr = entry;
	      else
		i++;
	    }

	  if(!addr)
	    {
	      printf("sbm: No address specified, quitting\n");
	      break;
	    }

	  if(imx_run(port,addr))
	    {
	      printf("sbm: Unable to run at 0x%08x, quitting\n",addr);
	      break;
	    }

	  i++;
	  continue;
	}

      if(!strcmp(argv[i],"terminal"))
	{

	  unsigned int baud;

	  if(argc < i+2)
	    baud = DEFAULT_TERMBAUD;
	  else
	    {
	      baud = strtoul(argv[i+1],NULL,0);
	      if(!baud)
		baud = DEFAULT_TERMBAUD;
	      else
		i++;
	    }

	  set_baud(port,baud);

	  if(terminal(port))
	    {
	      printf("sbm: Unable to start the interactive terminal, quitting\n");
	      break;
	    }

	  i++;
	  continue;
	}

      printf("sbm: Unknown command %s, quitting\n",argv[i]);
      break;
    }

  close(port);

  return 0;
}
