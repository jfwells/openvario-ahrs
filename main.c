/*  sensord - Sensor Interface for XCSoar Glide Computer - http://www.openvario.org/
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

#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <syslog.h>
//#include "version.h"
#include "nmea.h"
//#include "w1.h"
#include "def.h"
#include "KalmanFilter1d.h"

#include "cmdline_parser.h"

#include "ms5611.h"
#include "ams5915.h"
#include "ads1110.h"

#include "vario.h"
#include "AirDensity.h"
#include "24c16.h"

#include "mpu9150.h"
#include "linux_glue.h"
#include "ahrs_settings.h"

#include "configfile_parser.h"

#define I2C_ADDR 0x76
#define PRESSURE_SAMPLE_RATE 	20	// sample rate of pressure values (Hz)
#define TEMP_SAMPLE_RATE 		5	// sample rate of temp values (Hz)
#define NMEA_SLOW_SEND_RATE		2	// NMEA send rate for SLOW Data (pressures, etc..) (Hz)
#define MPU_SAMPLE_RATE			20  // sample rate of MPU9150
#define YAW_MIX_FACTOR			4   // Yaw mix factor for fused mag/accel values
#define I2C_BUS					1
 
#define MEASTIMER (SIGRTMAX)
#define DELTA_TIME_US(T1, T2)	(((T1.tv_sec+1.0e-9*T1.tv_nsec)-(T2.tv_sec+1.0e-9*T2.tv_nsec))*1000000)	

#define OV_PORT 				4353  // Port for OpenVario output
#define AHRS_PORT				2000  // Port for LevilAHRD output
					
timer_t  measTimer;
int g_debug=0;
int g_log=0;

// Sensor objects
t_ms5611 static_sensor;
t_ms5611 tep_sensor;
t_ams5915 dynamic_sensor;
t_ads1110 voltage_sensor;
t_mpu9150 mpu_sensor;
	
// configuration object
t_config config;

// Filter objects
t_kalmanfilter1d vkf;
	
// pressures
float tep;
float p_static;
float p_dynamic;

int g_foreground=TRUE;
int g_secordcomp=FALSE;

t_io_mode io_mode;

FILE *fp_console=NULL;
FILE *fp_sensordata=NULL;
FILE *fp_datalog=NULL;
FILE *fp_config=NULL;

//FILE *fp_rawlog=NULL;

enum e_state { IDLE, TEMP, PRESSURE} state = IDLE;

//typedef enum { measure_only, record, replay} t_measurement_mode;

/**
* @brief Signal handler if sensord will be interrupted
* @param sig_num
* @return 
* 
* Signal handler for catching STRG-C singal from command line
* Closes all open files handles like log files
* @date 17.04.2014 born
*
*/ 
void sigintHandler(int sig_num){

	signal(SIGINT, sigintHandler);
	
	// if meas_mode = record -> close fp now
	if (fp_datalog != NULL)
		fclose(fp_datalog);
	
	// if sensordata from file
	if (fp_sensordata != NULL)
		fclose(fp_sensordata);
		
	//close fp_config if used
	if (fp_config != NULL)
		fclose(fp_config);
	
	//fclose(fp_rawlog);
	printf("Exiting ...\n");
	fclose(fp_console);
	
	mpu9150_exit();
	
	exit(0);
}


