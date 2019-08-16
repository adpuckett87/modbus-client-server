/*
 * Copyright © 2017 Andrew Puckett <apuckett7@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

//gcc modbus-client.c -o modbus-client `pkg-config --libs --cflags libmodbus`
#include <stdio.h>
#ifndef _MSC_VER
	#include <unistd.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <modbus.h>

#define MODBUS_GET_M10K2_FROM_INT16(tab_int16, index) ((tab_int16[(index)] * 10000) | tab_int16[(index) + 1])

#define MODBUS_GET_M10K3_FROM_INT16(tab_int16, index) \
	(((int64_t)tab_int16[(index)    ] * 100000000) | \
	 ((int64_t)tab_int16[(index) + 1] * 10000) | \
	  (int64_t)tab_int16[(index) + 2])

#define MODBUS_GET_M10K4_FROM_INT16(tab_int16, index) \
	(((int64_t)tab_int16[(index)    ] * 1000000000000) | \
	 ((int64_t)tab_int16[(index) + 1] * 100000000) | \
	 ((int64_t)tab_int16[(index) + 2] * 10000) | \
	  (int64_t)tab_int16[(index) + 3])

#define MODBUS_SET_M10K2_TO_INT16(tab_int16, index, value) \
	do { \
		tab_int16[(index) + 1] = (value) % 10000; \
		tab_int16[(index)    ] = (value) / 10000; \
	} while (0)

#define MODBUS_SET_M10K3_TO_INT16(tab_int16, index, value) \
	do { \
		tab_int16[(index) + 2] = (value) % 10000; \
		MODBUS_SET_M10K2_TO_INT16(tab_int16, index, (value / 10000)); \
	} while (0)

#define MODBUS_SET_M10K4_TO_INT16(tab_int16, index, value) \
	do { \
		tab_int16[(index) + 3] = (value) % 10000; \
		MODBUS_SET_M10K3_TO_INT16(tab_int16, index, (value / 10000)); \
	} while (0)

uint8_t *tab_bit;
uint16_t *tab_reg;
uint16_t *tab_val;
modbus_t *ctx;

static void free_exit(int var) {
	/* Free the memory */
	free(tab_bit);
	free(tab_reg);
	free(tab_val);

	/* Close the connection */
	modbus_close(ctx);
	modbus_free(ctx);

	exit(var);
}

