/**
*			Copyright (C) 2008-2018 HPDCS Group
*			http://www.dis.uniroma1.it/~hpdcs
*
*
* This file is part of ROOT-Sim (ROme OpTimistic Simulator).
*
* ROOT-Sim is free software; you can redistribute it and/or modify it under the
* terms of the GNU General Public License as published by the Free Software
* Foundation; only version 3 of the License applies.
*
* ROOT-Sim is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
* A PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with
* ROOT-Sim; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
* @file power.c
* @brief This module implements power management facilities
* @author Stefano Conoci
* @date 23/01/2018
*/

#include <core/power.h>

#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

// Number of logical cores. Detected at startup and used to apply DVFS setting for all cores
static int nb_cores;			

// Number of physical cores. Detected at startup and needed to allow per-core DVFS settings when HT is enabled
static int nb_phys_cores;		

// Number of system package. Necessary to monitor energy consumption of all packages in th system
static int nb_packages;			

// Array of P-states initialized at startup which associates a frequency to each P-state 
static int* pstate;				

// Index of the highest available P-state for the system 
static int max_pstate;			

// Array of P-state values for all physical cores  
static int* current_pstates; 				

// Defines if frequency boosting (such as TurboBoost) is either enabled or disabled 
static int boost;		


/**
* This function initializes variables and data structure needed for power management.
* It sets the governor to userspace, reads the number of packages available in the system,
* inits the pstate array which associated P-states to CPU frequencies and reads the
* current P-state setting for each physical core.  
*
* @Author: Stefano Conoci
*/
static int init_DVFS_management(){
	char fname[64];
	char* freq_available, filename;
	int frequency, i, package_last_core, read_frequency;
	FILE* governor_file, numafile, frequency_file;
	uint32_t registers[4];

	// Retrieve number of virtual and physical cores, checking at runtime if hyperthreading is enabled
	nb_cores = sysconf(_SC_NPROCESSORS_ONLN);
	__asm__ __volatile__ ("cpuid " :
                  "=a" (registers[0]),
                  "=b" (registers[1]),
                  "=c" (registers[2]),
                  "=d" (registers[3])
                  : "a" (1), "c" (0));

	unsigned int CPUFeatureSet = registers[3];
	unsigned int hyperthreading = CPUFeatureSet & (1 << 28);
	if(hyperthreading)
		nb_phys_cores = nb_cores/2;
	 else 
		nb_phys_cores = nb_cores;
	#ifdef DEBUG_POWER
		printf("Virtual cores = %d - Physical cores = %d\n - Hyperthreading: %d", nb_cores, nb_phys_cores, hyperthreading);
	#endif

	// Set governor of all cores to userspace
	for(i=0; i<nb_cores;i++){
		sprintf(fname, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
		governor_file = fopen(fname,"w+");
		if(governor_file == NULL){
			printf("Error opening cpu%d scaling_governor file. Must be superuser\n", i);
			exit(0);		
		}		
		fprintf(governor_file, "userspace");
		fflush(governor_file);
		fclose(governor_file);
	}

	// Read number of packages in the system
	filename = malloc(sizeof(char)*64); 
	sprintf(filename,"/sys/devices/system/cpu/cpu%d/topology/physical_package_id", nb_cores-1);
	numafile = fopen(filename,"r");
	if (numafile == NULL){
		printf("Cannot read number of packages\n");
		exit(1);
	} 
	fscanf(numafile ,"%d", &package_last_core);
	nb_packages = package_last_core+1;
	free(filename);

	// Init array that associates frequencies to P-states
	FILE* available_freq_file = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies","r");
	if(available_freq_file == NULL){
		printf("Cannot open scaling_available_frequencies file\n");
		exit(0);
	}
	freq_available = malloc(sizeof(char)*256);
	fgets(freq_available, 256, available_freq_file);
	pstate = malloc(sizeof(int)*32);
	i = 0; 
	char * end;
	for (frequency = strtol(freq_available, &end, 10); freq_available != end; frequency = strtol(freq_available, &end, 10)){
		pstate[i]=frequency;
		freq_available = end;
  		i++;
	}
  	max_pstate = --i;
  	free(freq_available);

  	// Retrieve current P-state for all cores
  	current_pstates = malloc(sizeof(int)*nb_phys_cores);
	for(i=0; i<nb_phys_cores; i++){
		sprintf(fname, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed", i);
		frequency_file = fopen(fname,"r");
		if(frequency_file == NULL){
			printf("Error opening cpu%d scaling_setspeed file. Must be superuser\n", i);
			exit(0);		
		}		
		fscanf(frequency_file, "%d", &read_frequency);
		fflush(frequency_file);
		fclose(frequency_file);

		int found = 0; 
		for(int c = 0; c <= max_pstate && !found; c++){
			if(pstate[c] == read_frequency){
				found = 1;
				current_pstates[i] = c;
				printf("Core %d P-state %d\n", i, c);
			}
		}
	}

	#ifdef DEBUG_POWER
  		printf("Found %d p-states in the range from %d MHz to %d MHz\n", max_pstate, pstate[max_pstate]/1000, pstate[0]/1000);
  	#endif
  	fclose(available_freq_file);

	return 0;
}



/**
* This function sets all cores in the system to the P-state passed as parameter.
*
* @Author: Stefano Conoci
*/
static int set_pstate(int input_pstate){
	
	int i;
	char fname[64];
	FILE* frequency_file;

	#ifdef OVERHEAD_POWER
		long time_heuristic_start;
		long time_heuristic_end;
		double time_heuristic_microseconds;

		time_heuristic_start = get_time();
	#endif 
	
	if(input_pstate > max_pstate)
		return -1;
		
	int frequency = pstate[input_pstate];

	for(i=0; i<nb_cores; i++){
		sprintf(fname, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed", i);
		frequency_file = fopen(fname,"w+");
		if(frequency_file == NULL){
			printf("Error opening cpu%d scaling_setspeed file. Must be superuser\n", i);
			exit(0);		
		}		
		fprintf(frequency_file, "%d", frequency);
		if(i < nb_phys_cores)
			current_pstates[i] = input_pstate;
		fflush(frequency_file);
		fclose(frequency_file);
	}

	#ifdef OVERHEAD_POWER
		time_heuristic_end = get_time();
		time_heuristic_microseconds = (((double) time_heuristic_end) - ((double) time_heuristic_start))/1000;
		printf("OVERHEAD_POWER -  set_pstate() %lf microseconds\n", time_heuristic_microseconds);
	#endif 
	
	return 0;
}

/**
* This function sets the P-state of one core, and if supported its virtual sibling, to the passed value
*
* @Author: Stefano Conoci
*/	
int set_core_pstate(int core, int input_pstate){
		
		int i;
		char fname[64];
		FILE* frequency_file;

		#ifdef OVERHEAD_POWER
			long time_heuristic_start;
			long time_heuristic_end;
			double time_heuristic_microseconds;

			time_heuristic_start = get_time();
		#endif 
		
		if(input_pstate > max_pstate)
			return -1;

		if(current_pstates[core] != input_pstate){
			int frequency = pstate[input_pstate];
			
			// Changing frequency to the physical core
			sprintf(fname, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed", core);
			frequency_file = fopen(fname,"w+");
			if(frequency_file == NULL){
				printf("Error opening cpu%d scaling_setspeed file. Must be superuser\n", i);
				exit(0);		
			}		
			fprintf(frequency_file, "%d", frequency);
			current_pstates[core] = input_pstate;
			fflush(frequency_file);
			fclose(frequency_file);

			// Changing frequency to the Hyperthreading sibling, if enabled on the system
			if(nb_cores != nb_phys_cores){
				sprintf(fname, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed", core+nb_phys_cores);
				frequency_file = fopen(fname,"w+");
				if(frequency_file == NULL){
					printf("Error opening cpu%d scaling_setspeed file. Must be superuser\n", i);
					exit(0);		
				}		
				fprintf(frequency_file, "%d", frequency);
				fflush(frequency_file);
				fclose(frequency_file);
			}
		}

		#ifdef OVERHEAD_POWER
			time_heuristic_end = get_time();
			time_heuristic_microseconds = (((double) time_heuristic_end) - ((double) time_heuristic_start))/1000;
			printf("OVERHEAD_POWER - inside set_core_pstate() %lf microseconds\n", time_heuristic_microseconds);
		#endif 
		
		return 0;
	}

/**
* This function returns the sum of the energy counters of all packages, expressed in micro Joule. 
* Can be used to compute the energy consumption in a time interval, and consequently, the power consumption. 
*
* @Author: Stefano Conoci
*/
static long get_energy(){

	#ifdef OVERHEAD_POWER
		long time_heuristic_start;
		long time_heuristic_end;
		double time_heuristic_microseconds;

		time_heuristic_start = get_time();
	#endif 
	
	long energy;
	int i;
	FILE* energy_file;
	long total_energy = 0;
	char fname[64];

	// Sum the energy counters for all packages 
	for(i = 0; i<nb_packages; i++){

		sprintf(fname, "/sys/class/powercap/intel-rapl/intel-rapl:%d/energy_uj", i);
		energy_file = fopen(fname, "r");
		
		if(energy_file == NULL){
			printf("Error opening energy file\n");		
		}
		fscanf(energy_file,"%ld",&energy);
		fclose(energy_file);
		total_energy+=energy;
	}

	#ifdef OVERHEAD_POWER
		time_heuristic_end = get_time();
		time_heuristic_microseconds = (((double) time_heuristic_end) - ((double) time_heuristic_start))/1000;
		printf("OVERHEAD_POWER - get_energy(): %lf microseconds\n", time_heuristic_microseconds);
	#endif 

	return total_energy;
}

/**
* This function returns time as a monotomically increasing long, expressed in nanoseconds.
* Unlike some other techniques for reading time, this is not influenced by changes in CPU frequency.
*
* @Author: Stefano Conoci
*/
long get_time(){
	
	long time =0;
	struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    time += (ts.tv_sec*1000000000);
    time += ts.tv_nsec;
	return time;
}

/**
* This function can be used to enable or disable frequency boosting techniques,
* such as TurboBoost for Intel CPUs. Frequency boosting is managed by hardware when
* running at P-state 0. 
*
* @Author: Stefano Conoci
*/
static inline void set_boost(int value){

	int i;
	char fname[64];
	FILE* boost_file;

	#ifdef OVERHEAD_POWER
		long time_heuristic_start;
		long time_heuristic_end;
		double time_heuristic_microseconds;

		time_heuristic_start = get_time();
	#endif 
	
	if(value != 0 && value != 1){
		printf("Set_boost parameter invalid. Shutting down application\n");
		exit(1);
	}
	
	boost_file = fopen("/sys/devices/system/cpu/cpufreq/boost", "w+");
	fprintf(boost_file, "%d", value);
	fflush(boost_file);
	fclose(boost_file);

	#ifdef OVERHEAD_POWER
		time_heuristic_end = get_time();
		time_heuristic_microseconds = (((double) time_heuristic_end) - ((double) time_heuristic_start))/1000;
		printf("OVERHEAD_POWER - set_boost() %lf microseconds\n", time_heuristic_microseconds);
	#endif 
	
	return;
}

/**
* This function frees dymically allocated memory. Should be used at shutdown
* @Author: Stefano Conoci
*/
static void free_memory(){

	free(pstate);
	free(current_pstates);
}