/**
* @brief Command handler for NMEA messages
* @param sock Network socket handler
* @return 
* 
* Message handler called by main-loop to generate timing of NMEA messages
* @date 17.04.2014 born
*
*/ 
int NMEA_message_handler(int sock)
{
	// some local variables
	float vario;
	int sock_err;

	static int nmea_counter = 1;
	int result;
	char s[256];
	
	switch (nmea_counter)
	{
		case 5:
		case 10:
		case 15:
		case 20:
		case 25:
		case 30:
		case 35:
		case 40:
			// Compute Vario
			vario = ComputeVario(vkf.x_abs_, vkf.x_vel_);
			
			if (config.output_POV_P_Q == 1)
			{
				// Compose POV slow NMEA sentences
				result = Compose_Pressure_POV_slow(&s[0], p_static/100, p_dynamic*100);
				
				// NMEA sentence valid ?? Otherwise print some error !!
				if (result != 1)
				{
					printf("POV slow NMEA Result = %d\n",result);
				}	
			
				// Send NMEA string via socket to XCSoar
				if ((sock_err = send(sock, s, strlen(s), 0)) < 0)
				{	
					fprintf(stderr, "send failed\n");
					break;
				}
			}
			
			if (config.output_POV_E == 1)
			{
				if (tep_sensor.valid != 1)
				{
					vario = 99;
				}
				// Compose POV slow NMEA sentences
				result = Compose_Pressure_POV_fast(&s[0], vario);
				
				// NMEA sentence valid ?? Otherwise print some error !!
				if (result != 1)
				{
					printf("POV fast NMEA Result = %d\n",result);
				}	
				
				// Send NMEA string via socket to XCSoar
				if ((sock_err = send(sock, s, strlen(s), 0)) < 0)
				{	
					fprintf(stderr, "send failed\n");
					break;
				}
			}
			
			if (config.output_POV_V == 1 && voltage_sensor.present)
			{

				// Compose POV slow NMEA sentences
				result = Compose_Voltage_POV(&s[0], voltage_sensor.voltage_converted);
				
				// NMEA sentence valid ?? Otherwise print some error !!
				if (result != 1)
				{
					printf("POV voltage NMEA Result = %d\n",result);
				}	
				
				// Send NMEA string via socket to XCSoar
				if ((sock_err = send(sock, s, strlen(s), 0)) < 0)
				{	
					fprintf(stderr, "send failed\n");
					break;
				}
			}
			break;
		default:
			break;
	}
	
	// take care for statemachine counter
	if (nmea_counter == 40)
			nmea_counter = 1;
		else
			nmea_counter++;
		
	return(sock_err);
		
}

/**
* @brief Timming routine for pressure measurement
* @param 
* @return 
* 
* Timing handler to coordinate pressure measurement
* @date 17.04.2014 born
*
*/ 
void pressure_measurement_handler(void)
{
	static int meas_counter = 1;
	switch (meas_counter)
	{
		case 1:
		case 5:
		case 9:
		case 13:
		case 17:
		case 21:
		case 25:
		case 29:
		case 33:
		case 37:
			if (io_mode.sensordata_from_file != TRUE)
			{
				// start pressure measurement
				ms5611_start_pressure(&static_sensor);
				ms5611_start_pressure(&tep_sensor);
			}
			break;
		
		case 2:
		case 6:
		case 10:
		case 14:
		case 18:
		case 22:
		case 26:
		case 30:
		case 34:
		case 38:
			if (io_mode.sensordata_from_file != TRUE)
			{
				// read pressure values
				ms5611_read_pressure(&static_sensor);
				ms5611_read_pressure(&tep_sensor);
							
				// read AMS5915
				ams5915_measure(&dynamic_sensor);
				ams5915_calculate(&dynamic_sensor);
				
				// read ADS1110
				if(voltage_sensor.present)
				{
					ads1110_measure(&voltage_sensor);
					ads1110_calculate(&voltage_sensor);
				}
			}
			else
			{
				if (fscanf(fp_sensordata, "%f,%f,%f", &tep_sensor.p, &static_sensor.p, &dynamic_sensor.p) == EOF)
				{
					printf("End of File reached\n");
					printf("Exiting ...\n");
					exit(EXIT_SUCCESS);
				}
			}
			
			//
			// filtering
			//
			// of static pressure
			p_static = (3*p_static + static_sensor.p) / 4;
			
			// check tep_pressure input value for validity
			if ((tep_sensor.p/100 < 100) || (tep_sensor.p/100 > 1200))
			{
				// tep pressure out of range
				tep_sensor.valid = 0;
			}
			else
			{
				// of tep pressure
				KalmanFiler1d_update(&vkf, tep_sensor.p/100, 0.25, 0.05);
			}
			
			// of dynamic pressure
			p_dynamic = (3*p_dynamic + dynamic_sensor.p) / 4;
			//printf("Pdyn: %f\n",p_dynamic*100);
			// mask speeds < 10km/h
			if (p_dynamic < 0.04)
			{
				p_dynamic = 0.0;
			}
				
			// write pressure to file if option is set
			if (io_mode.sensordata_to_file == TRUE)
			{
				fprintf(fp_datalog, "%f,%f,%f\n",  tep_sensor.p/100, static_sensor.p/100, dynamic_sensor.p);
			}
			
			// datalog
			//fprintf(fp_rawlog,"%f,%f,%f\n",tep_sensor.p/100, vkf.x_abs_, vkf.x_vel_);
			
			break;
		case 3:
			// start temp measurement
			ms5611_start_temp(&static_sensor);
			ms5611_start_temp(&tep_sensor);
			break;
		case 4:
			// read temp values
			ms5611_read_temp(&static_sensor);
			ms5611_read_temp(&tep_sensor);
			break;
		default:
			break;
	}
	
	// take care for statemachine counter
	if (meas_counter == 40)
	{
		meas_counter = 1;
		ddebug_print("%s: start new cycle\n", __func__);
	}
	else
	{
		meas_counter++;
	}
}

