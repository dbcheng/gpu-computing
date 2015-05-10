#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <sstream>

#include <cuda_runtime.h>

#include "cluster_cuda.cuh"

using namespace std;

/*
NOTE: You can use this macro to easily check cuda error codes
and get more information.

Modified from:
http://stackoverflow.com/questions/14038589/
what-is-the-canonical-way-to-check-for-errors-using-the-cuda-runtime-api
*/
#define gpuErrChk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code,
                      const char *file,
                      int line,
                      bool abort=true) {
  if (code != cudaSuccess) {
    fprintf(stderr,"GPUassert: %s %s %d\n",
            cudaGetErrorString(code), file, line);
    exit(code);
  }
}

// timing setup code
cudaEvent_t start;
cudaEvent_t stop;

#define START_TIMER() {                         \
      gpuErrChk(cudaEventCreate(&start));       \
      gpuErrChk(cudaEventCreate(&stop));        \
      gpuErrChk(cudaEventRecord(start));        \
    }

#define STOP_RECORD_TIMER(name) {                           \
      gpuErrChk(cudaEventRecord(stop));                     \
      gpuErrChk(cudaEventSynchronize(stop));                \
      gpuErrChk(cudaEventElapsedTime(&name, start, stop));  \
      gpuErrChk(cudaEventDestroy(start));                   \
      gpuErrChk(cudaEventDestroy(stop));                    \
  }

////////////////////////////////////////////////////////////////////////////////
// Start non boilerplate code

// Fills output with standard normal data
void gaussianFill(float *output, int size) {
  std::default_random_engine generator;
  std::normal_distribution<float> distribution(0.0, 1.0);
  for (int i=0; i < size; i++) {
    output[i] = distribution(generator);
  }
}

// Takes a string of comma seperated floats and stores the float values into
// output. Each string should consist of REVIEW_DIM floats.
void readLSAReview(string review_str, float *output) {
  stringstream stream(review_str);
  int component_idx = 0;

  for (string component; getline(stream, component, ','); component_idx++) {
    output[component_idx] = atof(component.c_str());
  }
  assert(component_idx == REVIEW_DIM);
}

// used to pass arguments to printerCallback
struct printerArg {
  int review_idx_start;
  int batch_size;
  int *cluster_assignments;
};

// Prints out which cluster each review in a batch was assigned to.
// TODO: Call with cudaStreamAddCallback (after completing D->H memcpy)
void printerCallback(cudaStream_t stream, cudaError_t status, void *userData) {
  printerArg *arg = static_cast<printerArg *>(userData);

  for (int i=0; i < arg->batch_size; i++) {
    printf("%d: %d\n", 
	   arg->review_idx_start + i, 
	   arg->cluster_assignments[i]);
  }

  delete arg;
}

