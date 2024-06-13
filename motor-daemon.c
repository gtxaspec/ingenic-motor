#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>



#define SV_SOCK_PATH "/tmp/motors"
#define MAX_CONN 5
#define MOTOR_MOVE_STOP 0x0
#define MOTOR_MOVE_RUN 0x1

/* directional_attr */
#define MOTOR_DIRECTIONAL_UP 0x0
#define MOTOR_DIRECTIONAL_DOWN 0x1
#define MOTOR_DIRECTIONAL_LEFT 0x2
#define MOTOR_DIRECTIONAL_RIGHT 0x3

#define MOTOR1_MAX_SPEED 1000
#define MOTOR1_MIN_SPEED 10

/* ioctl cmd */
#define MOTOR_STOP 0x1
#define MOTOR_RESET 0x2
#define MOTOR_MOVE 0x3
#define MOTOR_GET_STATUS 0x4
#define MOTOR_SPEED 0x5
#define MOTOR_GOBACK 0x6
#define MOTOR_CRUISE 0x7

enum motor_status
{
  MOTOR_IS_STOP,
  MOTOR_IS_RUNNING,
};

struct request{
    char command; // d,r,s,p,b (move, reset,set speed,get position, is busy)
    char type;   // g,h,c (absolute,relative,cruise,stop)
    int x;
    int got_x;
    int y;
    int got_y;
};

struct response_position{
    int x;     //for -p and other status commands
    int y;    
};

struct motor_status_st
{
  int directional_attr;
  int total_steps;
  int current_steps;
  int min_speed;
  int cur_speed;
  int max_speed;
  int move_is_min;
  int move_is_max;
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

struct motors_steps
{
  int x;
  int y;
};

struct motor_reset_data
{
  unsigned int x_max_steps;
  unsigned int y_max_steps;
  unsigned int x_cur_step;
  unsigned int y_cur_step;
};



int motorfd = -1;
struct request request_message; // object for IPC request from client

void motor_ioctl(int cmd, void *arg)
{
  //basically exists to not pass around the motor FD
  ioctl(motorfd, cmd, arg);
}

void motor_status_get(struct motor_message *msg)
{
  motor_ioctl(MOTOR_GET_STATUS, msg);
}

void motor_get_maxsteps(unsigned int *maxx, unsigned int *maxy)
{
  struct motor_message msg;
  motor_status_get(&msg);
  if (maxx)
    *maxx = msg.x_max_steps;
  if (maxy)
    *maxy = msg.y_max_steps;
}

int motor_is_busy()
{
  struct motor_message msg;
  motor_status_get(&msg);
  return msg.status == MOTOR_IS_RUNNING ? 1 : 0;
}
void motor_steps(int xsteps, int ysteps, int stepspeed)
{
  struct motors_steps steps;
  steps.x = xsteps;
  steps.y = ysteps;

  
  syslog(LOG_NOTICE," -> steps, X %d, Y %d, speed %d\n", steps.x, steps.y, stepspeed);
  motor_ioctl(MOTOR_SPEED, &stepspeed);
  motor_ioctl(MOTOR_MOVE, &steps);
}

void motor_set_position(int xpos, int ypos, int stepspeed)
{
  struct motor_message msg;
  motor_status_get(&msg);

  int deltax = xpos - msg.x;
  int deltay = ypos - msg.y;

  
  syslog(LOG_NOTICE," -> set position current X: %d, Y: %d, steps required X: %d, Y: %d, speed %d\n", msg.x, msg.y, deltax, deltay, stepspeed);
  motor_steps(deltax, deltay, stepspeed); 
}




static void daemonsetup()
{
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir("/tmp/");

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }

    /* Open the log file */
    openlog ("motors-daemon", LOG_PID, LOG_DAEMON);
}

void requestcleanup(){
    //
    request_message.command = 'd';
    request_message.type = 's';
    request_message.x = 0;
    request_message.got_x = 0;
    request_message.y = 0;
    request_message.got_y = 0;
}

