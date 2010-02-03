/*
 * StarPU
 * Copyright (C) INRIA 2008-2010 (see AUTHORS file)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <pthread.h>
#include <stdio.h>
#include <cuda.h>
#include <cuda_runtime.h>

#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>

static pthread_t thread[2];
static unsigned thread_is_initialized[2];

static pthread_cond_t cond;
static pthread_mutex_t mutex;

static size_t buffer_size = 1;
static void *cpu_buffer;
static void *gpu_buffer[2];

static pthread_cond_t cond_go;
static unsigned ready = 0;
static unsigned nready_gpu = 0;

static unsigned niter = 100000;

static pthread_cond_t cond_gpu;
static pthread_mutex_t mutex_gpu;
static unsigned data_is_available[2];

static cudaStream_t stream[2];

#define ASYNC	1

void send_data(unsigned src, unsigned dst)
{
	/* Copy data from GPU to RAM */
#ifdef ASYNC
	cudaMemcpyAsync(cpu_buffer, gpu_buffer[src], buffer_size, cudaMemcpyDeviceToHost, stream[src]);
	cudaStreamSynchronize(stream[src]);
#else
	cudaMemcpy(cpu_buffer, gpu_buffer[src], buffer_size, cudaMemcpyDeviceToHost);
	cudaThreadSynchronize();
#endif

	/* Tell the other GPU that data is in RAM */
	pthread_mutex_lock(&mutex_gpu);
	data_is_available[src] = 0;
	data_is_available[dst] = 1;
	pthread_cond_signal(&cond_gpu);
	pthread_mutex_unlock(&mutex_gpu);
	//fprintf(stderr, "SEND on %d\n", src);
}

void recv_data(unsigned src, unsigned dst)
{
	/* Wait for the data to be in RAM */
	pthread_mutex_lock(&mutex_gpu);
	while (!data_is_available[dst])
	{
		pthread_cond_wait(&cond_gpu, &mutex_gpu);
	}
	pthread_mutex_unlock(&mutex_gpu);
	//fprintf(stderr, "RECV on %d\n", dst);

	/* Upload data */
#ifdef ASYNC
	cudaMemcpyAsync(gpu_buffer[dst], cpu_buffer, buffer_size, cudaMemcpyDeviceToHost, stream[dst]);
	cudaThreadSynchronize();
#else
	cudaMemcpy(gpu_buffer[dst], cpu_buffer, buffer_size, cudaMemcpyDeviceToHost);
	cudaThreadSynchronize();
#endif
}

void *launch_gpu_thread(void *arg)
{
	unsigned *idptr = arg;
	unsigned id = *idptr;

	fprintf(stderr, "Initialize device %d\n", id);
	cudaSetDevice(id);
	cudaFree(0);

	cudaMalloc(&gpu_buffer[id], buffer_size);
	cudaStreamCreate(&stream[id]);

	pthread_mutex_lock(&mutex);
	thread_is_initialized[id] = 1;
	pthread_cond_signal(&cond);

	nready_gpu++;

	while (!ready)
		pthread_cond_wait(&cond_go, &mutex);

	pthread_mutex_unlock(&mutex);

	fprintf(stderr, "Device %d GOGO\n", id);

	unsigned iter;
	for (iter = 0; iter < niter; iter++)
	{
		if (id == 0) {
			send_data(0, 1);
			recv_data(1, 0);
		}
		else {
			recv_data(0, 1);
			send_data(1, 0);
		}
	}

	pthread_mutex_lock(&mutex);
	nready_gpu--;
	pthread_cond_signal(&cond_go);
	pthread_mutex_unlock(&mutex);

	return NULL;
}

int main(int argc, char **argv)
{
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&cond_go, NULL);

	cudaHostAlloc(&cpu_buffer, buffer_size, cudaHostAllocPortable);

	unsigned id;
	for (id = 0; id < 2; id++)
	{
		thread_is_initialized[id] = 0;
		pthread_create(&thread[0], NULL, launch_gpu_thread, &id);

		pthread_mutex_lock(&mutex);
		while (!thread_is_initialized[id])
		{
			 pthread_cond_wait(&cond, &mutex);
		}
		pthread_mutex_unlock(&mutex);
	}

	struct timeval start;
	struct timeval end;

	/* Start the ping pong */
	gettimeofday(&start, NULL);

	pthread_mutex_lock(&mutex);
	ready = 1;
	pthread_cond_broadcast(&cond_go);
	pthread_mutex_unlock(&mutex);

	/* Wait for the end of the ping pong */
	pthread_mutex_lock(&mutex);
	while (nready_gpu > 0)
	{
		pthread_cond_wait(&cond_go, &mutex);
	}
	pthread_mutex_unlock(&mutex);

	gettimeofday(&end, NULL);
	
	double timing = (double)((end.tv_sec - start.tv_sec)*1000000 +
		(end.tv_usec - start.tv_usec));

	fprintf(stderr, "Took %.0f ms for %d iterations\n", timing/1000, niter);
	fprintf(stderr, "Latency: %.2f us\n", timing/(2*niter));

	return 0;
}