void cluster(int k, int batch_size) {
  // cluster centers
  float *d_clusters;

  // how many points lie in each cluster
  int *d_cluster_counts;

  // allocate memory for cluster centers and counts
  gpuErrChk(cudaMalloc(&d_clusters, k * REVIEW_DIM * sizeof(float)));
  gpuErrChk(cudaMalloc(&d_cluster_counts, k * sizeof(int)));

  // randomly initialize cluster centers
  float *clusters = new float[k * REVIEW_DIM];
  gaussianFill(clusters, k * REVIEW_DIM);
  gpuErrChk(cudaMemcpy(d_clusters, clusters, k * REVIEW_DIM * sizeof(float),
		       cudaMemcpyHostToDevice));

  // initialize cluster counts to 0
  gpuErrChk(cudaMemset(d_cluster_counts, 0, k * sizeof(int)));
  
  // TODO: allocate copy buffers and streams
  float * data = new float[batch_size * REVIEW_DIM];
  int * output = new int[batch_size];

  float * data1 = new float[batch_size * REVIEW_DIM];
  int * output1 = new int[batch_size];

  cudaStream_t s[2];
  cudaStreamCreate(&s[0]); cudaStreamCreate(&s[1]);
  
  float * d_data;
  int * d_output;
  gpuErrChk(cudaMalloc(&d_data, batch_size * REVIEW_DIM * sizeof(float)));
  gpuErrChk(cudaMalloc(&d_output, batch_size * sizeof(int)));

  float * d_data1;
  int * d_output1;
  gpuErrChk(cudaMalloc(&d_data1, batch_size * REVIEW_DIM * sizeof(float)));
  gpuErrChk(cudaMalloc(&d_output1, batch_size * sizeof(int)));

  printerArg * args = new printerArg;

  printf("DUDE1\n");

  // main loop to process input lines (each line corresponds to a review)
  int review_idx = 0;
  int streamFlag = 0;
  for (string review_str; getline(cin, review_str); review_idx++) {
    // TODO: readLSAReview into appropriate storage
    if (streamFlag == 0)
    {
      readLSAReview(review_str, data + REVIEW_DIM * review_idx);
      if(review_idx + 1 == batch_size)
      {
        cudaMemcpyAsync(d_data, data, REVIEW_DIM * batch_size,
          cudaMemcpyHostToDevice, s[0]);
        cudaCluster(d_clusters, d_cluster_counts, k, d_data,
          d_output, batch_size, s[0]);
        cudaMemcpyAsync(output, d_output, batch_size,
          cudaMemcpyDeviceToHost, s[0]);
        printf("INSIDE1\n");
        args->review_idx_start = 0;
        args->batch_size = batch_size;
        args->cluster_assignments = output;
        cudaStreamAddCallback(s[0], printerCallback, (void*) args, 0);
        printf("INSIDE2\n");
        review_idx = 0;
        streamFlag = 1;
      }
    }
    else
    {
      readLSAReview(review_str, data1 + REVIEW_DIM * review_idx);
      if(review_idx + 1 == batch_size)
      {
        cudaMemcpyAsync(d_data1, data1, REVIEW_DIM * batch_size,
          cudaMemcpyHostToDevice, s[1]);
        cudaCluster(d_clusters, d_cluster_counts, k, d_data1,
          d_output1, batch_size, s[1]);
        cudaMemcpyAsync(output1, d_output1, batch_size,
          cudaMemcpyDeviceToHost, s[1]);

        args->review_idx_start = 1;
        args->batch_size = batch_size;
        args->cluster_assignments = output;

        cudaStreamAddCallback(s[1], printerCallback, (void*) args, 0);
        review_idx = 0;
        streamFlag = 0;
      }
    }

    // printf("DUDE2\n");
    // TODO: if you have filled up a batch, copy H->D, kernel, copy D->H,
    //       and set callback to printerCallback. Will need to allocate
    //       printerArg struct. Do all of this in a stream.
  }

  printf("DUDE3\n");
  // wait for everything to end on GPU before final summary
  gpuErrChk(cudaDeviceSynchronize());

  // retrieve final cluster locations and counts
  int *cluster_counts = new int[k];
  gpuErrChk(cudaMemcpy(cluster_counts, d_cluster_counts, k * sizeof(int), 
		       cudaMemcpyDeviceToHost));
  gpuErrChk(cudaMemcpy(clusters, d_clusters, k * REVIEW_DIM * sizeof(int),
		       cudaMemcpyDeviceToHost));

  printf("DUDE4\n");

  // print cluster summaries
  for (int i=0; i < k; i++) {
    printf("Cluster %d, population %d\n", i, cluster_counts[i]);
    printf("[");
    for (int j=0; j < REVIEW_DIM; j++) {
      printf("%.4e,", clusters[i * REVIEW_DIM + j]);
    }
    printf("]\n\n");
  }

  // free cluster data
  gpuErrChk(cudaFree(d_clusters));
  gpuErrChk(cudaFree(d_cluster_counts));
  delete[] cluster_counts;
  delete[] clusters;

  // TODO: finish freeing memory, destroy streams
  delete[] data;
  delete[] data1;
  delete[] output;
  delete[] output1;

  delete args;

  gpuErrChk(cudaFree(d_data));
  gpuErrChk(cudaFree(d_data1));
  gpuErrChk(cudaFree(d_output));
  gpuErrChk(cudaFree(d_output1));

  cudaStreamDestroy(s[0]);
  cudaStreamDestroy(s[1]);
}

int main() {
  cluster(5, 32);
  return 0;
}
