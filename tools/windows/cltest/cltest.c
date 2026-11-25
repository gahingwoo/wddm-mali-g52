/*
 * cltest: OpenCL on the Mali-G52 (Mesa rusticl on Panfrost, docs/OPENCL.md).
 *
 *   cltest list                    platforms and devices, as clinfo would, short
 *   cltest vecadd [n]              c = a + b on n floats, checked on the CPU
 *   cltest sgemm [n] [iters]       C = A * B, n x n floats, a naive kernel and
 *                                  a 16 x 16 tiled one, checked on the CPU,
 *                                  timed by profiling events (GFLOP/s)
 *   cltest all                     list, vecadd 1048576, sgemm 256
 *
 * Options, before the command:
 *   --platform S   use the first platform whose name or vendor contains S
 *                  (default: any; rusticl's platform is named "rusticl")
 *   --device S     use the first device whose name contains S
 *   --expect S     fail (exit 3) unless the chosen device's name contains S
 *   --no-verify    run and time, but do not compare with the CPU (for the
 *                  emulated kernel driver in CI, which runs no job)
 *
 * Exit codes: 0 pass, 1 wrong results, 2 an OpenCL call failed, 3 no device
 * that matches.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#define CL_TARGET_OPENCL_VERSION 300
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>
#include <CL/cl_ext.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static const char *opt_platform, *opt_device, *opt_expect;
static int opt_verify = 1;

static double
now_ms(void)
{
#ifdef _WIN32
   LARGE_INTEGER f, c;
   QueryPerformanceFrequency(&f);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}

#define CHECK(call)                                                            \
   do {                                                                        \
      cl_int e_ = (call);                                                      \
      if (e_ != CL_SUCCESS) {                                                  \
         fprintf(stderr, "FAIL %s: %d (line %d)\n", #call, (int)e_, __LINE__); \
         return 2;                                                             \
      }                                                                        \
   } while (0)

static void
pinfo(cl_platform_id p, cl_platform_info what, char *buf, size_t n)
{
   buf[0] = 0;
   clGetPlatformInfo(p, what, n, buf, NULL);
}

static void
dinfo_str(cl_device_id d, cl_device_info what, char *buf, size_t n)
{
   buf[0] = 0;
   clGetDeviceInfo(d, what, n, buf, NULL);
}

static cl_ulong
dinfo_ulong(cl_device_id d, cl_device_info what)
{
   cl_ulong v = 0;
   clGetDeviceInfo(d, what, sizeof v, &v, NULL);
   return v;
}

static cl_uint
dinfo_uint(cl_device_id d, cl_device_info what)
{
   cl_uint v = 0;
   clGetDeviceInfo(d, what, sizeof v, &v, NULL);
   return v;
}

static size_t
dinfo_size(cl_device_id d, cl_device_info what)
{
   size_t v = 0;
   clGetDeviceInfo(d, what, sizeof v, &v, NULL);
   return v;
}

static int
cmd_list(void)
{
   cl_uint np = 0;
   cl_int e = clGetPlatformIDs(0, NULL, &np);
   printf("platforms: %u%s\n", np, e == CL_PLATFORM_NOT_FOUND_KHR ? " (none found by the loader)" : "");
   if (e != CL_SUCCESS || np == 0)
      return e == CL_SUCCESS || e == CL_PLATFORM_NOT_FOUND_KHR ? 0 : 2;
   cl_platform_id *ps = calloc(np, sizeof *ps);
   CHECK(clGetPlatformIDs(np, ps, NULL));
   for (cl_uint i = 0; i < np; i++) {
      char name[256], vendor[256], ver[256], ext[4096];
      pinfo(ps[i], CL_PLATFORM_NAME, name, sizeof name);
      pinfo(ps[i], CL_PLATFORM_VENDOR, vendor, sizeof vendor);
      pinfo(ps[i], CL_PLATFORM_VERSION, ver, sizeof ver);
      pinfo(ps[i], CL_PLATFORM_EXTENSIONS, ext, sizeof ext);
      printf("platform %u: %s | %s | %s\n", i, name, vendor, ver);
      printf("  extensions: %s\n", ext);
      cl_uint nd = 0;
      if (clGetDeviceIDs(ps[i], CL_DEVICE_TYPE_ALL, 0, NULL, &nd) != CL_SUCCESS)
         nd = 0;
      printf("  devices: %u\n", nd);
      if (!nd)
         continue;
      cl_device_id *ds = calloc(nd, sizeof *ds);
      CHECK(clGetDeviceIDs(ps[i], CL_DEVICE_TYPE_ALL, nd, ds, NULL));
      for (cl_uint j = 0; j < nd; j++) {
         char dn[256], dv[256], cv[256], drv[256], dext[8192];
         cl_device_type t = 0;
         clGetDeviceInfo(ds[j], CL_DEVICE_TYPE, sizeof t, &t, NULL);
         dinfo_str(ds[j], CL_DEVICE_NAME, dn, sizeof dn);
         dinfo_str(ds[j], CL_DEVICE_VERSION, dv, sizeof dv);
         dinfo_str(ds[j], CL_DEVICE_OPENCL_C_VERSION, cv, sizeof cv);
         dinfo_str(ds[j], CL_DRIVER_VERSION, drv, sizeof drv);
         dinfo_str(ds[j], CL_DEVICE_EXTENSIONS, dext, sizeof dext);
         printf("  device %u: %s (%s)\n", j, dn,
                t & CL_DEVICE_TYPE_GPU ? "GPU" : t & CL_DEVICE_TYPE_CPU ? "CPU" : "other");
         printf("    version: %s | %s | driver %s\n", dv, cv, drv);
         printf("    compute units %u, max clock %u MHz, max work-group %zu\n",
                dinfo_uint(ds[j], CL_DEVICE_MAX_COMPUTE_UNITS),
                dinfo_uint(ds[j], CL_DEVICE_MAX_CLOCK_FREQUENCY),
                dinfo_size(ds[j], CL_DEVICE_MAX_WORK_GROUP_SIZE));
         printf("    global memory %llu MiB, max allocation %llu MiB, local memory %llu KiB\n",
                (unsigned long long)(dinfo_ulong(ds[j], CL_DEVICE_GLOBAL_MEM_SIZE) >> 20),
                (unsigned long long)(dinfo_ulong(ds[j], CL_DEVICE_MAX_MEM_ALLOC_SIZE) >> 20),
                (unsigned long long)(dinfo_ulong(ds[j], CL_DEVICE_LOCAL_MEM_SIZE) >> 10));
         printf("    compiler available %u, images %u, fp64 config 0x%llx, fp16 config 0x%llx\n",
                dinfo_uint(ds[j], CL_DEVICE_COMPILER_AVAILABLE),
                dinfo_uint(ds[j], CL_DEVICE_IMAGE_SUPPORT),
                (unsigned long long)dinfo_ulong(ds[j], CL_DEVICE_DOUBLE_FP_CONFIG),
                (unsigned long long)dinfo_ulong(ds[j], CL_DEVICE_HALF_FP_CONFIG));
         printf("    extensions: %s\n", dext);
      }
      free(ds);
   }
   free(ps);
   return 0;
}

/* The device the tests run on, by --platform and --device. */
static int
pick(cl_platform_id *pp, cl_device_id *dp)
{
   cl_uint np = 0;
   if (clGetPlatformIDs(0, NULL, &np) != CL_SUCCESS || np == 0) {
      fprintf(stderr, "FAIL no OpenCL platform\n");
      return 3;
   }
   cl_platform_id *ps = calloc(np, sizeof *ps);
   clGetPlatformIDs(np, ps, NULL);
   for (cl_uint i = 0; i < np; i++) {
      char name[256], vendor[256];
      pinfo(ps[i], CL_PLATFORM_NAME, name, sizeof name);
      pinfo(ps[i], CL_PLATFORM_VENDOR, vendor, sizeof vendor);
      if (opt_platform && !strstr(name, opt_platform) && !strstr(vendor, opt_platform))
         continue;
      cl_uint nd = 0;
      if (clGetDeviceIDs(ps[i], CL_DEVICE_TYPE_ALL, 0, NULL, &nd) != CL_SUCCESS || !nd)
         continue;
      cl_device_id *ds = calloc(nd, sizeof *ds);
      clGetDeviceIDs(ps[i], CL_DEVICE_TYPE_ALL, nd, ds, NULL);
      for (cl_uint j = 0; j < nd; j++) {
         char dn[256];
         dinfo_str(ds[j], CL_DEVICE_NAME, dn, sizeof dn);
         if (opt_device && !strstr(dn, opt_device))
            continue;
         if (opt_expect && !strstr(dn, opt_expect)) {
            fprintf(stderr, "FAIL device is \"%s\", expected \"%s\"\n", dn, opt_expect);
            free(ds);
            free(ps);
            return 3;
         }
         printf("device: %s (platform %s)\n", dn, name);
         *pp = ps[i];
         *dp = ds[j];
         free(ds);
         free(ps);
         return 0;
      }
      free(ds);
   }
   free(ps);
   fprintf(stderr, "FAIL no device matches\n");
   return 3;
}

