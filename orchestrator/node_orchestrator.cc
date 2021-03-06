#include "utils/constants.h"

#include "utils/logger.h"
#include "rest_server/rest_server.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/**
*	Private variables
*/
#ifndef READ_JSON_FROM_FILE
	struct MHD_Daemon *http_daemon = NULL;
#endif

/**
*	Private prototypes
*/
#ifndef READ_JSON_FROM_FILE
bool parse_command_line(int argc, char *argv[],int *rest_port,int *core_mask, char **wirelessName);
#else
bool parse_command_line(int argc, char *argv[], char **file_name,int *core_mask, char **wirelessName);
#endif
bool usage(void);

/**
*	Implementations
*/

void singint_handler(int sig)
{    
    logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The '%s' is terminating...",MODULE_NAME);

#ifndef READ_JSON_FROM_FILE
	MHD_stop_daemon(http_daemon);
#endif
	
	try
	{
		RestServer::terminate();
	}catch(...)
	{
		//Do nothing, since the program is terminating
	}
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "Bye :D");
	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	//Check for root privileges 
	if(geteuid() != 0)
	{	
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Root permissions are required to run %s\n",argv[0]);
		exit(EXIT_FAILURE);	
	}

#ifdef READ_JSON_FROM_FILE
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "");
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "*************************************************************");
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "* The orchestrator has been compiled to read a JSON command *");
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "* from a file, hence the REST server will not be started    *");
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "*************************************************************");
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "");
#endif
	
	int core_mask;
	char *wirelessName = NULL;
#ifdef READ_JSON_FROM_FILE
	char *file_name = NULL;
	if(!parse_command_line(argc,argv,&file_name,&core_mask,&wirelessName))
#else
	int rest_port;
	if(!parse_command_line(argc,argv,&rest_port,&core_mask,&wirelessName))
#endif
		exit(EXIT_FAILURE);	

	//XXX: this code avoids that the program terminates when system() is executed
	sigset_t mask;
	sigfillset(&mask);
	sigprocmask(SIG_SETMASK, &mask, NULL);

#ifdef READ_JSON_FROM_FILE
	if(!RestServer::init(file_name,core_mask,(wirelessName == NULL)? false : true, wirelessName))
#else
	if(!RestServer::init(core_mask,(wirelessName == NULL)? false : true, wirelessName))
#endif
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Cannot start the %s",MODULE_NAME);
		exit(EXIT_FAILURE);	
	}

#ifndef READ_JSON_FROM_FILE
	http_daemon = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY, rest_port, NULL, NULL,&RestServer::answer_to_connection, 
		NULL, MHD_OPTION_NOTIFY_COMPLETED, &RestServer::request_completed, NULL,MHD_OPTION_END);
	
	if (NULL == http_daemon)
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Cannot start the HTTP deamon. The %s cannot be run.",MODULE_NAME);
		return EXIT_FAILURE;
	}
#endif
	
	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "The '%s' is started!",MODULE_NAME);
	signal(SIGINT,singint_handler);
	pause();
	
	return 0;
}