//modbus-client ip_address slave_id r/w 0,1,3,4 address num_reg/value format (a,b,c,d,(s)1,3,6,k,l,m)
int main(int argc, char *argv[]) {
	int reg_nb = 0;
	int rc = 0;
	int operation = 0;
	int reg_type = 0;
	int reg_start = 0;
	int isSigned = 0;
	char format = 0;
	char* value = NULL;

	if (argc < 6 || argc > 8) {
		printf("Compiled with libmodbus version %s (%06X)\n", LIBMODBUS_VERSION_STRING, LIBMODBUS_VERSION_HEX);
		printf("Linked with libmodbus version %d.%d.%d\n", libmodbus_version_major, libmodbus_version_minor, libmodbus_version_micro);
		fprintf(stderr, "Improper argument format\n");
		return -1;
	}
	if (argc > 7) {
		isSigned = strlen(argv[7]) == 2 && argv[7][0] == 's';
		format = strlen(argv[7]) == 1 ? argv[7][0] : (isSigned ? argv[7][1] : 0);
		switch (format) {
			case 'a':
			case 'b':
			case 'c':
			case 'd':
			case '1':
			case '3':
			case '6':
			case 'k':
			case 'l':
			case 'm':
				break;
			default:
				fprintf(stderr, "Invalid format. Valid options are a,b,c,d,(s)1,3,6,k,l,m\n");
				return -1;
		}
	} else {
		format = 1;
	}

	ctx = modbus_new_tcp(argv[1], 502);
	modbus_set_slave(ctx, strtol(argv[2], NULL, 10));
	if (modbus_connect(ctx) == -1) {
		fprintf(stderr, "Connection failed: %s\n", modbus_strerror(errno));
		modbus_free(ctx);
		return -1;
	}

	/* Allocate and initialize the memory to store the status */
	tab_bit = (uint8_t *) malloc(MODBUS_MAX_READ_BITS * sizeof(uint8_t));
	memset(tab_bit, 0, MODBUS_MAX_READ_BITS * sizeof(uint8_t));

	/* Allocate and initialize the memory to store the registers */
	tab_reg = (uint16_t *) malloc(MODBUS_MAX_READ_REGISTERS * sizeof(uint16_t));
	memset(tab_reg, 0, MODBUS_MAX_READ_REGISTERS * sizeof(uint16_t));

	/* Allocate and initialize the memory to store the value */
	tab_val = (uint16_t *) malloc(4 * sizeof(uint16_t));
	memset(tab_val, 0, 4 * sizeof(uint16_t));

	operation = argv[3][0];
	reg_type = strtol(argv[4], NULL, 10);
	reg_start = strtol(argv[5], NULL, 10);
	value = argv[6];
	if (operation == 'r') {
		reg_nb = argc > 6 ? strtol(value, NULL, 10) : 1;
		if (format && reg_type > 1) {
			switch (format) {
				case 'a':
				case 'b':
				case 'c':
				case 'd':
				case '3':
				case 'k':
					reg_nb *= 2;
					break;
				case 'l':
					reg_nb *= 3;
					break;
				case '6':
				case 'm':
					reg_nb *= 4;
					break;
			}
		}
		switch (reg_type) {
			case 0:
				rc = modbus_read_bits(ctx, reg_start, reg_nb, tab_bit);
				break;
			case 1:
				rc = modbus_read_input_bits(ctx, reg_start, reg_nb, tab_bit);
				break;
			case 3:
				rc = modbus_read_input_registers(ctx, reg_start, reg_nb, tab_reg);
				break;
			case 4:
				rc = modbus_read_registers(ctx, reg_start, reg_nb, tab_reg);
				break;
			default:
				fprintf(stderr, "Invalid register type. Valid options are 0,1,3 or 4\n");
				free_exit(-1);
		}
		if (rc == -1) {
			fprintf(stderr, "%s\n", modbus_strerror(errno));
			free_exit(-1);
		}
		int i;
		for (i = 0; i < rc; i++) {
			if (reg_type > 1) {
				tab_val[0] = tab_reg[i];
				tab_val[1] = tab_reg[++i];
				switch (format) {
					case 'a':
						printf("%f\n", modbus_get_float_abcd(tab_val));
						break;
					case 'b':
						printf("%f\n", modbus_get_float_badc(tab_val));
						break;
					case 'c':
						printf("%f\n", modbus_get_float_cdab(tab_val));
						break;
					case 'd':
						printf("%f\n", modbus_get_float_dcba(tab_val));
						break;
					case '3':
						printf(isSigned ? "%d\n" : "%u\n", MODBUS_GET_INT32_FROM_INT16(tab_reg, i-1));
						break;
					case '6':
						printf(isSigned ? "%lld\n" : "%llu\n", MODBUS_GET_INT64_FROM_INT16(tab_reg, i-1));
						i = i + 2;
						break;
					case 'k':
						printf(isSigned ? "%d\n" : "%u\n", MODBUS_GET_M10K2_FROM_INT16(tab_reg, i-1));
						break;
					case 'l':
						printf(isSigned ? "%lld\n" : "%llu\n", MODBUS_GET_M10K3_FROM_INT16(tab_reg, i-1));
						i++;
						break;
					case 'm':
						printf(isSigned ? "%lld\n" : "%llu\n", MODBUS_GET_M10K4_FROM_INT16(tab_reg, i-1));
						i = i + 2;
						break;
					default:
						printf(isSigned ? "%hd\n" : "%hu\n", tab_reg[--i]);
						break;
				}
			} else {
				printf("%hu\n", tab_bit[i]);
			}
		}
	} else if (operation == 'w') {
		if (argc < 7) {
			fprintf(stderr, "Improper argument format\n");
			free_exit(-1);
		}
		if (reg_type == 0) {
			rc = modbus_write_bit(ctx, reg_start, value[0] == '1' ? TRUE : FALSE);
		} else if (reg_type == 4) {
			switch (format) {
				case 'a':
					modbus_set_float_abcd(strtod(value, NULL), tab_val);
					reg_nb = 2;
					break;
				case 'b':
					modbus_set_float_badc(strtod(value, NULL), tab_val);
					reg_nb = 2;
					break;
				case 'c':
					modbus_set_float_cdab(strtod(value, NULL), tab_val);
					reg_nb = 2;
					break;
				case 'd':
					modbus_set_float_dcba(strtod(value, NULL), tab_val);
					reg_nb = 2;
					break;
				case '3':
					MODBUS_SET_INT32_TO_INT16(tab_val, 0, isSigned ? strtol(value, NULL, 10) : strtoul(value, NULL, 10));
					reg_nb = 2;
					break;
				case '6':
					MODBUS_SET_INT64_TO_INT16(tab_val, 0, isSigned ? strtoll(value, NULL, 10) : strtoull(value, NULL, 10));
					reg_nb = 4;
					break;
				case 'k':
					MODBUS_SET_M10K2_TO_INT16(tab_val, 0, isSigned ? strtol(value, NULL, 10) : strtoul(value, NULL, 10));
					reg_nb = 2;
					break;
				case 'l':
					MODBUS_SET_M10K3_TO_INT16(tab_val, 0, isSigned ? strtoll(value, NULL, 10) : strtoull(value, NULL, 10));
					reg_nb = 3;
					break;
				case 'm':
					MODBUS_SET_M10K4_TO_INT16(tab_val, 0, isSigned ? strtoll(value, NULL, 10) : strtoull(value, NULL, 10));
					reg_nb = 4;
					break;
				default:
					tab_val[0] = isSigned ? (int16_t) strtol(value, NULL, 10) : (uint16_t) strtol(value, NULL, 10);
					reg_nb = 1;
					break;
			}
			rc = modbus_write_registers(ctx, reg_start, reg_nb, tab_val);
		} else {
			fprintf(stderr, "Invalid register type. Valid options are 0 or 4\n");
			free_exit(-1);
		}
		if (rc == -1) {
			fprintf(stderr, "%s\n", modbus_strerror(errno));
			free_exit(-1);
		}
	} else {
		fprintf(stderr, "Invalid operation. Valid options are r or w\n");
		free_exit(-1);
	}

	free_exit(0);
}