static int
build(cl_context ctx, cl_device_id dev, const char *src, cl_program *out)
{
   cl_int e;
   double t0 = now_ms();
   cl_program p = clCreateProgramWithSource(ctx, 1, &src, NULL, &e);
   CHECK(e);
   e = clBuildProgram(p, 1, &dev, "-cl-std=CL1.2", NULL, NULL);
   size_t n = 0;
   clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &n);
   if (n > 2) {
      char *log = malloc(n + 1);
      clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, n, log, NULL);
      log[n] = 0;
      fprintf(stderr, "build log:\n%s\n", log);
      free(log);
   }
   CHECK(e);
   printf("build: %.1f ms\n", now_ms() - t0);
   *out = p;
   return 0;
}

static double
event_ms(cl_event ev)
{
   cl_ulong a = 0, b = 0;
   if (clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof a, &a, NULL) != CL_SUCCESS ||
       clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof b, &b, NULL) != CL_SUCCESS ||
       b < a)
      return -1.0;
   return (double)(b - a) / 1e6;
}

static const char *vecadd_src =
   "__kernel void vecadd(__global const float *a, __global const float *b,\n"
   "                     __global float *c, uint n)\n"
   "{\n"
   "   uint i = get_global_id(0);\n"
   "   if (i < n)\n"
   "      c[i] = a[i] + b[i];\n"
   "}\n";