#ifndef READ_JSON_FROM_FILE
bool parse_command_line(int argc, char *argv[], int *rest_port, int *core_mask, char **wirelessName)
#else
bool parse_command_line(int argc, char *argv[], char **file_name, int *core_mask, char **wirelessName)
#endif
{
	int opt;
	char **argvopt;
	int option_index;
	
#ifdef READ_JSON_FROM_FILE
	static struct option lgopts[] = {
		{"c", 1, 0, 0},
		{"w", 1, 0, 0},
		{"f", 1, 0, 0},
		{"h", 0, 0, 0},
		{NULL, 0, 0, 0}
	};
#else
static struct option lgopts[] = {
		{"p", 1, 0, 0},
		{"c", 1, 0, 0},
		{"w", 1, 0, 0},
		{"h", 0, 0, 0},
		{NULL, 0, 0, 0}
	};
#endif

	argvopt = argv;
	uint32_t arg_c = 0, arg_w;
#ifdef READ_JSON_FROM_FILE
	uint32_t arg_f = 0;
#else
	uint32_t arg_p = 0;
#endif

	*core_mask = CORE_MASK;
	wirelessName[0] = '\0';
#ifdef READ_JSON_FROM_FILE
	file_name[0] = '\0';
#else
	*rest_port = REST_PORT;
#endif

	while ((opt = getopt_long(argc, argvopt, "", lgopts, &option_index)) != EOF)
    {
		switch (opt)
		{
			/* long options */
			case 0:
	   			if (!strcmp(lgopts[option_index].name, "c"))/* core mask for network functions */
	   			{
	   				if(arg_c > 0)
	   				{
		   				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Argument \"--c\" can appear only once in the command line");
	   					return usage();
	   				}
	   				char *port = (char*)malloc(sizeof(char)*(strlen(optarg)+1));
	   				strcpy(port,optarg);
	   				
	   				sscanf(port,"%x",&(*core_mask));
	   				
	   				arg_c++;
	   			}
	   			else if (!strcmp(lgopts[option_index].name, "w"))/* wireless */
	   			{
	   				*wirelessName = optarg;
	   				
	   				arg_w++;
	   			}
#ifdef READ_JSON_FROM_FILE
				else if (!strcmp(lgopts[option_index].name, "f"))/* file */
	   			{
	   				*file_name = optarg;
	   				
	   				arg_f++;
	   			}
#else
				else if (!strcmp(lgopts[option_index].name, "p"))/* rest port */
				{
					if(arg_p > 0)
	   				{
		   				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Argument \"--p\" can appear only once in the command line");
	   					return usage();
	   				}
	   				char *port = (char*)malloc(sizeof(char)*(strlen(optarg)+1));
	   				strcpy(port,optarg);
	   				
	   				sscanf(port,"%d",rest_port);
	   				
	   				arg_p++;
	   			}
#endif
				else if (!strcmp(lgopts[option_index].name, "h"))/* help */
	   			{
	   				return usage();
	   			}
	   			else
	   			{
	   				logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Invalid command line parameter '%s'\n",lgopts[option_index].name);
	   				return usage();
	   			}
				break;
			default:
				return usage();
		}
	}

	/* Check that all mandatory arguments are provided */
#ifdef READ_JSON_FROM_FILE
	if (arg_f == 0)
	{
		logger(ORCH_ERROR, MODULE_NAME, __FILE__, __LINE__, "Not all mandatory arguments are present in the command line");
		return usage();
	}
#endif

	return true;
}

bool usage(void)
{
	char message[]=	\

#ifndef READ_JSON_FROM_FILE
	"Usage:                                                                                   \n" \
	"  sudo ./name-orchestrator                                                               \n" \
	"                                                                                         \n" \
	"Parameters:                                                                              \n" \
	"                                                                                         \n" \
	"Options:                                                                                 \n" \
	"  --p tcp_port                                                                           \n" \
	"        TCP port used by the REST server to receive commands (default is 8080)           \n" \
	"  --c core_mask                                                                          \n" \
	"        Mask that specifies which cores must be used for DPDK network functions. These   \n" \
	"        cores will be allocated to the DPDK network functions in a round robin fashion   \n" \
	"        (default is 0x2)                                                                 \n" \
	"  --w                                                                                    \n" \
	"        name of a wireless interface (existing on the node) to be attached to the system \n" \
	"  --h                                                                                    \n" \
	"        Print this help.                                                                 \n" \
	"                                                                                         \n" \
	"Example:                                                                                 \n" \
	"  sudo ./node-orchestrator --w wlan0                                                     \n\n";
#else
	"Usage:                                                                                   \n" \
	"  sudo ./name-orchestrator --f file_name                                                 \n" \
	"                                                                                         \n" \
	"Parameters:                                                                              \n" \
	"  --f file_name                                                                          \n" \
	"        Name of the file describing the NF-FG to be deployed on the node.                \n" \
	"                                                                                         \n" \
	"Options:                                                                                 \n" \
	"  --c core_mask                                                                          \n" \
	"        Mask that specifies which cores must be used for DPDK network functions. These   \n" \
	"        cores will be allocated to the DPDK network functions in a round robin fashion   \n" \
	"        (default is 0x2)                                                                 \n" \
	"  --w                                                                                    \n" \
	"        name of a wireless interface (existing on the node) to be attached to the system \n" \
	"  --h                                                                                    \n" \
	"        Print this help.                                                                 \n" \
	"                                                                                         \n" \
	"Example:                                                                                 \n" \
	"  sudo ./node-orchestrator --f example.json --w wlan0                                    \n\n";

#endif

	logger(ORCH_INFO, MODULE_NAME, __FILE__, __LINE__, "\n\n%s",message);
	
	return false;
}
