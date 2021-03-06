/*  sensorcal - Sensor Calibration for Openvario Sensorboard - http://www.openvario.org/
    Copyright (C) 2014  The openvario project
    A detailed list of copyright holders can be found in the file "AUTHORS" 

    This program is free software; you can redistribute it and/or 
    modify it under the terms of the GNU General Public License 
    as published by the Free Software Foundation; either version 3
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "24c16.h"
#include "ams5915.h"
#include "sensorcal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "mpu9150.h"
#include "linux_glue.h"
#include "../ahrs_settings.h"
#include <termios.h>
#include <assert.h>

#include <fcntl.h>


	
int g_debug=0;
FILE *fp_console=NULL;
int rotation = -1;
	
int calibrate_ams5915(t_eeprom_data* data)
{
	t_ams5915 dynamic_sensor;
	float offset=0.0;
	int i;
	
	// open sensor for differential pressure
	/// @todo remove hardcoded i2c address for differential pressure
	printf("Open sensor ...");
	if (ams5915_open(&dynamic_sensor, 0x28) != 0)
	{
		printf(" failed !!\n");
		return 1;
	}
	else
	{
		printf(" success!\n");
	}
	
	dynamic_sensor.offset = 0.0;
	dynamic_sensor.linearity = 1.0;
	
	//initialize differential pressure sensor
	ams5915_init(&dynamic_sensor);
	
	for (i=0;i<10 ;i++)
	{
		// read AMS5915
		ams5915_measure(&dynamic_sensor);
		ams5915_calculate(&dynamic_sensor);
		
		// wait some time ...
		usleep(1000000);
		printf("Measured: %f\n",dynamic_sensor.p);
		
		// calc offset 
		offset += dynamic_sensor.p;
	}
		
	data->zero_offset = -1*(offset/10);
	
	return(0);
}

int calibrate_mpu9150(t_eeprom_data* data, short unsigned int mag)
{
	int i2c_bus = AHRS_I2C_BUS;
	int sample_rate = AHRS_SAMPLE_RATE_HZ;
	int i;
	unsigned long loop_delay;
	mpudata_t mpu;
	signed int minVal[3];
	signed int maxVal[3];
	short change;
	int k;
	int done = 0;
	int ch;
	
	if (sample_rate == 0)
		return 1;
		
	if(rotation ==-1)
	{
		printf("\nI need to know how your Openvario will be installed.\n");
		printf("Enter '0, 1, 2 or 3, below, or press 'ESC' to cancel\n");
		printf("0 = Normal landscape\n");
		printf("1 = portrait (rotated clockwise 90 degrees)\n");
		printf("2 = landscape (upside down)\n");
		printf("3 = portrait (rotated anticlockwise 90 degrees)\n");

		ch = get_keypress_blocking();
		switch(ch)
		{
			case 27:
				return 1;
				break;
			case 49:
				rotation = 1;
				break;
			case 50:
				rotation = 2;
				break;
			case 51:
				rotation = 3;
				break;
			case 48:
			default:
				printf("Invalid orientation. Assuming '0'\n");
				rotation = 0;
				break;
		}
	}

	if (mpu9150_init(i2c_bus, sample_rate, 0, rotation))
	{
		printf("Failed to connect to mpu9150\n");
		return 1;
	}
	memset(&mpu, 0, sizeof(mpudata_t));

	for (i = 0; i < 3; i++) 
	{
		minVal[i] = 0;
		maxVal[i] = 0;
	}

	loop_delay = (1000 / sample_rate) - 2;

	
	printf("\nRotate your OpenVario gently 360 degrees in three dimensions.\n");
	printf("Keep turning until you can no longer increase the displayed numbers. When done, press any key, or press 'ESC' to cancel.\n\n");

	linux_delay_ms(loop_delay);
	
	while (!done) 
	{
		if(get_keypress_nonblocking()) 
		{
			mpu9150_exit();
			k = getchar();
			done = 1;
			if(k != 27)
			{
				for (i=0; i<3; i++)
				{
					if(mag)
					{
						data->mag_min[i] = minVal[i];
						data->mag_max[i] = maxVal[i];
					}
					else
					{
						data->accel_min[i] = minVal[i];
						data->accel_max[i] = maxVal[i];
					}
				}
				return 0;
			}
			return 1;
		}
			
		change = 0;
		if(mag) 
		{
			if (mpu9150_read_mag(&mpu) == 0)
			{
				for (i = 0; i < 3; i++)
				{
					if (mpu.rawMag[i] < minVal[i])
					{
						minVal[i] = mpu.rawMag[i];
						change = 1;
					}
				
					if (mpu.rawMag[i] > maxVal[i])
					{
						maxVal[i] = mpu.rawMag[i];
						change = 1;
					}
				}
			}
		}
		else
		{
			if (mpu9150_read_dmp(&mpu) == 0)
			{
				for (i = 0; i < 3; i++)
				{
					if (mpu.rawAccel[i] < minVal[i])
					{
						minVal[i] = mpu.rawAccel[i];
						change = 1;
					}
					if (mpu.rawAccel[i] > maxVal[i])
					{
						maxVal[i] = mpu.rawAccel[i];
						change = 1;
					}
				}
			}
		}
	
		if (change) 
		{
			
			for (i = 0; i < 3; i++) 
			{
				if(mag)
				{
					printf("\rX %d|%d|%d    Y %d|%d|%d    Z %d|%d|%d             ",
					minVal[0], mpu.rawMag[0], maxVal[0],
					minVal[1], mpu.rawMag[1], maxVal[1],
					minVal[2], mpu.rawMag[2], maxVal[2]);
				}
					else
				{
					printf("\rX %d|%d|%d    Y %d|%d|%d    Z %d|%d|%d             ",
					minVal[0], mpu.rawAccel[0], maxVal[0],
					minVal[1], mpu.rawAccel[1], maxVal[1],
					minVal[2], mpu.rawAccel[2], maxVal[2]);

				}
			}

			fflush(stdout);
		}

		linux_delay_ms(loop_delay);
	}

	mpu9150_exit();
	
	return 0;
}


int main (int argc, char **argv) {
	
	// local variables
	t_24c16 eeprom;
	t_eeprom_data data;
	
	
	int exit_code=0;
	int result;
	int c;
	int i;
	int ch;
	char zero[1]={0x00};
	


	// usage message
	const char* Usage = "\n"\
	"  -c              calibrate sensors\n"\
	"  -s [serial]     write serial number (6 characters)\n"\
    "  -i              initialize EEPROM. All values will be cleared !!!\n"\
	"\n";
	
	// disable buffering 
	setbuf(stdout, NULL);
	
	// print banner
	printf("sensorcal V%c.%c RELEASE %c build: %s %s\n", VERSION_MAJOR, VERSION_MINOR, VERSION_RELEASE,  __DATE__, __TIME__);
	printf("sensorcal Copyright (C) 2014  see AUTHORS on www.openvario.org\n");
	printf("This program comes with ABSOLUTELY NO WARRANTY;\n");
	printf("This is free software, and you are welcome to redistribute it under certain conditions;\n"); 
	
	fflush(stdout);
	
	// open eeprom object
	result = eeprom_open(&eeprom, 0x50);
	if (result != 0)
	{
		printf("No EEPROM found !!\n");
		exit(1);
	}
		
	// check commandline arguments
	while ((c = getopt (argc, argv, "his:cde")) != -1)
	{
		switch (c) {
			case 'h':
				printf("Usage: sensorcal [OPTION]\n%s",Usage);
				break;
				
			case 'i':
				printf("Initialize EEPROM ...\n");
				for (i=0; i<128; i++)
				{
					result = eeprom_write(&eeprom, &zero[0], i, 1);
				}
				strcpy(data.header, "OV");
				data.data_version = EEPROM_DATA_VERSION;
				strcpy(data.serial, "000000");
				data.zero_offset	=  0.0;
				
				for (i = 0; i < 3; i++) 
				{
					data.accel_min[i] = 0x0000;
					data.accel_max[i] = 0x0000;
					data.mag_min[i] = 0x0000;
					data.mag_max[i] = 0x0000;
				}
				
				update_checksum(&data);
				printf("Writing data to EEPROM ...\n");
				result = eeprom_write(&eeprom, (char*)&data, 0x00, sizeof(data));
				break;
			
			case 'c':
				// read actual EEPROM values
				printf("Reading EEPROM values ...\n\n");
				if(eeprom_read_data(&eeprom, &data) == 0)
				{

					printf("1. Differential pressure calibration\n");
					printf("=====================================\n");
					printf("Press any key to continue, or 'ESC' to skip\n\n");
					
					ch = get_keypress_blocking();
					if(ch == 27)
						printf("Skipped.\n\n");
					else
						calibrate_ams5915(&data);
						
					
					printf("2. Accelerometer calibration\n");
					printf("=============================\n");
					printf("The calibration routine will require you to turn your OpenVario through 360 degrees in three dimensions!\n");
					printf("If you cannot do this, press 'ESC' to cancel now.\n");
					printf("Press any key to continue, or 'ESC' to skip\n\n");
					ch = get_keypress_blocking();
					if(ch == 27)
						printf("Skipped.\n\n");
					else
						if(calibrate_mpu9150(&data, 0))
							printf("Skipped.\n\n");
					
					printf("3. Magnetometer calibration\n");
					printf("==========================\n");
					printf("The calibration routine will require you to turn your OpenVario through 360 degrees in three dimensions!\n");
					printf("If you cannot do this, press 'ESC' to cancel now. \n");
					printf("Press any key to continue, or 'ESC' to skip\n\n");
					
					ch = get_keypress_blocking();
					if(ch == 27)
						printf("Skipped.\n\n");
					else
						if(calibrate_mpu9150(&data, 1))
							printf("Skipped.\n\n");				
					
					printf("New pressure offset: %f\n",(data.zero_offset));
					
					for (i = 0; i < 3; i++) 
					{
						printf("New accel min[%d]: \t%d\n", i, data.accel_min[i]);
						printf("New accel max[%d]: \t%d\n", i, data.accel_max[i]);
						printf("New mag min[%d]: \t%d\n", i, data.mag_min[i]);
						printf("New mag max[%d]: \t%d\n", i, data.mag_max[i]);
					}
					
					// update EEPROM to latest data version
					data.data_version = EEPROM_DATA_VERSION;				
					
					update_checksum(&data);
					printf("Writing data to EEPROM ...\n");
					result = eeprom_write(&eeprom, (char*)&data, 0x00, sizeof(data));
				}
				else
				{
					printf("EEPROM content not valid !!\n");
					printf("Please use -i to initialize EEPROM !!\n");
					exit_code=2;
					break;
				}						
				break;
			case 'e':
				// delete complete EEPROM
				printf("Delete whole EEPROM ...\n\n");
				for (i=0; i< sizeof(data); i++)
				{
						result = eeprom_write(&eeprom, &zero[0], 0x00, 1);
				}
				printf("EEPROM cleared !!\n");
				exit_code=3;
				printf("End ...\n");
				exit(exit_code);
				break;
			case 'd':
				// read actual EEPROM values
				printf("Reading EEPROM values ...\n\n");
				if( eeprom_read_data(&eeprom, &data) == 0)
				{
					printf("Actual EEPROM values:\n");
					printf("---------------------\n");
					printf("Serial: \t\t\t%s\n", data.serial);
					printf("Differential pressure offset:\t%f\n",data.zero_offset);
					switch(data.data_version)
					{
						case 1:
							printf("*** No IMU values stored. Please re-calibrate ***\n");
							break;
						case 2:
						case 0:
						default:
							for (i = 0; i < 3; i++) 
							{
								printf("Accel min[%d]: \t%d\n", i, data.accel_min[i]);
								printf("Accel max[%d]: \t%d\n", i, data.accel_max[i]);
								printf("Mag min[%d]: \t%d\n", i, data.mag_min[i]);
								printf("Mag max[%d]: \t%d\n", i, data.mag_max[i]);
							}
							break;
					}
						
				}
				else
				{
					printf("EEPROM content not valid !!\n");
					printf("Please use -i to initialize EEPROM !!\n");
					exit_code=2;
					break;
				}
				printf("End ...\n");
				exit(exit_code);
				break;
				
			case 's':
				if( strlen(optarg) == 6)
				{
					// read actual EEPROM values	
					if( eeprom_read_data(&eeprom, &data) == 0)
					{
						for(i=0; i<7;i++)
						{
							data.serial[i]=*optarg;
							optarg++;
						}
						data.serial[7]='\n';
						printf("New Serial number: %s\n",data.serial);
						update_checksum(&data);
						printf("Writing data to EEPROM ...\n");
						result = eeprom_write(&eeprom, (char*)&data, 0x00, sizeof(data));
					}
					else
					{
						printf("EEPROM content not valid !!\n");
						printf("Please use -i to initialize EEPROM !!\n");
						exit_code=2;
						break;
					}
				}
				else
				{
					printf("ERROR: Serialnumber has to have exactly 6 characters !!\n");
					exit_code=1;
					break;
				}
				break;
				
			case '?':
			default:
				printf("Unknown option %c\n", optopt);
				printf("Usage: sensorcal [OPTION]\n%s",Usage);
				printf("Exiting ...\n");
		}
	}
		
	printf("End ...\n");
	return(exit_code);
}

int get_keypress_blocking(void) {
      int c=0;

      struct termios org_opts, new_opts;
      int res=0;
          //-----  store old settings -----------
      res=tcgetattr(STDIN_FILENO, &org_opts);
      assert(res==0);
          //---- set new terminal parms --------
      memcpy(&new_opts, &org_opts, sizeof(new_opts));
      new_opts.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ECHOPRT | ECHOKE | ICRNL);
      tcsetattr(STDIN_FILENO, TCSANOW, &new_opts);
      c=getchar();
          //------  restore old settings ---------
      res=tcsetattr(STDIN_FILENO, TCSANOW, &org_opts);
      assert(res==0);
      return(c);
}

int get_keypress_nonblocking(void)
{
  struct termios oldt, newt;
  int ch;
  int oldf;
 
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
 
  ch = getchar();
 
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);
 
  if(ch != EOF)
  {
    ungetc(ch, stdin);
    return 1;
  }
 
  return 0;
}
	
