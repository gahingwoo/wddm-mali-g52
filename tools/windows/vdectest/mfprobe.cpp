/*
 * mfprobe - Media Foundation's H.264 decoder (CLSID_CMSH264DecoderMFT) on
 * this D3D11 device, as a browser or a player hands it one (vdectest --mf):
 * the DXGI device manager, MFT_MESSAGE_SET_D3D_MANAGER, a 1080p H.264
 * input type and an NV12 output type. Edge decodes HEVC through Media
 * Foundation (MediaFoundationVideoDecoder) and may do so for H.264; when
 * the decoder takes the device it creates its D3D11 video decoder here,
 * which the UMD log shows. Each step is printed; exit 0 when the MFT
 * accepted the device and both types, 4 otherwise.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <windows.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <stdio.h>

#include "mfprobe.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static HRESULT
set_type(IMFTransform *mft, bool input, const GUID &sub, UINT w, UINT h, DWORD index, HRESULT *got)
{
   IMFMediaType *t = NULL;
   HRESULT hr = MFCreateMediaType(&t);
   if (FAILED(hr))
      return hr;
   t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
   t->SetGUID(MF_MT_SUBTYPE, sub);
   MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
   MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30, 1);
   MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
   t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
   if (input)
      t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
   hr = input ? mft->SetInputType(index, t, 0) : mft->SetOutputType(index, t, 0);
   t->Release();
   *got = hr;
   return hr;
}

int
mf_probe(ID3D11Device *dev)
{
   IMFDXGIDeviceManager *mgr = NULL;
   IMFTransform *mft = NULL;
   IMFAttributes *attr = NULL;
   ID3D10Multithread *mt = NULL;
   UINT token = 0, aware = 0;
   HRESULT hr, in_hr = E_FAIL, out_hr = E_FAIL, mgr_hr = E_FAIL;
   int rc = 4;

   printf("media foundation H.264 decoder probe:\n");
   CoInitializeEx(NULL, COINIT_MULTITHREADED);
   hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
   printf("  MFStartup: 0x%08lx\n", (unsigned long)hr);
   if (FAILED(hr))
      return 4;
   if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt))) {
      mt->SetMultithreadProtected(TRUE);
      mt->Release();
   }
   hr = MFCreateDXGIDeviceManager(&token, &mgr);
   if (SUCCEEDED(hr))
      hr = mgr->ResetDevice(dev, token);
   printf("  DXGI device manager with this device: 0x%08lx\n", (unsigned long)hr);
   if (FAILED(hr))
      goto out;
   hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
   printf("  CLSID_CMSH264DecoderMFT: 0x%08lx\n", (unsigned long)hr);
   if (FAILED(hr))
      goto out;
   if (SUCCEEDED(mft->GetAttributes(&attr))) {
      attr->GetUINT32(MF_SA_D3D11_AWARE, &aware);
      printf("  MF_SA_D3D11_AWARE: %u\n", aware);
      attr->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
   }
   mgr_hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr);
   printf("  MFT_MESSAGE_SET_D3D_MANAGER: 0x%08lx%s\n", (unsigned long)mgr_hr,
          mgr_hr == MF_E_UNSUPPORTED_D3D_TYPE ? " (MF_E_UNSUPPORTED_D3D_TYPE: no usable DXVA H.264 here)" : "");
   set_type(mft, true, MFVideoFormat_H264, 1920, 1080, 0, &in_hr);
   printf("  input H.264 High 1920x1080: 0x%08lx\n", (unsigned long)in_hr);
   for (DWORD i = 0; SUCCEEDED(in_hr); i++) {
      IMFMediaType *t = NULL;
      GUID sub;
      if (FAILED(mft->GetOutputAvailableType(0, i, &t)))
         break;
      if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && IsEqualGUID(sub, MFVideoFormat_NV12)) {
         out_hr = mft->SetOutputType(0, t, 0);
         t->Release();
         break;
      }
      t->Release();
   }
   printf("  output NV12: 0x%08lx\n", (unsigned long)out_hr);
   if (attr) {
      UINT32 accel = 0;
      HRESULT ah = attr->GetUINT32(CODECAPI_AVDecVideoAcceleration_H264, &accel);
      printf("  CODECAPI_AVDecVideoAcceleration_H264 after the types: %u (0x%08lx)\n", accel, (unsigned long)ah);
   }
   if (SUCCEEDED(out_hr)) {
      hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      printf("  MFT_MESSAGE_NOTIFY_BEGIN_STREAMING: 0x%08lx\n", (unsigned long)hr);
   }
   rc = SUCCEEDED(mgr_hr) && SUCCEEDED(in_hr) && SUCCEEDED(out_hr) ? 0 : 4;
out:
   printf("media foundation: %s\n", rc ? "the H.264 decoder did not take this device and types"
                                       : "the H.264 decoder took this device and types");
   if (attr) attr->Release();
   if (mft) mft->Release();
   if (mgr) mgr->Release();
   MFShutdown();
   return rc;
}