static int
cmd_vecadd(size_t n)
{
   cl_platform_id plat;
   cl_device_id dev;
   int r = pick(&plat, &dev);
   if (r)
      return r;

   cl_int e;
   cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &e);
   CHECK(e);
   cl_command_queue q = clCreateCommandQueue(ctx, dev, CL_QUEUE_PROFILING_ENABLE, &e);
   CHECK(e);
   cl_program p;
   if ((r = build(ctx, dev, vecadd_src, &p)))
      return r;
   cl_kernel k = clCreateKernel(p, "vecadd", &e);
   CHECK(e);

   float *a = malloc(n * sizeof(float)), *b = malloc(n * sizeof(float)), *c = malloc(n * sizeof(float));
   for (size_t i = 0; i < n; i++) {
      a[i] = (float)(i % 1000) * 0.5f;
      b[i] = (float)(i % 777) * 0.25f + 1.0f;
      c[i] = -1.0f;
   }
   cl_mem ba = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(float), a, &e);
   CHECK(e);
   cl_mem bb = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n * sizeof(float), b, &e);
   CHECK(e);
   cl_mem bc = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, n * sizeof(float), NULL, &e);
   CHECK(e);
   cl_uint un = (cl_uint)n;
   CHECK(clSetKernelArg(k, 0, sizeof ba, &ba));
   CHECK(clSetKernelArg(k, 1, sizeof bb, &bb));
   CHECK(clSetKernelArg(k, 2, sizeof bc, &bc));
   CHECK(clSetKernelArg(k, 3, sizeof un, &un));
   size_t local = 64, global = (n + local - 1) / local * local;
   cl_event ev;
   double t0 = now_ms();
   CHECK(clEnqueueNDRangeKernel(q, k, 1, NULL, &global, &local, 0, NULL, &ev));
   CHECK(clFinish(q));
   double wall = now_ms() - t0;
   CHECK(clEnqueueReadBuffer(q, bc, CL_TRUE, 0, n * sizeof(float), c, 0, NULL, NULL));
   double gpu = event_ms(ev);
   printf("vecadd n=%zu: kernel %.3f ms (events), %.3f ms (wall, enqueue to finish), %.2f GB/s\n",
          n, gpu, wall, gpu > 0 ? 3.0 * n * sizeof(float) / (gpu * 1e6) : 0.0);

   size_t bad = 0;
   if (opt_verify) {
      for (size_t i = 0; i < n; i++) {
         if (c[i] != a[i] + b[i]) {
            if (bad < 5)
               fprintf(stderr, "  c[%zu] = %g, expected %g\n", i, c[i], a[i] + b[i]);
            bad++;
         }
      }
      printf("vecadd: %s (%zu of %zu wrong)\n", bad ? "FAIL" : "PASS", bad, n);
   } else {
      printf("vecadd: not verified (--no-verify)\n");
   }
   clReleaseEvent(ev);
   clReleaseMemObject(ba);
   clReleaseMemObject(bb);
   clReleaseMemObject(bc);
   clReleaseKernel(k);
   clReleaseProgram(p);
   clReleaseCommandQueue(q);
   clReleaseContext(ctx);
   free(a);
   free(b);
   free(c);
   return bad ? 1 : 0;
}

