/*
 * Copyright © 2017 Andrew Puckett <apuckett7@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//gcc inih/ini.c modbus-server.c -o modbus-server -std=gnu99 `mysql_config --cflags --libs` `pkg-config --libs --cflags libmodbus`
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <mysql.h>
#include <modbus/modbus.h>
#include "inih/ini.h"

#if defined(_WIN32)
	#include <ws2tcpip.h>
#else
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
#endif

static modbus_t *ctx = NULL;
static modbus_mapping_t *mb_mapping;
static int server_socket = -1;

typedef struct {
	const char* ip;
	int port;
	int nb_connections;
	int start_bits;
	int nb_bits;
	int start_input_bits;
	int nb_input_bits;
	int start_registers;
	int nb_registers;
	int start_input_registers;
	int nb_input_registers;

	const char* db;
	const char* user;
	const char* pass;
	const char* table;
} configuration;

configuration config;
MYSQL *con;
MYSQL_RES *result;
unsigned long long num_connections;
unsigned long long num_responses;
unsigned long long num_errors;

#define INI_FILE "/etc/modbus-server.ini"
#define REPORT_INTERVAL 60 //Seconds

static int handler(void* config, const char* section, const char* name, const char* value) {
	configuration* pconfig = (configuration*)config;

	#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
	if (MATCH("server", "ip")) {
		if (strcmp(value, "NULL") == 0 || strcmp(value, "null") == 0) {
			pconfig->ip = NULL;
		} else {
			pconfig->ip = strdup(value);
		}
	} else if (MATCH("server", "port")) {
		pconfig->port = atoi(value);
	} else if (MATCH("server", "nb_connections")) {
		pconfig->nb_connections = atoi(value);
	} else if (MATCH("server", "start_bits")) {
		pconfig->start_bits = atoi(value);
	} else if (MATCH("server", "nb_bits")) {
		pconfig->nb_bits = atoi(value);
	} else if (MATCH("server", "start_input_bits")) {
		pconfig->start_input_bits = atoi(value);
	} else if (MATCH("server", "nb_input_bits")) {
		pconfig->nb_input_bits = atoi(value);
	} else if (MATCH("server", "start_registers")) {
		pconfig->start_registers = atoi(value);
	} else if (MATCH("server", "nb_registers")) {
		pconfig->nb_registers = atoi(value);
	} else if (MATCH("server", "start_input_registers")) {
		pconfig->start_input_registers = atoi(value);
	} else if (MATCH("server", "nb_input_registers")) {
		pconfig->nb_input_registers = atoi(value);
	} else if (MATCH("mysql", "db")) {
		pconfig->db = strdup(value);
	} else if (MATCH("mysql", "user")) {
		pconfig->user = strdup(value);
	} else if (MATCH("mysql", "pass")) {
		pconfig->pass = strdup(value);
	} else if (MATCH("mysql", "table")) {
		pconfig->table = strdup(value);
	} else {
		return 0;	/* unknown section/name, error */
	}
	return 1;
}

static void close_sigint(int var) {
	printf("CLOSING\n");
	printf("Connections: %lld; Responses: %lld; Errors: %lld\n", num_connections, num_responses, num_errors);
	if (server_socket != -1) {
		close(server_socket);
	}
	modbus_free(ctx);
	modbus_mapping_free(mb_mapping);

	exit(var);
}

static int mysql_execute_query(MYSQL *con, char *query_string) {
	if (mysql_query(con, query_string)) {
		fprintf(stderr, "%s - RECONNECTING\n", mysql_error(con));
		if (mysql_real_connect(con, "localhost", config.user, config.pass, config.db, 0, NULL, 0) == NULL) {
			fprintf(stderr, "%s\n", mysql_error(con));
			return -1;
		}
		if (mysql_query(con, query_string)) {
			fprintf(stderr, "%s\n", mysql_error(con));
			return -1;
		}
	}
	return 0;
}

