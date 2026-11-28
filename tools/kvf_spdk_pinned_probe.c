/* Memory-sharing prototype only: no NVMe opens or I/O, never a performance fast path. */
#define _GNU_SOURCE
#include <spdk/env.h>
#include <cuda_runtime_api.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    struct spdk_env_opts opts;spdk_env_opts_init(&opts);
    opts.name="kvfabric_pinned_probe";opts.core_mask="0x4";opts.no_pci=true;
    opts.no_huge=argc>1&&!strcmp(argv[1],"no-huge");opts.mem_size=64;opts.iova_mode="va";
    if(opts.no_huge) opts.iova_mode=NULL;
    else opts.env_context="--legacy-mem";
    if(spdk_env_init(&opts)<0) return 1;
    size_t bytes=1024*1024;unsigned char *p=spdk_dma_zmalloc(bytes,4096,NULL);
    if(!p) return 1;
    cudaError_t rc=cudaSetDevice(0);
    if(rc==cudaSuccess) rc=cudaHostRegister(p,bytes,cudaHostRegisterDefault);
    if(rc!=cudaSuccess) {
        printf("{\"registered\":false,\"cuda_error\":%d,\"message\":\"%s\",\"huge\":%s}\n",rc,cudaGetErrorString(rc),opts.no_huge?"false":"true");
        spdk_dma_free(p);spdk_env_fini();return 77;
    }
    void *gpu=NULL;cudaStream_t stream=NULL;
    rc=cudaMalloc(&gpu,bytes);
    if(rc==cudaSuccess) rc=cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking);
    memset(p,0x5a,bytes);
    if(rc==cudaSuccess) rc=cudaMemcpyAsync(gpu,p,bytes,cudaMemcpyHostToDevice,stream);
    if(rc==cudaSuccess) rc=cudaStreamSynchronize(stream);
    if(rc==cudaSuccess) {
        memset(p,0,bytes);rc=cudaMemcpyAsync(p,gpu,bytes,cudaMemcpyDeviceToHost,stream);
    }
    cudaError_t stopped=cudaStreamSynchronize(stream);
    if(stopped!=cudaSuccess) {
        printf("{\"registered\":true,\"dma_confirmed\":false,\"quarantined\":true}\n");
        return 1; /* Neither allocation nor env is freed with unconfirmed GPU DMA. */
    }
    int valid=rc==cudaSuccess;
    for(size_t i=0;i<bytes&&valid;++i) valid=p[i]==0x5a;
    printf("{\"registered\":true,\"roundtrip_valid\":%s,\"huge\":%s,\"bytes\":%zu}\n",valid?"true":"false",opts.no_huge?"false":"true",bytes);
    if(stream) cudaStreamDestroy(stream);
    if(gpu) cudaFree(gpu);
    rc=cudaHostUnregister(p);
    if(rc!=cudaSuccess) return 1;
    spdk_dma_free(p);spdk_env_fini();return valid?0:1;
}
