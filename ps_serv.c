#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <mariadb/mysql.h>

#define PIN_LEN 4
#define FNAME_LEN 256
#define COUNTER_DIGITS 8
#define SCAN_NAME_LEN 19

#define DB_USER ""
#define DB_UPASSWORD ""
#define DB_NAME ""

#define MODE_PRINT 4
#define MODE_SCAN_LOGIN 8
#define MODE_SCAN_MAIN 16

#define ERROR_PIN 1

struct str_list
{
	char *name;
	struct str_list* next;
	struct str_list* prev;
	int count;

	char *print_left;
};
void free_list_from_head(struct str_list *head);
void free_list_from_tail(struct str_list **tail);

void cl_handler(int cl_sd, MYSQL *db_con);
char* receive_pin(int cl_sd);
struct str_list* make_file_list(char *pin_str);

MYSQL* db_connect();
char* db_get_user_id(MYSQL *db_con, char *pin_str);
struct str_list* db_get_file_names(MYSQL *db_con, char *user_id);

void db_set_scans_paid(MYSQL *db_con, char *user_id, char* new_value);
char* db_get_scans_paid(MYSQL *db_con, char *user_id);
void db_set_new_scan(MYSQL *db_con, char *user_id, char *scan_name);
void send_scans_paid(int cl_sd, char *scans_paid_str);
char* cl_get_scans_paid(int cl_sd);

void cl_main(MYSQL *db_con, int ls_sd);
int cl_get_mode(int ls_sd);

void cl_print_mode(MYSQL *db_con, int ls_sd);
void cl_scan_mode(MYSQL *db_con, int ls_sd);
void cl_scan_login_mode(MYSQL *db_con, int ls_sd);
void cl_scan_main_mode(MYSQL *db_con, int ls_sd);

char* itoa(int n)
{
	int len = snprintf(NULL, 0, "%u", n);
	char *n_str = malloc(len+1);
	snprintf(n_str, len+1, "%u", n);
	return n_str;
}

void sigchld_handler()
{
	wait(NULL);
}

