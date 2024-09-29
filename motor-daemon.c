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
#include <stdbool.h>

#define SV_SOCK_PATH "/dev/md"
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

#define PID_SIZE 32

enum motor_status
{
  MOTOR_IS_STOP,
  MOTOR_IS_RUNNING,
};

enum motor_inversion {
    MOTOR_NO_INVERSION = 0x0,      // No inversion
    MOTOR_INVERT_X = 0x1,          // Invert X only
    MOTOR_INVERT_Y = 0x2,          // Invert Y only
    MOTOR_INVERT_BOTH = 0x3        // Invert both X and Y
};

enum motor_inversion motor_inversion_state = MOTOR_NO_INVERSION;  // Default is no inversion

struct request{
    char command; // d,r,s,p,b,S,i,j (move, reset,set speed,get position, is busy,Status,initial,JSON)
    char type;   // g,h,c,s (absolute,relative,cruise,stop)
    int x;
    int got_x;
    int y;
    int got_y;
    int speed;  // Add speed to the request structure
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
  unsigned int inversion_state; // Report the inversion state
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
int last_known_speed = 900; // Default speed
bool motor_inverted = false; // Global flag for motor inversion

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

void motor_steps(int xsteps, int ysteps, int stepspeed) {
  struct motors_steps steps;

  // Apply the correct inversion based on the motor_inversion_state
  steps.x = (motor_inversion_state & MOTOR_INVERT_X) ? -xsteps : xsteps;
  steps.y = (motor_inversion_state & MOTOR_INVERT_Y) ? -ysteps : ysteps;

  syslog(LOG_DEBUG,"Starting relative move");
  syslog(LOG_DEBUG," -> steps, X %d, Y %d, speed %d\n", steps.x, steps.y, stepspeed);
  motor_ioctl(MOTOR_SPEED, &stepspeed);
  motor_ioctl(MOTOR_MOVE, &steps);
  syslog(LOG_DEBUG,"Finished setting relative move");
}

void motor_set_position(int xpos, int ypos, int stepspeed) {
  struct motor_message msg;
  motor_status_get(&msg);

  int deltax = xpos - msg.x;
  int deltay = ypos - msg.y;

  // Apply inversion to deltas based on the inversion state
  if (motor_inversion_state & MOTOR_INVERT_X) {
      deltax = -deltax;
  }
  if (motor_inversion_state & MOTOR_INVERT_Y) {
      deltay = -deltay;
  }

  syslog(LOG_DEBUG,"Starting absolute move");
  syslog(LOG_DEBUG," -> set position current X: %d, Y: %d, steps required X: %d, Y: %d, speed %d\n", msg.x, msg.y, deltax, deltay, stepspeed);
  motor_steps(deltax, deltay, stepspeed); 
  syslog(LOG_DEBUG,"Finished setting absolute move");
}

int check_pid(char *file_name)
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

int create_pid(char *file_name)
{
    FILE *f;
    char pid_buffer[PID_SIZE];

    f = fopen(file_name, "w");
    if (f == NULL)
        return -1;

    memset(pid_buffer, '\0', PID_SIZE);
    sprintf(pid_buffer, "%ld\n", (long) getpid());
    if (fwrite(pid_buffer, strlen(pid_buffer), 1, f) != 1) {
        fclose(f);
        return -2;
    }
    fclose(f);

    return 0;
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

    /* Change the working directory */
    chdir("/dev/");

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
    request_message.speed = 0;  // Reset speed in request
}

int main(int argc, char *argv[])
{   
    int c;
    char *pid_file;
    bool skip_reset = false; // Initialize skip_reset to false
    pid_file = "/var/run/motors-daemon";
    //setlogmask(LOG_UPTO(LOG_DEBUG));
    while ((c = getopt(argc, argv, "dhp")) != -1){
        switch(c){
            case 'd':
           // setlogmask(LOG_UPTO(LOG_DEBUG));
            break;
            case 'p':
            skip_reset = true; // Set skip_reset to true if -p is provided
            break;
            default:
                printf("Usage : \n"
                       "\t -d enable debugging messages to syslog\n"
                       "\t -h print this help message\n"
                       "\t -p skip reset position on launch\n"
                       "\t No option to start the daemon\n");
            return EXIT_FAILURE;
            break;
        }

    }
    daemonsetup();
    if (check_pid(pid_file) == 1) {
        syslog(LOG_INFO,"Motors daemon is already running.");
        printf("Motors daemon is already running\n");
        exit(EXIT_FAILURE);
    }
    if (create_pid(pid_file) < 0) {
        syslog(LOG_INFO,"Error creating pid file %s", pid_file);
        exit(EXIT_FAILURE);
    }
    int daemonstop = 0;
    int closeready = 0;
    //struct instances
    struct sockaddr_un addr; //socket struct
    struct motor_reset_data motor_reset_data;
    struct motor_message motor_message;

    //acquire control of motor device
    motorfd = open("/dev/motor", 0);

    //reset motor as setup if not skipping reset
    if (!skip_reset) {
        syslog(LOG_DEBUG,"== Reset position, please wait");
        memset(&motor_reset_data, 0, sizeof(motor_reset_data));
        ioctl(motorfd, MOTOR_RESET, &motor_reset_data);
    }

    int serverfd = socket(AF_UNIX, SOCK_STREAM, 0);
    syslog(LOG_DEBUG,"Server socket fd = %d", serverfd);
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

    syslog (LOG_INFO, "motors-daemon started");

    while (daemonstop == 0)
    {   
        //make request object go back to initial value
        syslog(LOG_DEBUG,"Start request cleanup");
        requestcleanup();
        //reset ok to close fd flag
        int closeready = 0;
        syslog(LOG_DEBUG,"Waiting to accept a connection");
        //blocking code, wait for a connection
        int clientfd = accept(serverfd, NULL, NULL);
        if(clientfd == -1){
            syslog(LOG_DEBUG,"clientfd is invalid after connection accept, socket doesnt work and is %i errno : %i",clientfd,errno);
            syslog(LOG_DEBUG,"exiting...");
            exit(EXIT_FAILURE);
        }
        syslog(LOG_DEBUG,"Accepting a connection\n");

        //load the message onto the reques_message struct
        if(read(clientfd,&request_message,sizeof(struct request)) == -1){
            syslog(LOG_DEBUG,"Could not read message from motors app, ignore request");
            syslog(LOG_DEBUG,"client fd at this point is %i errno : %i",clientfd,errno);
        }
        else{
            syslog (LOG_DEBUG, "request command is %c",request_message.command);

            if (request_message.speed != 0) {
                last_known_speed = request_message.speed;
                syslog(LOG_DEBUG, "Updating last known speed to %d", last_known_speed);
            } else {
                syslog(LOG_DEBUG, "Using last known speed %d", last_known_speed);
            }

            switch(request_message.command){
                case 'd': // move direction
                    syslog (LOG_DEBUG, "request type is %c",request_message.type);
                    switch(request_message.type){
                    case 'g': //relative movement
                        motor_steps(request_message.x, request_message.y, last_known_speed);
                        syslog (LOG_DEBUG, "request x is %i",request_message.x);
                        syslog (LOG_DEBUG, "request y is %i",request_message.y);
                        break;
                    case 'h': // absolute movement
                            motor_status_get(&motor_message);
                            if (request_message.got_x == 0)
                              request_message.x = motor_message.x; //as we are rewriting initial between requests this should not be necessary but leaving as is as to not break anything
                            if (request_message.got_y == 0)
                              request_message.y = motor_message.y;
                            motor_set_position(request_message.x, request_message.y, last_known_speed);
                            syslog (LOG_DEBUG, "request x is %i",request_message.x);
                            syslog (LOG_DEBUG, "request y is %i",request_message.y);
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
                    syslog (LOG_DEBUG, "== Reset position, please wait");
                    //cleanup of reset data before reset, is necesary otherwise reset is never performed even though it never fails
                    memset(&motor_reset_data, 0, sizeof(motor_reset_data));
                    ioctl(motorfd, MOTOR_RESET, &motor_reset_data);
                break;
                case 'i': //get initial parameters
                    //This doesnt seem right, we are returning current information instead of initial parameters
                    //not correcting for now, as we want to have functional parity
                    motor_status_get(&motor_message);
                    syslog (LOG_DEBUG, "Got current status to load into command");
                    write(clientfd,&motor_message,sizeof(struct motor_message));
                break;
                case 'j': //get json
                    motor_status_get(&motor_message);
                    syslog (LOG_DEBUG, "Got current status to load into command");
                    write(clientfd,&motor_message,sizeof(struct motor_message));
                break;
                case 'p': //get simple x y position 
                    motor_status_get(&motor_message);
                    syslog (LOG_DEBUG, "Got current status to load into command");
                    write(clientfd,&motor_message,sizeof(struct motor_message));

                break;
                case 'b': //is busy
                    motor_status_get(&motor_message);
                    syslog (LOG_DEBUG, "Got current status to load into command");
                    write(clientfd,&motor_message,sizeof(struct motor_message));

                break;
                case 's': //set speed
                    if (request_message.speed > 900){
                        last_known_speed = 900;
                    }
                    else{
                        last_known_speed = request_message.speed;
                    }
                    motor_ioctl(MOTOR_SPEED, &last_known_speed);
                    syslog(LOG_DEBUG, "Set speed command, last known speed now %d", last_known_speed);
                break;
                case 'I': // Invert motor direction
                    switch (request_message.type) {
                        case 'x': // Invert X only
                            motor_inversion_state ^= MOTOR_INVERT_X;
                            syslog(LOG_DEBUG, "Motor inversion X set to %s", (motor_inversion_state & MOTOR_INVERT_X) ? "ON" : "OFF");
                            break;
                        case 'y': // Invert Y only
                            motor_inversion_state ^= MOTOR_INVERT_Y;
                            syslog(LOG_DEBUG, "Motor inversion Y set to %s", (motor_inversion_state & MOTOR_INVERT_Y) ? "ON" : "OFF");
                            break;
                        case 'b': // Invert both X and Y
                            motor_inversion_state ^= MOTOR_INVERT_BOTH;
                            syslog(LOG_DEBUG, "Motor inversion set to %s", (motor_inversion_state == MOTOR_INVERT_BOTH) ? "BOTH ON" : "BOTH OFF");
                            break;
                        default:
                            syslog(LOG_DEBUG, "Invalid inversion command type.");
                            break;
                    }
                break;
                case 'S': //show status
                    motor_status_get(&motor_message);
                    motor_message.inversion_state = motor_inversion_state;
                    write(clientfd,&motor_message,sizeof(struct motor_message));
                    syslog(LOG_DEBUG, "Sent motor status");
                break;
            }

            //need to close fd after each request is completed
            close(clientfd); 
        }
       
        syslog (LOG_DEBUG, "====================");

        //break;
    }

    syslog (LOG_INFO, "motors-daemon terminated.");
    unlink(pid_file);
    closelog();

    return EXIT_SUCCESS;
}
