# Modbus TCP Client/Server

Based on libmodbus

+ modbus-client is a standalone command line Modbus TCP client
+ modbus-server is a Modbus TCP server that uses mysql to store register values

## Usage

```
modbus-client ip_address slave_id r reg_type address [num_reg format]
```
```
modbus-client ip_address slave_id w reg_type address value [format]
```

Register Type

+ 0 - Coil
+ 1 - Discrete Input
+ 3 - Input registers
+ 4 - Holding registers

Format

+ a - floating point abcd
+ b - floating point badc
+ c - floating point cdab
+ d - floating point dcba
+ (s)1 - 16-bit register. Optionally signed. Default.
+ (s)3 - 32-bit register. Optionally signed.
+ (s)6 - 64-bit register. Optionally signed.
+ (s)k - 32-bit Mod10k. Optionally signed.
+ (s)l - 48-bit Mod10k. Optionally signed.
+ (s)m - 64-bit Mod10k. Optionally signed.


## Install modbus-client

### Install [libmodbus](https://github.com/stephane/libmodbus)

From libmodbus documentation:
```
chmod +x ./autogen.sh
./autogen.sh
./configure
make install
```

May need to do this depending on distribution
```
cp src/.libs/libmodbus.so* /usr/lib/
ln -s -f /usr/lib/libmodbus.so.5.1.0 /usr/lib/libmodbus.so.5
ln -s -f /usr/lib/libmodbus.so.5.1.0 /usr/lib/libmodbus.so
```

### Compile modbus-client
```
gcc modbus-client.c -o modbus-client `pkg-config --libs --cflags libmodbus`
```

## Install modbus-server

### Install [libmodbus](https://github.com/stephane/libmodbus) (see above) and [inih](https://github.com/benhoyt/inih)

### Compile modbus-server
```
gcc inih/ini.c modbus-server.c -o modbus-server -std=gnu99 `mysql_config --cflags --libs` `pkg-config --libs --cflags libmodbus`

cp modbus-server /usr/bin/
```

### Make sure mysql is installed with libmysqlclient-dev

Choose a database and create the following table
```
CREATE TABLE IF NOT EXISTS `modbusServer` (
	`id` int(11) NOT NULL AUTO_INCREMENT,
	`regType` int(11) NOT NULL,
	`address` int(11) NOT NULL,
	`val` int(11) NOT NULL DEFAULT '0',
	`modifiedCount` int(11) NOT NULL DEFAULT '0',
	`updated` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	PRIMARY KEY (`id`),
	KEY (`regType`),
	KEY (`address`),
	UNIQUE KEY (`regType`, `address`)
) ENGINE=InnoDB;
```

Copy ini to /etc and fill in mysql information. Mysql user should have read/write access
```
cp modbus-server.ini /etc/
```

Run `modbus-server`

### Optionally install modbus-server as a service

Install runit and execute the following commands

```
mkdir -p /etc/sv/modbus-server/log
mkdir /var/log/modbus-server
cat > /etc/sv/modbus-server/run <<- EOM
#!/bin/sh
exec /usr/bin/modbus-server 2>&1
EOM

cat > /etc/sv/modbus-server/log/run <<- EOM
#!/bin/sh
exec svlogd -tt /var/log/modbus-server/
EOM

chmod +x /etc/sv/modbus-server/run
chmod +x /etc/sv/modbus-server/log/run
ln -s /etc/sv/modbus-server /etc/service/
```