/**
* AHRS_message: Output in NMEA format as
* expected by XCSoar LevilAHRS Driver:
* $RPYL,Roll,Pitch,MagnHeading,SideSlip,YawRate,G,errorcode
*
* Error bits (not implemented yet):
*   0: Roll gyro test failed  
*   1: Roll gyro test failed 
*   2: Roll gyro test failed 
*   3: Acc X test failed 
*   4: Acc Y test failed 
*   5: Acc Z test failed 
*   6: Watchdog test failed
*   7: Ram test failed
*   8: EEPROM access test failed
*   9: EEPROM checksum test failed
*  10: Flash checksum test failed
*  11: Low voltage error
*  12: High temperature error (>60 C)
*  13: Inconsistent roll data between gyro and acc.
*  14: Inconsistent pitch data between gyro and acc.
*  15: Inconsistent yaw data between gyro and acc.
*/
void AHRS_message(mpudata_t *mpu, t_mpu9150 *mpucal, int sock)
{
	
	int sock_err;
	char s[256];
	
	sprintf(s, "$RPYL,%0.0f,%0.0f,%0.0f,0,0,%0.0f,0\r\n",
			// orientations
	       		((mpu->fusedEuler[VEC3_X] * RAD_TO_DEGREE) + mpucal->roll_adjust) * 10.,
	       		((mpu->fusedEuler[VEC3_Y] * RAD_TO_DEGREE) + mpucal->pitch_adjust) * 10.,
	       		((mpu->fusedEuler[VEC3_Z] * RAD_TO_DEGREE) + mpucal->yaw_adjust) * 10.,
	
			// sideslip & delta yaw don't seem to be used by XCSoar
			// Magnitude of "G"
			sqrt(
				pow(mpu->calibratedAccel[VEC3_X], 2) + 
				pow(mpu->calibratedAccel[VEC3_Y], 2) +
			    pow(mpu->calibratedAccel[VEC3_Z], 2)
			) * ((mpu->calibratedAccel[VEC3_Z] < 0.) ? -1000. : 1000.)
	);
	
	// Send NMEA string via socket to XCSoar
	if ((sock_err = send(sock, s, strlen(s), 0)) < 0)
	{	
		fprintf(stderr, "send failed\n");
	}
	
	
}

	
int main (int argc, char **argv) {
	
	// local variables
	int i=0;
	int result;
	int sock_err = 0;
	
	int sock_imu_connected = 0;
	struct timeval imu_last_sample, curr_time;
		
	t_24c16 eeprom;
	t_eeprom_data data;
	
	mpudata_t mpu;
	t_mpu9150_cal accel_cal;
	t_mpu9150_cal mag_cal;
	
	// for daemonizing
	pid_t pid;
	pid_t sid;

	io_mode.sensordata_from_file = FALSE;
	io_mode.sensordata_to_file = FALSE;
	
	// signals and action handlers
	struct sigaction sigact;
	
	// socket communication
	int sock;
	int sock_imu;
	struct sockaddr_in server;
	struct sockaddr_in server_imu;
	
	// initialize variables
	static_sensor.offset = 0.0;
	static_sensor.linearity = 1.0;
	
	dynamic_sensor.offset = 0.0;
	dynamic_sensor.linearity = 1.0;
	
	tep_sensor.offset = 0.0;
	tep_sensor.linearity = 1.0;
	
	config.output_POV_E = 0;
	config.output_POV_P_Q = 0;
	
	
	for(i=0;i<3;i++) {
		accel_cal.offset[i] = 0;
		accel_cal.range[i] = 0;
		mag_cal.offset[i] = 0;
		mag_cal.range[i] = 0;
	}	

	mpu_sensor.accel_cal = accel_cal;
	mpu_sensor.mag_cal = mag_cal;
	mpu_sensor.rotation = 1;
	mpu_sensor.roll_adjust = 0.0;
	mpu_sensor.pitch_adjust = 0.0;
	mpu_sensor.yaw_adjust = 0.0;


	
	//open file for raw output
	//fp_rawlog = fopen("raw.log","w");
		
	//parse command line arguments
	cmdline_parser(argc, argv, &io_mode);
	
	// get config file options
	if (fp_config != NULL)
		cfgfile_parser(fp_config, &static_sensor, &tep_sensor, &dynamic_sensor, &voltage_sensor, &mpu_sensor, &config);
	
	// check if we are a daemon or stay in foreground
	if (g_foreground == TRUE)
	{
		// stay in foreground
		// install signal handler for CTRL-C
		sigact.sa_handler = sigintHandler;
		sigemptyset (&sigact.sa_mask);
		sigact.sa_flags = 0;
		sigaction(SIGINT, &sigact, NULL);
		
		// open console again, but as file_pointer
		fp_console = stdout;
		stderr = stdout;
		setbuf(fp_console, NULL);
		setbuf(stderr, NULL);
		
		// close the standard file descriptors
		close(STDIN_FILENO);
		//close(STDOUT_FILENO);
		close(STDERR_FILENO);	
	}
	else
	{
		// implement handler for kill command
		printf("Daemonizing ...\n");
		pid = fork();
		
		// something went wrong when forking
		if (pid < 0) 
		{
			exit(EXIT_FAILURE);
		}
		
		// we are the parent
		if (pid > 0)
		{
			exit(EXIT_SUCCESS);
		}
		
		// set umask to zero
		umask(0);
				
		/* Try to create our own process group */
		sid = setsid();
		if (sid < 0) {
			syslog(LOG_ERR, "Could not create process group\n");
			exit(EXIT_FAILURE);
		}
		
		// close the standard file descriptors
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		
		//open file for log output
		fp_console = fopen("sensord.log","w+");
		stderr = fp_console;
		setbuf(fp_console, NULL);
	}
		
	// ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);
	
	// get config from EEPROM
	// open eeprom object
	result = eeprom_open(&eeprom, 0x50);
	if (result != 0)
	{
		printf("No EEPROM found !!\n");
	}
	else
	{
		if( eeprom_read_data(&eeprom, &data) == 0)
		{
			fprintf(fp_console,"Using EEPROM calibration values ...\n");
			dynamic_sensor.offset = data.zero_offset;
			
			// IMU calibration values; calculate range and offsets
			for(i=0;i<3;i++) {
				mpu_sensor.accel_cal.offset[i] = (short)((data.accel_min[i] + data.accel_max[i]) / 2);
				mpu_sensor.accel_cal.range[i] = (short)(data.accel_max[i] - mpu_sensor.accel_cal.offset[i]);
				mpu_sensor.mag_cal.offset[i] = (short)((data.mag_min[i] + data.mag_max[i]) / 2);
				mpu_sensor.mag_cal.range[i] = (short)(data.mag_max[i] - mpu_sensor.mag_cal.offset[i]);
			}
			
		}
		else
		{
			fprintf(stderr, "EEPROM Checksum wrong !!\n");
		}
	}
	
	// print runtime config
	print_runtime_config();
	
	if (io_mode.sensordata_from_file != TRUE)
	{
		// we need hardware sensors for running !!
		// open sensor for static pressure
		/// @todo remove hardcoded i2c address static pressure
		if (ms5611_open(&static_sensor, 0x76) != 0)
		{
			fprintf(stderr, "Open sensor failed !!\n");
			return 1;
		}
		
		//initialize static pressure sensor
		ms5611_reset(&static_sensor);
		usleep(10000);
		ms5611_init(&static_sensor);
		static_sensor.secordcomp = g_secordcomp;
		static_sensor.valid = 1;
				
		// open sensor for velocity pressure
		/// @todo remove hardcoded i2c address for velocity pressure
		if (ms5611_open(&tep_sensor, 0x77) != 0)
		{
			fprintf(stderr, "Open sensor failed !!\n");
			return 1;
		}
		
		//initialize tep pressure sensor
		ms5611_reset(&tep_sensor);
		usleep(10000);
		ms5611_init(&tep_sensor);
		tep_sensor.secordcomp = g_secordcomp;
		tep_sensor.valid = 1;
		
		// open sensor for differential pressure
		/// @todo remove hardcoded i2c address for differential pressure
		if (ams5915_open(&dynamic_sensor, 0x28) != 0)
		{
			fprintf(stderr, "Open sensor failed !!\n");
			return 1;
		}
		
		// open sensor for battery voltage
		/// @todo remove hardcoded i2c address for voltage sensor
		if (ads1110_open(&voltage_sensor, 0x48) != 0)
		{
			fprintf(stderr, "Open sensor failed !!\n");
		}
		
		//initialize differential pressure sensor
		ams5915_init(&dynamic_sensor);
		dynamic_sensor.valid = 1;
		
		//initialize voltage sensor
		if(voltage_sensor.present)
			ads1110_init(&voltage_sensor);
			
		// Initialise MPU
		if (mpu9150_init(I2C_BUS, MPU_SAMPLE_RATE, YAW_MIX_FACTOR, mpu_sensor.rotation))
		{
			fprintf(stderr, "Failed to open MPU9150\n");
		}
		else
		{
			usleep(10000);
			mpu9150_set_accel_cal(&mpu_sensor.accel_cal);
			usleep(10000);
			mpu9150_set_mag_cal(&mpu_sensor.mag_cal);
			usleep(10000);	
			memset(&mpu, 0, sizeof(mpudata_t));
			gettimeofday(&imu_last_sample, NULL);
		}
		
		// poll sensors for offset compensation
		ms5611_start_temp(&static_sensor);
		usleep(10000);
		ms5611_read_temp(&static_sensor);
		ms5611_start_pressure(&static_sensor);
		usleep(10000);
		ms5611_read_pressure(&static_sensor);
	
		ms5611_start_temp(&tep_sensor);
		usleep(10000);
		ms5611_read_temp(&tep_sensor);

		// initialize variables
		p_static = static_sensor.p;
	}
	else
	{
		p_static = 101300.0;
	}
	
	// initialize kalman filter
	KalmanFilter1d_reset(&vkf);
	vkf.var_x_accel_ = config.vario_x_accel;
	
	for(i=0; i < 1000; i++)
		KalmanFiler1d_update(&vkf, p_static/100, 0.25, 1);
			
	while(1)
	{
		// reset sock_err variable
		sock_err = 0;
		
		// Open Socket for TCP/IP communication
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock == -1)
			fprintf(stderr, "could not create socket\n");

		server.sin_addr.s_addr = inet_addr("127.0.0.1");
		server.sin_family = AF_INET;
		server.sin_port = htons(OV_PORT);


		// Open Separate Socket for LevilAHRS driver
		sock_imu = socket(AF_INET, SOCK_STREAM, 0);
		if (sock_imu == -1)
			fprintf(stderr, "could not create IMU socket\n");

		server_imu.sin_addr.s_addr = inet_addr("127.0.0.1");
		server_imu.sin_family = AF_INET;
		server_imu.sin_port = htons(AHRS_PORT);
  
		// try to connect to XCSoar
		while (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) 
		{
			fprintf(stderr, "failed to connect (main socket), trying again\n");
			fflush(stdout);
			sleep(1);
		}
		
		
				
		// socket connected
		// main data acquisition loop
		while(sock_err >= 0)
		{	
			int result;
		
			result = usleep(12500);
			if (result != 0)
			{
				printf("usleep error\n");
				usleep(12500);
			}
			pressure_measurement_handler();
			sock_err = NMEA_message_handler(sock);
			
			if(!sock_imu_connected) 
			{
				if (connect(sock_imu, (struct sockaddr *)&server_imu, sizeof(server_imu)) >= 0) 
					sock_imu_connected = 1;
				else
				{
					fprintf(stderr, "failed to connect (IMU socket)\n");
					fflush(stdout);
				}
			}
			if(sock_imu_connected)
			{
				// compare timer
				gettimeofday(&curr_time, NULL);
				if((curr_time.tv_usec - imu_last_sample.tv_usec) >= (1e6/AHRS_SAMPLE_RATE_HZ))
				{
					imu_last_sample = curr_time;
					if (mpu9150_read(&mpu) == 0)
						AHRS_message(&mpu, &mpu_sensor, sock_imu);
				}
			}
		
		} 
		
		// connection dropped
		close(sock);
		close(sock_imu);
	} // while(1)
	return 0;
}

