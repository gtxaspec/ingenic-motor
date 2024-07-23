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
#include <signal.h>

#define SV_SOCK_PATH "/dev/md"
#define BUF_SIZE 15

#define PID_SIZE 32

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
    int speed;  // Add speed to the request structure
    bool speed_supplied; // Track if speed was supplied
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
  printf("\n");
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
  printf("\n");
}

void xy_pos(struct motor_message *message)
{
  printf("%d,%d\n", (*message).x, (*message).y);
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

int check_daemon(char *file_name)
{
    FILE *f;
    long pid;
    char pid_buffer[PID_SIZE];

    f = fopen(file_name, "r");
    if(f == NULL)
        return 0;

    if (fgets(pid_buffer, PID_SIZE, f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);

    if (sscanf(pid_buffer, "%ld", &pid) != 1) {
        return 0;
    }

    if (kill(pid, 0) == 0) {
        return 1;
    }

    return 0;
}

void print_request_message(struct request *req)
{
    printf("Sent message: command=%c, type=%c, x=%d, y=%d, speed=%d, speed_supplied=%d\n",
           req->command, req->type, req->x, req->y, req->speed, req->speed_supplied);
}

void initialize_request_message(struct request *req) {
    memset(req, 0, sizeof(struct request));
    req->command = 'd'; // Default command
    req->type = 's'; // Default type
    req->x = 0;
    req->got_x = 0;
    req->y = 0;
    req->got_y = 0;
    req->speed = 0;
    req->speed_supplied = false;
}

int main(int argc, char *argv[])
{
  char direction = '\0';
  int stepspeed = 900;
  int c;
  char *daemon_pid_file;
  struct request request_message;
  bool verbose = false; // Initialize verbose to false

  initialize_request_message(&request_message);

  //openlog ("motors app", LOG_PID, LOG_USER);
  daemon_pid_file = "/var/run/motors-daemon";
  if (check_daemon(daemon_pid_file) == 0) {
        printf("Motors daemon is NOT running, please start the daemon\n");
        exit(EXIT_FAILURE);
    }
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
      if (atoi(optarg) > 900)
      {
        stepspeed = 900;
      }
      else
      {
        stepspeed = atoi(optarg);
      }
      request_message.speed = stepspeed;
      request_message.speed_supplied = true; // Set speed_supplied to true when speed is provided
      request_message.command = 's';
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
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));

      struct motor_message status;
      read(serverfd,&status,sizeof(struct motor_message));

      JSON_status(&status);
      return 0;
    case 'i':
      // get all initial values
      request_message.command = 'i';
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message initial;
      read(serverfd,&initial,sizeof(struct motor_message));

      JSON_initial(&initial);
      return 0;
    case 'p':
      request_message.command = 'p';
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message pos;
      read(serverfd,&pos,sizeof(struct motor_message));

      xy_pos(&pos);
      return 0;
    case 'v':
      verbose = true; // Enable verbose mode
      break;
    case 'r': // reset
      request_message.command = 'r';
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));
      return 0;
    case 'S': // status
      request_message.command = 'S';
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message stat;
      read(serverfd,&stat,sizeof(struct motor_message));

      show_status(&stat);
      return 0;
    case 'b': // is moving?
      request_message.command = 'b';
      if (verbose) print_request_message(&request_message);
      write(serverfd,&request_message,sizeof(struct request));
      
      struct motor_message busy;
      read(serverfd,&busy,sizeof(struct motor_message));
        if(busy.status == MOTOR_IS_RUNNING){
          printf("1\n");
          return(1);
        }
        else{
          printf("0\n");
          return(0);
        }
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

  // If the command is speed only, send it and return
  if (request_message.command == 's') {
    if (verbose) print_request_message(&request_message);
    write(serverfd,&request_message,sizeof(struct request));
    return 0;
  }

  // Ensure the final request uses the correct speed if supplied
  if (request_message.speed_supplied) {
    request_message.speed = stepspeed;
  } else {
    request_message.speed = 0;  // Indicate that speed is not set
  }

  if (request_message.command == 'd') {
    switch (direction)
    {
    case 's': // stop
      request_message.type = 's';
      break;

    case 'c': // cruise
      request_message.type = 'c';
      break;

    case 'b': // go back
      request_message.type = 'b';
      break;

    case 'h': // set position (absolute movement)
      request_message.type = 'h';
      break;

    case 'g': // move x y (relative movement)
      request_message.type = 'g';
      break;

    default:
      printf("Invalid Direction Argument %c\n", direction);
      printf("Usage : %s -d\n"
             "\t s (Stop)\n"
             "\t c (Cruise)\n"
             "\t b (Go to home position)\n"
             "\t h (Set position X and Y)\n"
             "\t g (Steps X and Y)\n",
             argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  // Print and send the final request message if it's a move command
  if (request_message.command == 'd') {
    if (verbose) print_request_message(&request_message);
    write(serverfd,&request_message,sizeof(struct request));
  }

  return 0;
}
