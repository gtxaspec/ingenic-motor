#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <syslog.h>


#define SV_SOCK_PATH "/dev/md"
#define BUF_SIZE 15

enum motor_status
{
  MOTOR_IS_STOP,
  MOTOR_IS_RUNNING,
};

struct request{
    char command; // d,r,s,p,b,S,i,j (move, reset,set speed,get position, is busy,Status,initial,JSON)
    char type;   // g,h,c,s (absolute,relative,cruise,stop)
    int x;
    int got_x;
    int y;
    int got_y;
};

struct motor_message
{
  int x;
  int y;
  enum motor_status status;
  int speed;
  /* these two members are not standard from the original kernel module */
  unsigned int x_max_steps;
  unsigned int y_max_steps;
};

void JSON_initial(struct motor_message *message)
{
  // return all known parameters in JSON string
  // idea is when client page loads in browser we
  // get current details from camera
  printf("{");
  printf("\"status\":\"%d\"", (*message).status);
  printf(",");
  printf("\"xpos\":\"%d\"", (*message).x);
  printf(",");
  printf("\"ypos\":\"%d\"", (*message).y);
  printf(",");
  printf("\"xmax\":\"%d\"", (*message).x_max_steps);
  printf(",");
  printf("\"ymax\":\"%d\"", (*message).y_max_steps);
  printf(",");
  printf("\"speed\":\"%d\"", (*message).speed);
  printf("}");
}

void JSON_status(struct motor_message *message)
{
  // return xpos,ypos and status in JSON string
  // allows passing straight back to async call from ptzclient.cgi
  // with little effort and ability to track x,y position
  printf("{");
  printf("\"status\":\"%d\"", (*message).status);
  printf(",");
  printf("\"xpos\":\"%d\"", (*message).x);
  printf(",");
  printf("\"ypos\":\"%d\"", (*message).y);
  printf(",");
  printf("\"speed\":\"%d\"", (*message).speed);
  printf("}");
}

void xy_pos(struct motor_message *message)
{
  printf("%d,%d", (*message).x, (*message).y);
}

void show_status(struct motor_message *message)
{
  printf("Max X Steps %d.\n", (*message).x_max_steps);
  printf("Max Y Steps %d.\n", (*message).y_max_steps);
  printf("Status Move: %d.\n", (*message).status);
  printf("X Steps %d.\n", (*message).x);
  printf("Y Steps %d.\n",(*message).y);
  printf("Speed %d.\n", (*message).speed);
}


int main(int argc, char *argv[])
{
  char direction = 's';
  int stepspeed = 900;
  int c;
  int closeready = 0;
  struct request request_message;
  request_message.got_x = 0;
  request_message.got_y = 0;

  openlog ("motors app", LOG_PID, LOG_USER);

  //should open socket here
  struct sockaddr_un addr;

 
  int serverfd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (serverfd == -1) {
      exit(EXIT_FAILURE);
    }
  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SV_SOCK_PATH, sizeof(addr.sun_path) - 1);

  //connect to the socket
  if (connect(serverfd, (struct sockaddr *) &addr,sizeof(struct sockaddr_un)) == -1)
      exit(EXIT_FAILURE);
  

  
 
  while ((c = getopt(argc, argv, "d:s:x:y:jipSrvb")) != -1)
  {
    switch (c)
    {
    case 'd':
      request_message.command = 'd';
      direction = optarg[0];
      break;
    case 's':
      request_message.command = 's';
      if (atoi(optarg) > 900)
      {
        request_message.x = 900;
      }
      else
      {
        request_message.x = atoi(optarg);
      }
      write(serverfd,&request_message,sizeof(struct request));
      break;
    case 'x':
      request_message.x = atoi(optarg);
      request_message.got_x = 1;
      break;
    case 'y':
      request_message.y = atoi(optarg);
      request_message.got_y = 1;
      break;
    case 'j':
      request_message.command = 'j';
      write(serverfd,&request_message,sizeof(struct request));

      struct motor_message status;
      read(serverfd,&status,sizeof(struct motor_message));

      write(serverfd,&closeready,sizeof(closeready));
 	    JSON_status(&status);
      break;
    case 'i':
      // get all initial values
      request_message.command = 'i';
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message initial;
      read(serverfd,&initial,sizeof(struct motor_message));

      write(serverfd,&closeready,sizeof(closeready));
      JSON_initial(&initial);
      break;
    case 'p':
      request_message.command = 'p';
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message pos;
      int load = read(serverfd,&pos,sizeof(struct motor_message));
      syslog(LOG_DEBUG,"%i,%i,%i,%i",pos.x,pos.y,load,errno);
      load = printf("%i,%i,%i,%i",pos.x,pos.y,load,errno);
      syslog(LOG_DEBUG,"printf printed %i bytes and errno is %i",load,errno);
      write(serverfd,&closeready,sizeof(closeready));
      xy_pos(&pos);
      break;
    case 'v':
      //not printing any debug on the app yet,daemon prints on logread
      //debugflag = true;
      break;
    case 'r': // reset
      request_message.command = 'r';
      break;
    case 'S': // status
      request_message.command = 'S';
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message stat;
      read(serverfd,&stat,sizeof(struct motor_message));

      write(serverfd,&closeready,sizeof(closeready));
      show_status(&stat);
      break;
    case 'b': // is moving?
      request_message.command = 'b';
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message busy;
      read(serverfd,&busy,sizeof(struct motor_message));
      write(serverfd,&closeready,sizeof(closeready));
      	if(busy.status == MOTOR_IS_RUNNING){
      		printf("1\n");
      		return(1);
      	}
      	else{
      		printf("0\n");
      		return(0);
      	}
      break;
    default:
      printf("Invalid Argument %c\n", c);
      printf("Usage : %s\n"
             "\t -d Direction step\n"
             "\t -s Speed step (default 900)\n"
             "\t -x X position/step (default 0)\n"
             "\t -y Y position/step (default 0) .\n"
             "\t -r reset to default pos.\n"
             "\t -v verbose mode, prints debugging information while app is running\n"
             "\t -j return json string xpos,ypos,status.\n"
             "\t -i return json string for all camera parameters\n"
             "\t -p return xpos,ypos as a string\n"
             "\t -b prints 1 if motor is (b)usy moving or 0 if is not\n"
             "\t -S show status\n",
             argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  switch (direction)
  {
  case 's': // stop
  	request_message.type = 's';
  	write(serverfd,&request_message,sizeof(struct request));
    break;

  case 'c': // cruise
  	request_message.type = 'c';
  	write(serverfd,&request_message,sizeof(struct request));
    break;

  case 'b': // go back
  	request_message.type = 'b';
  	write(serverfd,&request_message,sizeof(struct request));
    break;

  case 'h': // set position (absolute movement)
  	request_message.type = 'h';
  	write(serverfd,&request_message,sizeof(struct request));
    break;

  case 'g': // move x y (relative movement)
    request_message.type = 'g';
    write(serverfd,&request_message,sizeof(struct request));
    break;

  default:
    printf("Invalid Direction Argument %c\n", direction);
    printf("Usage : %s -d\n"
           "\t s (Stop)\n"
           "\t c (Cruise)\n"
           "\t b (Go to previous position)\n"
           "\t h (Set position X and Y)\n"
           "\t g (Steps X and Y)\n",
           argv[0]);
    exit(EXIT_FAILURE);
  }

  return 0;
}