void print_runtime_config(void)
{
	int i;
	
	// print actual used config
	fprintf(fp_console,"=========================================================================\n");
	fprintf(fp_console,"Runtime Configuration:\n");
	fprintf(fp_console,"----------------------\n");
	fprintf(fp_console,"Vario:\n");
	fprintf(fp_console,"  Kalman Accel:\t%f\n",config.vario_x_accel);
	fprintf(fp_console,"Sensor TEK:\n");
	fprintf(fp_console,"  Offset: \t%f\n",tep_sensor.offset);
	fprintf(fp_console,"  Linearity: \t%f\n", tep_sensor.linearity);
	fprintf(fp_console,"Sensor STATIC:\n");
	fprintf(fp_console,"  Offset: \t%f\n",static_sensor.offset);
	fprintf(fp_console,"  Linearity: \t%f\n", static_sensor.linearity);
	fprintf(fp_console,"Sensor TOTAL:\n");
	fprintf(fp_console,"  Offset: \t%f\n",dynamic_sensor.offset);
	fprintf(fp_console,"  Linearity: \t%f\n", dynamic_sensor.linearity);
	fprintf(fp_console,"Accelerometer:\n");
	for(i=0;i<3;i++) 
	{
		fprintf(fp_console,"Offset %d:\t%d\n", i, mpu_sensor.accel_cal.offset[i]);
		fprintf(fp_console,"Range %d:\t%d\n", i, mpu_sensor.accel_cal.range[i]);
	}
	fprintf(fp_console,"Magnetometer:\n");	
	for(i=0;i<3;i++) 
	{	
		fprintf(fp_console,"Offset %d:\t%d\n", i, mpu_sensor.mag_cal.offset[i]);
		fprintf(fp_console,"Range %d:\t%d\n", i, mpu_sensor.mag_cal.range[i]);
	}
	
	fprintf(fp_console,"Range %d:\t%d\n", i, mpu_sensor.mag_cal.range[i]);
	fprintf(fp_console,"=========================================================================\n");
	
}
 