/* Row-major, C = A * B, all n x n. The tiled kernel needs n % 16 == 0. */
static const char *sgemm_src =
   "__kernel void sgemm_naive(__global const float *A, __global const float *B,\n"
   "                          __global float *C, uint n)\n"
   "{\n"
   "   uint col = get_global_id(0), row = get_global_id(1);\n"
   "   float acc = 0.0f;\n"
   "   for (uint k = 0; k < n; k++)\n"
   "      acc += A[row * n + k] * B[k * n + col];\n"
   "   C[row * n + col] = acc;\n"
   "}\n"
   "#define TS 16\n"
   "__kernel __attribute__((reqd_work_group_size(TS, TS, 1)))\n"
   "void sgemm_tiled(__global const float *A, __global const float *B,\n"
   "                 __global float *C, uint n)\n"
   "{\n"
   "   __local float As[TS][TS];\n"
   "   __local float Bs[TS][TS];\n"
   "   uint tx = get_local_id(0), ty = get_local_id(1);\n"
   "   uint col = get_global_id(0), row = get_global_id(1);\n"
   "   float acc = 0.0f;\n"
   "   for (uint t = 0; t < n; t += TS) {\n"
   "      As[ty][tx] = A[row * n + t + tx];\n"
   "      Bs[ty][tx] = B[(t + ty) * n + col];\n"
   "      barrier(CLK_LOCAL_MEM_FENCE);\n"
   "      for (uint k = 0; k < TS; k++)\n"
   "         acc += As[ty][k] * Bs[k][tx];\n"
   "      barrier(CLK_LOCAL_MEM_FENCE);\n"
   "   }\n"
   "   C[row * n + col] = acc;\n"
   "}\n";

