
/***************************************************************************
 *cr
 *cr            (C) Copyright 2010 The Board of Trustees of the
 *cr                        University of Illinois
 *cr                         All Rights Reserved
 *cr
 ***************************************************************************/

#include <assert.h>
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <parboil.h>

#include "file.h"
#include "common.h"

#define CHECK_ERROR(errorMessage)           \
  if(clStatus != CL_SUCCESS)                \
  {                                         \
     printf("Error: %s!\n",errorMessage);   \
     printf("Line: %d\n",__LINE__);         \
     exit(1);                               \
  }

static int read_data(float *A0, int nx,int ny,int nz,FILE *fp) 
{	
	int s=0;
	int i,j,k;
	for(i=0;i<nz;i++)
	{
		for(j=0;j<ny;j++)
		{
			for(k=0;k<nx;k++)
			{
                                fread(A0+s,sizeof(float),1,fp);
				s++;
			}
		}
	}
	return 0;
}


int main(int argc, char** argv) {
	struct pb_TimerSet timers;
	struct pb_Parameters *parameters;
	
	printf("OpenCL accelerated 7 points stencil codes****\n");
	printf("Author: Li-Wen Chang <lchang20@illinois.edu>\n");
	parameters = pb_ReadParameters(&argc, argv);

	pb_InitializeTimerSet(&timers);
	pb_SwitchToTimer(&timers, pb_TimerID_COMPUTE);
	
	//declaration
	int nx,ny,nz;
	int size;
 	int iteration;
	float c0=1.0f/6.0f;
	float c1=1.0f/6.0f/6.0f;

	if (argc<5) 
    	{
	     printf("Usage: probe nx ny nz t\n"
	     "nx: the grid size x\n"
	     "ny: the grid size y\n"
	     "nz: the grid size z\n"
	     "t: the iteration time");
	     return -1;
	}

	nx = atoi(argv[1]);
	if (nx<1)
		return -1;
	ny = atoi(argv[2]);
	if (ny<1)
		return -1;
	nz = atoi(argv[3]);
	if (nz<1)
		return -1;
	iteration = atoi(argv[4]);
	if(iteration<1)
		return -1;
	
  // Below is the new interface, coupled with Parboil runtime.
  pb_Context* pb_context;
  pb_context = pb_InitOpenCLContext(parameters);
  if (pb_context == NULL) {
    fprintf (stderr, "Error: No OpenCL platform/device can be found."); 
    return -1;
  }

  cl_int clStatus;
  cl_context clContext;
  cl_device_id clDevice;
  cl_platform_id clPlatform;

  // okay, let's deliver actual variables
  clPlatform = (cl_platform_id) pb_context->clPlatformId;
  clContext = (cl_context) pb_context->clContext;
  clDevice = (cl_device_id) pb_context->clDeviceId;

	cl_command_queue clCommandQueue = clCreateCommandQueue(clContext,clDevice,CL_QUEUE_PROFILING_ENABLE,&clStatus);
	CHECK_ERROR("clCreateCommandQueue")

  pb_SetOpenCL(&clContext, &clCommandQueue);

	const char* clSource[] = {readFile("src/opencl_mxpa/kernel.cl")};
	cl_program clProgram = clCreateProgramWithSource(clContext,1,clSource,NULL,&clStatus);
	CHECK_ERROR("clCreateProgramWithSource")

	char clOptions[50];
	sprintf(clOptions,"-I src/opencl_mxpa");
	clStatus = clBuildProgram(clProgram,1,&clDevice,clOptions,NULL,NULL);
	if (clStatus != CL_SUCCESS) {
	  size_t error_size = 0;
    clGetProgramBuildInfo(clProgram, clDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &error_size);
    char *error_value = (char *)malloc(error_size);
    clGetProgramBuildInfo(clProgram, clDevice, CL_PROGRAM_BUILD_LOG, error_size, error_value, NULL);
    puts(error_value);
  }
	CHECK_ERROR("clBuildProgram")

	cl_kernel clKernel = clCreateKernel(clProgram,"block2D_reg_tiling",&clStatus);
	CHECK_ERROR("clCreateKernel") 			

	//host data
	float *h_A0;
	float *h_Anext;
	
	//device
	cl_mem d_A0;
	cl_mem d_Anext;

	//load data from files
	size=nx*ny*nz;
	
	h_A0=(float*)malloc(sizeof(float)*size);
	h_Anext=(float*)malloc(sizeof(float)*size);
	pb_SwitchToTimer(&timers, pb_TimerID_IO);
  FILE *fp = fopen(parameters->inpFiles[0], "rb");
	read_data(h_A0, nx,ny,nz,fp);
  fclose(fp);
  memcpy (h_Anext,h_A0,sizeof(float)*size);
 

	pb_SwitchToTimer(&timers, pb_TimerID_COPY);
	
	//memory allocation
	d_A0 = clCreateBuffer(clContext,CL_MEM_READ_WRITE,size*sizeof(float),NULL,&clStatus);
	CHECK_ERROR("clCreateBuffer")
	d_Anext = clCreateBuffer(clContext,CL_MEM_READ_WRITE,size*sizeof(float),NULL,&clStatus);
	CHECK_ERROR("clCreateBuffer")	
	
	//memory copy
	clStatus = clEnqueueWriteBuffer(clCommandQueue,d_A0,CL_FALSE,0,size*sizeof(float),h_A0,0,NULL,NULL);
	CHECK_ERROR("clEnqueueWriteBuffer")
	clStatus = clEnqueueWriteBuffer(clCommandQueue,d_Anext,CL_TRUE,0,size*sizeof(float),h_Anext,0,NULL,NULL);
	CHECK_ERROR("clEnqueueWriteBuffer")
	
	pb_SwitchToTimer(&timers, pb_TimerID_COMPUTE);

	//only use tx-by-ty threads
	size_t tx = 512;
	size_t ty = 2;
	size_t block[3] = {tx,ty,1};
	size_t grid[3] = {(nx+tx-1)/tx*block[0],(ny+ty-1)/ty*block[1],1};

	clStatus = clSetKernelArg(clKernel,0,sizeof(float),(void*)&c0);
	clStatus = clSetKernelArg(clKernel,1,sizeof(float),(void*)&c1);
	clStatus = clSetKernelArg(clKernel,2,sizeof(cl_mem),(void*)&d_A0);
	clStatus = clSetKernelArg(clKernel,3,sizeof(cl_mem),(void*)&d_Anext);
	clStatus = clSetKernelArg(clKernel,4,sizeof(int),(void*)&nx);
	clStatus = clSetKernelArg(clKernel,5,sizeof(int),(void*)&ny);
	clStatus = clSetKernelArg(clKernel,6,sizeof(int),(void*)&nz);
	CHECK_ERROR("clSetKernelArg")

	//main execution
	pb_SwitchToTimer(&timers, pb_TimerID_KERNEL);

	int t;
	for(t=0;t<iteration;t++)
	{
		clStatus = clEnqueueNDRangeKernel(clCommandQueue,clKernel,3,NULL,grid,block,0,NULL,NULL);
		CHECK_ERROR("clEnqueueNDRangeKernel")
    cl_mem d_temp = d_A0;
    d_A0 = d_Anext;
    d_Anext = d_temp;
    clStatus = clSetKernelArg(clKernel,2,sizeof(cl_mem),(void*)&d_A0);
    clStatus = clSetKernelArg(clKernel,3,sizeof(cl_mem),(void*)&d_Anext);

	}

  cl_mem d_temp = d_A0;
  d_A0 = d_Anext;
  d_Anext = d_temp;


	clStatus = clFinish(clCommandQueue);
	CHECK_ERROR("clFinish")
	
	pb_SwitchToTimer(&timers, pb_TimerID_COPY);
	clStatus = clEnqueueReadBuffer(clCommandQueue,d_Anext,CL_TRUE,0,size*sizeof(float),h_Anext,0,NULL,NULL);
	CHECK_ERROR("clEnqueueReadBuffer")

 	clStatus = clReleaseMemObject(d_A0);
	clStatus = clReleaseMemObject(d_Anext);
	clStatus = clReleaseKernel(clKernel);
	clStatus = clReleaseProgram(clProgram);
	clStatus = clReleaseCommandQueue(clCommandQueue);
	clStatus = clReleaseContext(clContext);
	CHECK_ERROR("clReleaseContext")
 
	if (parameters->outFile) {
		pb_SwitchToTimer(&timers, pb_TimerID_IO);
		outputData(parameters->outFile,h_Anext,nx,ny,nz);
		
	}
	pb_SwitchToTimer(&timers, pb_TimerID_COMPUTE);
		
	free((void*)clSource[0]);

	free(h_A0);
	free(h_Anext);
	pb_SwitchToTimer(&timers, pb_TimerID_NONE);

	pb_PrintTimerSet(&timers);
	pb_FreeParameters(parameters);

	return 0;
}