int main(int argc, char *argv[])
{
	MYSQL* db_con = db_connect();

	if(argc < 2)
	{
		printf("Usage: ./ps_serv <port>\n");
		exit(0);
	}

	signal(SIGCHLD, sigchld_handler);

	short port = atoi(argv[1]);

	int ls_sd = socket(AF_INET, SOCK_STREAM, 0);
	assert(ls_sd != -1);

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	int opt = 1;
	setsockopt(ls_sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	int bind_flag = bind(ls_sd, (struct sockaddr*)&addr, sizeof(addr));
	assert(bind_flag != -1);

	int ls_flag = listen(ls_sd, 5);
	assert(ls_flag != -1);

	for(;;)
	{
		cl_main(db_con, ls_sd);
	}

	return 0;
}

void free_list_from_tail(struct str_list **tail_ref)
{
   struct str_list* current = *tail_ref;
   struct str_list* prev;

   while (current != NULL)
   {
       prev = current->prev;
       free(current);
       current = prev;
   }

   *tail_ref = NULL;
}

MYSQL* db_connect()
{
	MYSQL* MySQLConRet;
	MYSQL* MySQLConnection = NULL;

	MySQLConnection = mysql_init(NULL);
	MySQLConRet = mysql_real_connect(	MySQLConnection,
										"localhost",
										DB_USER,
										DB_UPASSWORD,
										DB_NAME,
										0,
										NULL,
										0);

	return MySQLConRet;
}

void cl_print_mode(MYSQL *db_con, int ls_sd)
{
	FILE *f;
	int c;
	uint32_t fsize;
	char *f_buf;
	uint32_t i;
	char *pin_str;
	char *user_id;
	int nul;

	int cl_sd = accept(ls_sd, NULL, NULL);

	pin_str = receive_pin(cl_sd);
	user_id = db_get_user_id(db_con, pin_str);
	nul = 0;

	if(user_id == NULL)
	{
		write(cl_sd, &nul, sizeof(nul));
		return;
	}

	struct str_list* file_names = db_get_file_names(db_con, user_id);

	if(file_names != NULL)
	{
		write(cl_sd, &(file_names->count), sizeof(file_names->count));
		chdir(user_id);

		for(; file_names->next != NULL; file_names = file_names->next)
		{
			fsize = 0;
			i = 0;
			f = fopen(file_names->name, "r");

			while((c = fgetc(f)) != EOF) fsize++;
			rewind(f);

			f_buf = malloc(fsize);
			while((c = fgetc(f)) != EOF) { f_buf[i] = c; i++; }
			
			write(cl_sd, &fsize, sizeof(fsize));
			write(cl_sd, file_names->name, FNAME_LEN);
			write(cl_sd, f_buf, fsize);	
			write(cl_sd, file_names->print_left, COUNTER_DIGITS);	

			fclose(f);
			free(f_buf);
		}

		chdir("..");
		free(pin_str);
		free_list_from_tail(&file_names);

	}
	else
	{
		exit(ERROR_PIN);
	}
}

char* receive_pin(int cl_sd)
{
	char *pin_str = malloc((PIN_LEN+1)*sizeof(char));
	memset(pin_str, '\0', PIN_LEN+1);
	read(cl_sd, pin_str, PIN_LEN);
	return pin_str;
}

char* db_get_user_id(MYSQL *db_con, char *pin_str)
{
	int pin_len;
	char req_arg[FNAME_LEN];
	int num_rows;

	pin_len = strlen(pin_str);
	memset(req_arg, '\0', FNAME_LEN);
	req_arg[0] = '"';
	strcpy(req_arg+1, pin_str);
	req_arg[pin_len+1] = '"';

	char req_sql[1024] = "SELECT user_id FROM users WHERE pin_code=";
	int req_sql_len = strlen(req_sql);
	strcpy(req_sql+req_sql_len, req_arg);

	int req_res = mysql_query(db_con, req_sql);
	if(req_res != 0)
	{
		fprintf(stderr, "db_get_user_id::error\n");
	}

	MYSQL_RES* sql_res = mysql_store_result(db_con);
	MYSQL_ROW row = mysql_fetch_row(sql_res);
	num_rows =  mysql_num_rows(sql_res);

	if(num_rows == 0)
		return NULL;

	return row[0];
}

struct str_list* db_get_file_names(MYSQL *db_con, char *user_id)
{
	int userid_len = strlen(user_id);
	char req_arg[FNAME_LEN];
	memset(req_arg, '\0', FNAME_LEN);
	req_arg[0] = '"';
	strcpy(req_arg+1, user_id);
	req_arg[userid_len+1] = '"';

	char req_sql[1024] =
		"SELECT doc_paid, doc_name, print_left FROM documents WHERE user_id=";
	int req_sql_len = strlen(req_sql);
	strcpy(req_sql+req_sql_len, req_arg);

	int req_res = mysql_query(db_con, req_sql);
	if(req_res != 0)
	{
		fprintf(stderr, "db_get_file_names::error\n");
		exit(1);
	}

	MYSQL_RES* sql_res = mysql_store_result(db_con);
	MYSQL_ROW row;

	struct str_list* file_names = malloc(sizeof(struct str_list));
	file_names->next = NULL;
	file_names->prev = NULL;
	struct str_list* prev;
	int count = 0;

	while((row = mysql_fetch_row(sql_res)))
	{
		if(row[0][0] != '0')
		{
			file_names->name = malloc((FNAME_LEN+1)*sizeof(char));
			memset(file_names->name, '\0', FNAME_LEN+1);
			file_names->print_left = malloc((COUNTER_DIGITS+1)*sizeof(char));
			memset(file_names->print_left, '\0', COUNTER_DIGITS+1);

			strcpy(file_names->name, row[1]);
			strcpy(file_names->print_left, row[2]);

			file_names->next = malloc(sizeof(struct str_list));
			prev = file_names;
			file_names = file_names->next;
			file_names->next = NULL;
			file_names->prev = prev;

			count++;
		}
	}

	for(; file_names->prev != NULL; file_names = file_names->prev)
	{
		file_names->count = count;
	}
	file_names->count = count;

	return file_names;
}

void db_set_scans_paid(MYSQL *db_con, char *user_id, char* new_value)
{
	int usrid_len;
	char usrid_arg[FNAME_LEN];

	usrid_len = strlen(user_id);
	memset(usrid_arg, '\0', FNAME_LEN);
	usrid_arg[0] = '"';
	strcpy(usrid_arg+1, user_id);
	usrid_arg[usrid_len+1] = '"';

	char req1[FNAME_LEN] = "UPDATE users SET scans_paid=";
	char req2[FNAME_LEN] = "WHERE user_id=";
	char req_sql[FNAME_LEN*2];

	int req1_len = strlen(req1);
	int req2_len = strlen(req2);

	strcpy(req1+req1_len, new_value);
	strcpy(req2+req2_len, usrid_arg);

	strcpy(req_sql, req1);
	int req_len = strlen(req_sql);
	req_sql[req_len] = ' ';
	strcpy(req_sql+req_len+1, req2);

	int req_res = mysql_query(db_con, req_sql);
	if(req_res != 0)
	{
		fprintf(stderr, "db_set_scans_paid::error\n");
		exit(2);
	}
}

char* db_get_scans_paid(MYSQL *db_con, char *user_id)
{
	int usrid_len;
	char req_arg[FNAME_LEN];
	int num_row;

	usrid_len = strlen(user_id);
	memset(req_arg, '\0', FNAME_LEN);
	req_arg[0] = '"';
	strcpy(req_arg+1, user_id);
	req_arg[usrid_len+1] = '"';

	char req_sql[1024] = "SELECT scans_paid FROM users WHERE user_id=";
	int req_sql_len = strlen(req_sql);
	strcpy(req_sql+req_sql_len, req_arg);

	int req_res = mysql_query(db_con, req_sql);
	if(req_res != 0)
	{
		fprintf(stderr, "db_get_scans_paid::error\n");
		exit(1);
	}

	MYSQL_RES* sql_res = mysql_store_result(db_con);
	MYSQL_ROW row = mysql_fetch_row(sql_res);
	num_row = mysql_num_rows(sql_res);

	if(num_row == 0)
		return NULL;

	return row[0];
}

void send_scans_paid(int cl_sd, char *scans_paid_str)
{
	uint32_t scans_paid = atoi(scans_paid_str);
	write(cl_sd, &scans_paid, sizeof(scans_paid));
}

char* cl_get_scans_paid(int cl_sd)
{
	char *scans_paid_str;
	uint32_t scans_paid;

	read(cl_sd, &scans_paid, sizeof(scans_paid));
	scans_paid_str = itoa(scans_paid);

	return scans_paid_str;
}

void db_set_new_scan(MYSQL *db_con, char *user_id, char *scan_name)
{
	int usrid_len, scanname_len;
	char usrid_arg[32];
	char scanname_arg[SCAN_NAME_LEN+3];
	char req_sql[FNAME_LEN] =
		"INSERT INTO scan_table (user_id, doc_name) VALUES (";
	
	usrid_len = strlen(user_id);
	memset(usrid_arg, '\0', 32);
	usrid_arg[0] = '"';
	strcpy(usrid_arg+1, user_id);
	usrid_arg[usrid_len+1] = '"';

	scanname_len = strlen(scan_name);
	memset(scanname_arg, '\0', SCAN_NAME_LEN+3);
	scanname_arg[0] = '"';
	strcpy(scanname_arg+1, scan_name);
	scanname_arg[scanname_len+1] = '"';
	
	strcpy(req_sql+strlen(req_sql), usrid_arg);
	req_sql[strlen(req_sql)] = ',';
	req_sql[strlen(req_sql)] = ' ';
	strcpy(req_sql+strlen(req_sql), scanname_arg);
	req_sql[strlen(req_sql)] = ')';

	int req_res = mysql_query(db_con, req_sql);
	if(req_res != 0)
	{
		fprintf(stderr, "db_set_new_scan::error\n");
		exit(2);
	}
}

void cl_scan_main_mode(MYSQL *db_con, int ls_sd)
{

	int cl_sd = accept(ls_sd, NULL, NULL);
	char *scan_name = malloc(SCAN_NAME_LEN+1);
	memset(scan_name, '\0', SCAN_NAME_LEN+1);
	uint32_t scan_size;
	char *scan_buf;
	int scan_fd;
	char *pin_str = malloc(PIN_LEN+1);;
	memset(pin_str, '\0', PIN_LEN+1);

	read(cl_sd, scan_name, SCAN_NAME_LEN);
	read(cl_sd, &scan_size, sizeof(scan_size));
	read(cl_sd, pin_str, PIN_LEN);

	char *new_value = cl_get_scans_paid(cl_sd);
	char *user_id = db_get_user_id(db_con, pin_str);

	db_set_new_scan(db_con, user_id, scan_name);
	db_set_scans_paid(db_con, user_id, new_value);

	scan_buf = malloc(scan_size);
	read(cl_sd, scan_buf, scan_size);

	scan_fd = open(scan_name, O_CREAT | O_WRONLY, 0666);
	write(scan_fd, scan_buf, scan_size);
	free(scan_buf);
}

void cl_scan_login_mode(MYSQL *db_con, int ls_sd)
{
	int cl_sd = accept(ls_sd, NULL, NULL);
	char* pin_str = receive_pin(cl_sd);
	char* user_id = db_get_user_id(db_con, pin_str);
	char* scans_paid = db_get_scans_paid(db_con, user_id);
	send_scans_paid(cl_sd, scans_paid);
}

void cl_scan_mode(MYSQL *db_con, int ls_sd)
{
	cl_scan_login_mode(db_con, ls_sd);
	cl_scan_main_mode(db_con, ls_sd);
}

int cl_get_mode(int ls_sd)
{
	int cl_sd = accept(ls_sd, NULL, NULL);
	int mode;
	read(cl_sd, &mode, sizeof(mode));
	return mode;
}

void cl_main(MYSQL *db_con, int ls_sd)
{
	int mode = cl_get_mode(ls_sd);

	switch(mode)
	{
		case MODE_PRINT: cl_print_mode(db_con, ls_sd); break;
		case MODE_SCAN_LOGIN: cl_scan_login_mode(db_con, ls_sd); cl_scan_main_mode(db_con, ls_sd);
		default: break;
	}
}