int main()
{
    daemonsetup();
    int daemonstop = 0;
    int stepspeed =900; //TODO move this value to a struct
    //struct instances
    struct sockaddr_un addr; //socket struct
    struct motor_reset_data motor_reset_data;
    struct motor_message pos;

    //acquire control of motor device and perform a reset
    motorfd = open("/dev/motor", 0);

    //reset motor as setup
    syslog(LOG_NOTICE,"== Reset position, please wait");
    memset(&motor_reset_data, 0, sizeof(motor_reset_data));
    ioctl(motorfd, MOTOR_RESET, &motor_reset_data);


    int serverfd = socket(AF_UNIX, SOCK_STREAM, 0);
    syslog(LOG_NOTICE,"Server socket fd = %d", serverfd);
    //check if we could acquire fd for socket
    if (serverfd == -1){
        syslog (LOG_ERR, "Error initializing the socket, could not get a proper fd");
        closelog();
        exit(EXIT_FAILURE);
    }

    //cleanup for socket path if path already exists
    if (remove(SV_SOCK_PATH) == -1 && errno != ENOENT) {
        syslog(LOG_ERR,"could not remove-%s, exiting", SV_SOCK_PATH);
        closelog();
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SV_SOCK_PATH, sizeof(addr.sun_path) - 1);
    //binding to socket
    if (bind(serverfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1) {
        syslog(LOG_ERR,"Error binding to socket, exiting");
        closelog();
        exit(EXIT_FAILURE);
    }

    //start listening on socket
    if (listen(serverfd, MAX_CONN) == -1) {
        closelog();
        exit(EXIT_FAILURE);
    }

    syslog (LOG_NOTICE, "motors-daemon started");


    while (daemonstop == 0)
    {   
        //make request object go back to initial value
        requestcleanup();
        syslog(LOG_NOTICE,"Waiting to accept a connection");
        //blocking code, wait for a connection
        int clientfd = accept(serverfd, NULL, NULL);
        syslog(LOG_NOTICE,"Accepting a connection\n");

        //load the message onto the reques_message struct
        if(read(clientfd,&request_message,sizeof(struct request)) == -1){
            syslog (LOG_NOTICE, "Could not read message from motors app, ignore request");
        }
        else{
            switch(request_message.command){
                case 'd': // move direction
                    switch(request_message.type){
                    case 'g': //relative movement
                        motor_steps(request_message.x, request_message.y, stepspeed);
                        break;
                    case 'h': // absolute movement
                            motor_status_get(&pos);
                            if (request_message.got_x == 0)
                              request_message.x = pos.x; //as we are rewriting initial between requests this should not be necessary but leaving as is as to not break anything
                            if (request_message.got_y == 0)
                              request_message.y = pos.y;
                            motor_set_position(request_message.x, request_message.y, stepspeed);
                        break;
                    case 'b': // go back
                        motor_ioctl(MOTOR_GOBACK, NULL);//should we block until "go back" movement is finished?
                    break;
                    case 'c': // cruise
                        motor_ioctl(MOTOR_CRUISE, NULL);
                    break;
                    case 's': // stop
                        motor_ioctl(MOTOR_STOP, NULL);
                    break;

                    }
                break;
                case 'r': //reset
                syslog (LOG_NOTICE, "executing reset...");
                //cleanup of reset data before reset, is necesary otherwise reset is never performed even though it never fails
                memset(&motor_reset_data, 0, sizeof(motor_reset_data));
                int reset = ioctl(motorfd, MOTOR_RESET, &motor_reset_data);
                break;
                case 'i': //get initial parameters
                

                break;
                case 'j': //get json

                break;
                case 'p': //get simple x y position 

                break;
                case 'b': //is busy

                break;
                case 's': //set speed
                if (request_message.x > 900){
                    stepspeed = 900;
                  }
                else{
                    stepspeed = request_message.x; //if command is s, we use the x variable so we dont define a new variable inside struct so object size is smaller
                  }

                break;
                case 'S': //show status

                break;

                
            }

        }


        syslog (LOG_NOTICE, "request command is %c",request_message.command);
        syslog (LOG_NOTICE, "request type is %c",request_message.type);
        syslog (LOG_NOTICE, "request x is %i",request_message.x);
        syslog (LOG_NOTICE, "request y is %i",request_message.y);
        syslog (LOG_NOTICE, "====================");
        motor_status_get(&pos);
        syslog (LOG_NOTICE, "final absolute coordinates are %i,%i",pos.x,pos.y);
        //break;
    }

    syslog (LOG_NOTICE, "motors-daemon terminated.");
    closelog();

    return EXIT_SUCCESS;
}