static int
cmd_sgemm(size_t n, int iters)
{
   if (n % 16) {
      fprintf(stderr, "sgemm: n must be a multiple of 16\n");
      return 2;
   }
   cl_platform_id plat;
   cl_device_id dev;
   int r = pick(&plat, &dev);
   if (r)
      return r;

   cl_int e;
   cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &e);
   CHECK(e);
   cl_command_queue q = clCreateCommandQueue(ctx, dev, CL_QUEUE_PROFILING_ENABLE, &e);
   CHECK(e);
   cl_program p;
   if ((r = build(ctx, dev, sgemm_src, &p)))
      return r;

   size_t nn = n * n;
   float *A = malloc(nn * sizeof(float)), *B = malloc(nn * sizeof(float)), *C = malloc(nn * sizeof(float));
   double *ref = malloc(nn * sizeof(double));
   srand(1234);
   for (size_t i = 0; i < nn; i++) {
      A[i] = (float)(rand() % 2001 - 1000) / 1000.0f;
      B[i] = (float)(rand() % 2001 - 1000) / 1000.0f;
   }
   if (opt_verify) {
      for (size_t i = 0; i < n; i++)
         for (size_t j = 0; j < n; j++) {
            double acc = 0.0;
            for (size_t k = 0; k < n; k++)
               acc += (double)A[i * n + k] * (double)B[k * n + j];
            ref[i * n + j] = acc;
         }
   }
   cl_mem bA = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, nn * sizeof(float), A, &e);
   CHECK(e);
   cl_mem bB = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, nn * sizeof(float), B, &e);
   CHECK(e);
   cl_mem bC = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, nn * sizeof(float), NULL, &e);
   CHECK(e);

   static const char *names[2] = { "sgemm_naive", "sgemm_tiled" };
   int fail = 0;
   for (int v = 0; v < 2; v++) {
      cl_kernel k = clCreateKernel(p, names[v], &e);
      CHECK(e);
      cl_uint un = (cl_uint)n;
      CHECK(clSetKernelArg(k, 0, sizeof bA, &bA));
      CHECK(clSetKernelArg(k, 1, sizeof bB, &bB));
      CHECK(clSetKernelArg(k, 2, sizeof bC, &bC));
      CHECK(clSetKernelArg(k, 3, sizeof un, &un));
      size_t global[2] = { n, n }, local[2] = { 16, 16 };
      double best = -1.0, total = 0.0, wall0 = now_ms();
      for (int it = 0; it < iters; it++) {
         cl_event ev;
         CHECK(clEnqueueNDRangeKernel(q, k, 2, NULL, global, local, 0, NULL, &ev));
         CHECK(clWaitForEvents(1, &ev));
         double ms = event_ms(ev);
         if (ms > 0 && (best < 0 || ms < best))
            best = ms;
         total += ms > 0 ? ms : 0;
         clReleaseEvent(ev);
      }
      double wall = (now_ms() - wall0) / iters;
      CHECK(clEnqueueReadBuffer(q, bC, CL_TRUE, 0, nn * sizeof(float), C, 0, NULL, NULL));
      double flop = 2.0 * (double)n * n * n;
      printf("%s n=%zu: best %.3f ms, mean %.3f ms (events), %.3f ms (wall), %.2f GFLOP/s (best)\n",
             names[v], n, best, total / iters, wall, best > 0 ? flop / (best * 1e6) : 0.0);
      if (opt_verify) {
         size_t bad = 0;
         double maxerr = 0.0;
         for (size_t i = 0; i < nn; i++) {
            double err = fabs((double)C[i] - ref[i]);
            /* fp32 accumulation of n products of magnitude <= 1 */
            double tol = 1e-5 * (double)n + 1e-4;
            if (err > maxerr)
               maxerr = err;
            if (!(err <= tol)) {
               if (bad < 5)
                  fprintf(stderr, "  C[%zu] = %g, expected %g\n", i, C[i], ref[i]);
               bad++;
            }
         }
         printf("%s: %s (%zu wrong, max error %.3g)\n", names[v], bad ? "FAIL" : "PASS", bad, maxerr);
         fail |= bad != 0;
      }
      memset(C, 0, nn * sizeof(float));
      clReleaseKernel(k);
   }
   if (!opt_verify)
      printf("sgemm: not verified (--no-verify)\n");
   clReleaseMemObject(bA);
   clReleaseMemObject(bB);
   clReleaseMemObject(bC);
   clReleaseProgram(p);
   clReleaseCommandQueue(q);
   clReleaseContext(ctx);
   free(A);
   free(B);
   free(C);
   free(ref);
   return fail;
}

static int
usage(void)
{
   fprintf(stderr,
           "usage: cltest [--platform S] [--device S] [--expect S] [--no-verify]\n"
           "              list | vecadd [n] | sgemm [n] [iters] | all\n");
   return 2;
}

int
main(int argc, char **argv)
{
   int i = 1;
   setvbuf(stdout, NULL, _IONBF, 0);
   for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
      if (!strcmp(argv[i], "--platform") && i + 1 < argc)
         opt_platform = argv[++i];
      else if (!strcmp(argv[i], "--device") && i + 1 < argc)
         opt_device = argv[++i];
      else if (!strcmp(argv[i], "--expect") && i + 1 < argc)
         opt_expect = argv[++i];
      else if (!strcmp(argv[i], "--no-verify"))
         opt_verify = 0;
      else
         return usage();
   }
   if (i >= argc)
      return usage();
   const char *cmd = argv[i++];
   if (!strcmp(cmd, "list"))
      return cmd_list();
   if (!strcmp(cmd, "vecadd"))
      return cmd_vecadd(i < argc ? (size_t)strtoull(argv[i], NULL, 0) : 1u << 20);
   if (!strcmp(cmd, "sgemm"))
      return cmd_sgemm(i < argc ? (size_t)strtoull(argv[i], NULL, 0) : 256,
                       i + 1 < argc ? atoi(argv[i + 1]) : 5);
   if (!strcmp(cmd, "all")) {
      int r = cmd_list();
      if (r)
         return r;
      if ((r = cmd_vecadd(1u << 20)))
         return r;
      return cmd_sgemm(256, 5);
   }
   return usage();
}