int main(int argc, char* argv[]) {
	uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
	int master_socket;
	int rc;
	fd_set refset;
	fd_set rdset;
	int fdmax;

	MYSQL_ROW row;
	char query_string[100] = {0};
	unsigned int reg_start;
	unsigned int reg_nb;
	unsigned int address;
	int val;
	unsigned int num_registers = 0;
	time_t last_time = 0;

	printf("Compiled with libmodbus version %s (%06X)\n", LIBMODBUS_VERSION_STRING, LIBMODBUS_VERSION_HEX);
	printf("Linked with libmodbus version %d.%d.%d\n", libmodbus_version_major, libmodbus_version_minor, libmodbus_version_micro);

	if (ini_parse(INI_FILE, handler, &config) < 0) {
		printf("Can't load '%s'\n", INI_FILE);
		return 1;
	}

	ctx = modbus_new_tcp(config.ip, config.port);

	mb_mapping = modbus_mapping_new_start_address(config.start_bits, config.nb_bits,
	                                              config.start_input_bits, config.nb_input_bits,
	                                              config.start_registers, config.nb_registers,
	                                              config.start_input_registers, config.nb_input_registers);
	if (mb_mapping == NULL) {
		fprintf(stderr, "Failed to allocate the mapping: %s\n",
				modbus_strerror(errno));
		modbus_free(ctx);
		return -1;
	}

	server_socket = modbus_tcp_listen(ctx, config.nb_connections);
	if (server_socket == -1) {
		fprintf(stderr, "Unable to listen TCP connection\n");
		modbus_free(ctx);
		return -1;
	}

	con = mysql_init(NULL);

	if (con == NULL) {
		fprintf(stderr, "%s\n", mysql_error(con));
	}

	if (mysql_real_connect(con, "localhost", config.user, config.pass, config.db, 0, NULL, 0) == NULL) {
		fprintf(stderr, "%s\n", mysql_error(con));
		mysql_close(con);
	} else {
		for (int i = 0; i <= 4; i++) {
			if (i == 2) i++;
			switch (i) {
				case 0:
					reg_start = config.start_bits;
					reg_nb = config.nb_bits;
					break;
				case 1:
					reg_start = config.start_input_bits;
					reg_nb = config.nb_input_bits;
					break;
				case 3:
					reg_start = config.start_input_registers;
					reg_nb = config.nb_input_registers;
					break;
				case 4:
					reg_start = config.start_registers;
					reg_nb = config.nb_registers;
					break;
			}

			sprintf(query_string, "SELECT address, val FROM %s WHERE regType = %d ORDER BY address asc", config.table, i);
			if (mysql_query(con, query_string)) {
				fprintf(stderr, "%s\n", mysql_error(con));
				mysql_close(con);
				break;
			}

			result = mysql_store_result(con);

			if (result == NULL) {
				fprintf(stderr, "%s\n", mysql_error(con));
				continue;
			}
			row = mysql_fetch_row(result);
			for (int j = reg_start; j < reg_nb; j++) {
				if (row && atoi(row[0]) == j) {
					switch (i) {
						case 0:
							mb_mapping->tab_bits[j - reg_start] = atoi(row[1]);
							break;
						case 1:
							mb_mapping->tab_input_bits[j - reg_start] = atoi(row[1]);
							break;
						case 3:
							mb_mapping->tab_input_registers[j - reg_start] = atoi(row[1]);
							break;
						case 4:
							mb_mapping->tab_registers[j - reg_start] = atoi(row[1]);
							break;
					}
					row = mysql_fetch_row(result);
					num_registers++;
				} else {
					sprintf(query_string, "INSERT INTO %s (regType, address) VALUES (%d, %d)", config.table, i, j);
					if (mysql_query(con, query_string)) {
						fprintf(stderr, "%s\n", mysql_error(con));
					}
				}
			}
			mysql_free_result(result);
		}
		printf("%d values loaded into modbus map\n", num_registers);
		fflush(stdout);
	}

	signal(SIGINT, close_sigint);

	/* Clear the reference set of socket */
	FD_ZERO(&refset);
	/* Add the server socket */
	FD_SET(server_socket, &refset);

	/* Keep track of the max file descriptor */
	fdmax = server_socket;

	for (;;) {
		if (time(NULL) >= last_time + REPORT_INTERVAL) {
			last_time = time(NULL);
			printf("Connections: %lld; Responses: %lld; Errors: %lld\n", num_connections, num_responses, num_errors);
			fflush(stdout);
		}

		rdset = refset;
		if (select(fdmax+1, &rdset, NULL, NULL, NULL) == -1) {
			perror("Server select() failure.");
			close_sigint(1);
		}

		/* Run through the existing connections looking for data to be
		 * read */
		for (master_socket = 0; master_socket <= fdmax; master_socket++) {

			if (!FD_ISSET(master_socket, &rdset)) {
				continue;
			}

			if (master_socket == server_socket) {
				/* A client is asking a new connection */
				socklen_t addrlen;
				struct sockaddr_in clientaddr;
				int newfd;

				/* Handle new connections */
				addrlen = sizeof(clientaddr);
				memset(&clientaddr, 0, sizeof(clientaddr));
				newfd = accept(server_socket, (struct sockaddr *)&clientaddr, &addrlen);
				if (newfd == -1) {
					perror("Server accept() error");
				} else {
					FD_SET(newfd, &refset);

					if (newfd > fdmax) {
						/* Keep track of the maximum */
						fdmax = newfd;
					}
					num_connections++;
				}
			} else {
				modbus_set_socket(ctx, master_socket);
				rc = modbus_receive(ctx, query);
				if (rc > 0) {
					rc = modbus_reply(ctx, query, rc, mb_mapping);
					if (rc > 9) {
						num_responses++;
						address = (query[8] << 8) + query[9];
						val = (query[10] << 8) + query[11];
						switch (query[7]) {
							case 5:
								//Single Coil
								if (val == 0) {
									val = 0;
								} else if (val == 0xFF00) {
									val = 1;
								} else {
									val = -1;
								}
								if (val != -1) {
									sprintf(query_string, "UPDATE %s SET val = %d, modifiedCount=modifiedCount+1 WHERE regType=0 AND address=%d", config.table, val, address);
									mysql_execute_query(con, query_string);
								}
								break;
							case 6:
								//Single Register
								sprintf(query_string, "UPDATE %s SET val = %d, modifiedCount=modifiedCount+1 WHERE regType=4 AND address=%d", config.table, val, address);
								mysql_execute_query(con, query_string);
								break;
							case 15:
								//Multiple Coils
								break;
							case 16:
								//Multiple Registers
								num_registers = val;
								for (int i = 13; i < num_registers * 2 + 13; i++) {
									val = (query[i] << 8) + query[++i];
									sprintf(query_string, "UPDATE %s SET val = %d, modifiedCount=modifiedCount+1 WHERE regType=4 AND address=%d", config.table, val, address);
									mysql_execute_query(con, query_string);
									address++;
								}
								break;
						}
					} else {
						num_errors++;
					}
				} else if (rc == -1) {
					/* This example server in ended on connection closing or
					 * any errors. */
					close(master_socket);

					/* Remove from reference set */
					FD_CLR(master_socket, &refset);

					if (master_socket == fdmax) {
						fdmax--;
					}
				}
			}
		}
	}

	return 0;
}
